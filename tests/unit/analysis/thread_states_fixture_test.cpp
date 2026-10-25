// buildThreadStates against the tracegen fixtures: exact per-thread state totals, exact interval lists where
// expected.json has them, structural invariants, and determinism across worker counts.
#include "analysis/thread_states.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using model::ThreadStateInterval;
using model::TraceModel;

const char* const kFixtures[] = {"tiny",   "medium",    "scopes",  "fibers",
                                 "frames", "recursion", "partial", "partial_torn"};

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

nlohmann::json readJson(const std::string& file) {
  std::ifstream f(generated(file));
  return nlohmann::json::parse(f);
}

const char* stateName(ThreadState s) {
  switch (s) {
    case ThreadState::Running: return "running";
    case ThreadState::Ready: return "ready";
    case ThreadState::Waiting: return "waiting";
    case ThreadState::Unknown: return "unknown";
  }
  return "?";
}

// Resolves "module!function" names of the fake symbol tables written next to a fixture.
class Symbols {
 public:
  explicit Symbols(const std::string& fixture) : m_json(readJson(fixture + ".symbols.json")) {}

  std::string name(const TraceModel& m, FrameIdx f) const {
    const model::Frame& fr = m.frames[f.get()];
    if (!fr.module.valid()) return "abs";
    const auto& mod = m_json["modules"][fr.module.get()];
    for (const auto& fn : mod["functions"]) {
      const uint64_t s = fn["start_rva"].get<uint64_t>();
      if (fr.addr_or_rva >= s && fr.addr_or_rva < s + fn["size"].get<uint64_t>())
        return mod["name"].get<std::string>() + "!" + fn["name"].get<std::string>();
    }
    return "?";
  }

 private:
  nlohmann::json m_json;
};

// Leaf-first frame names of a callstack (the order used by expected.json).
std::vector<std::string> stackNames(const TraceModel& m, const Symbols& sym, CallstackIdx s) {
  std::vector<std::string> out;
  uint32_t i = s.valid() ? s.get() : 0;
  while (i != 0 && i < m.nodes.size()) {
    out.push_back(sym.name(m, m.nodes[i].frame));
    i = m.nodes[i].parent.get();
  }
  return out;
}

void expectInvariants(const TraceModel& m) {
  ASSERT_EQ(m.statesByThread.size(), m.threads.size());
  for (size_t t = 0; t < m.threads.size(); ++t) {
    const auto& v = m.statesByThread[t];
    ASSERT_FALSE(v.empty()) << "tid " << m.threads[t].tid;
    EXPECT_EQ(v.front().start, m.header.start_ts);
    EXPECT_EQ(v.back().end, m.header.end_ts);
    for (size_t i = 0; i < v.size(); ++i) {
      EXPECT_LT(v[i].start, v[i].end);
      if (i) EXPECT_EQ(v[i - 1].end, v[i].start);
    }
  }
}

class ThreadStatesFixture : public ::testing::TestWithParam<const char*> {};

TEST_P(ThreadStatesFixture, TotalsMatchExpected) {
  const std::string name = GetParam();
  auto expected = readJson(name + ".expected.json");
  auto loaded = TraceModel::load(generated(name + ".tachyon"));
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  TraceModel& m = *loaded;
  ASSERT_TRUE(buildThreadStates(m).ok());
  expectInvariants(m);

  for (const auto& et : expected["threads"]) {
    const uint32_t tid = et["tid"].get<uint32_t>();
    const auto it = m.tidToThread.find(tid);
    ASSERT_NE(it, m.tidToThread.end()) << tid;
    std::map<std::string, uint64_t> got;
    for (const ThreadStateInterval& iv : m.statesByThread[it->second.get()])
      got[stateName(iv.state)] += iv.end - iv.start;
    for (const char* s : {"running", "ready", "waiting", "unknown"}) {
      const uint64_t want = et["state_ticks"].contains(s) ? et["state_ticks"][s].get<uint64_t>() : 0;
      EXPECT_EQ(got[s], want) << name << " tid " << tid << " " << s;
    }
  }
}

TEST_P(ThreadStatesFixture, IntervalListsMatchExpectedWhenPresent) {
  const std::string name = GetParam();
  auto expected = readJson(name + ".expected.json");
  auto loaded = TraceModel::load(generated(name + ".tachyon"));
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  TraceModel& m = *loaded;
  ASSERT_TRUE(buildThreadStates(m).ok());
  const Symbols sym(name);

  size_t listsChecked = 0;
  for (const auto& et : expected["threads"]) {
    if (!et.contains("intervals")) continue;
    ++listsChecked;
    const uint32_t tid = et["tid"].get<uint32_t>();
    const auto& got = m.statesByThread[m.tidToThread.at(tid).get()];
    const auto& want = et["intervals"];
    ASSERT_EQ(got.size(), want.size()) << name << " tid " << tid;
    for (size_t i = 0; i < got.size(); ++i) {
      const auto& w = want[i];
      SCOPED_TRACE(name + " tid " + std::to_string(tid) + " interval " + std::to_string(i));
      EXPECT_EQ(got[i].start, w["start"].get<uint64_t>());
      EXPECT_EQ(got[i].end, w["end"].get<uint64_t>());
      EXPECT_STREQ(stateName(got[i].state), w["state"].get<std::string>().c_str());
      EXPECT_EQ(got[i].woke_by_tid, w.contains("woke_by_tid") ? w["woke_by_tid"].get<uint32_t>() : 0u);
      for (const char* key : {"blocking_stack", "unblocking_stack"}) {
        const CallstackIdx s =
            std::string(key) == "blocking_stack" ? got[i].blocking_stack : got[i].unblocking_stack;
        const std::vector<std::string> names = stackNames(m, sym, s);
        if (w.contains(key)) {
          const auto& ws = w[key];
          ASSERT_EQ(names.size(), ws.size()) << key;
          for (size_t k = 0; k < names.size(); ++k)
            if (names[k] != "abs") EXPECT_EQ(names[k], ws[k].get<std::string>()) << key << " frame " << k;
        } else {
          EXPECT_TRUE(names.empty()) << key;
        }
      }
      if (got[i].state == ThreadState::Running) {
        EXPECT_FALSE(got[i].blocking_stack.valid());
      }
    }
  }
  if (name == "tiny") EXPECT_EQ(listsChecked, 2u);
}

TEST_P(ThreadStatesFixture, DeterministicAcrossThreadCounts) {
  const std::string name = GetParam();
  auto a = TraceModel::load(generated(name + ".tachyon"));
  auto b = TraceModel::load(generated(name + ".tachyon"));
  ASSERT_TRUE(a.ok() && b.ok());
  BuildOptions one;
  one.threads = 1;
  BuildOptions many;
  many.threads = 7;
  ASSERT_TRUE(buildThreadStates(*a, one).ok());
  ASSERT_TRUE(buildThreadStates(*b, many).ok());
  ASSERT_EQ(a->statesByThread.size(), b->statesByThread.size());
  for (size_t t = 0; t < a->statesByThread.size(); ++t) {
    const auto& x = a->statesByThread[t];
    const auto& y = b->statesByThread[t];
    ASSERT_EQ(x.size(), y.size());
    for (size_t i = 0; i < x.size(); ++i) {
      EXPECT_EQ(x[i].start, y[i].start);
      EXPECT_EQ(x[i].end, y[i].end);
      EXPECT_EQ(x[i].state, y[i].state);
      EXPECT_EQ(x[i].wait_reason, y[i].wait_reason);
      EXPECT_EQ(x[i].cpu, y[i].cpu);
      EXPECT_EQ(x[i].blocking_stack, y[i].blocking_stack);
      EXPECT_EQ(x[i].unblocking_stack, y[i].unblocking_stack);
      EXPECT_EQ(x[i].woke_by_tid, y[i].woke_by_tid);
      EXPECT_EQ(x[i].ready_ts, y[i].ready_ts);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ThreadStatesFixture, ::testing::ValuesIn(kFixtures),
                         [](const auto& info) { return std::string(info.param); });

TEST(ThreadStatesTiny, SpotChecksOfRunningCpuAndReadySplit) {
  auto loaded = TraceModel::load(generated("tiny.tachyon"));
  ASSERT_TRUE(loaded.ok());
  TraceModel& m = *loaded;
  ASSERT_TRUE(buildThreadStates(m).ok());
  const ThreadIdx t101 = m.tidToThread.at(101);
  const auto& v = m.statesByThread[t101.get()];
  ASSERT_GE(v.size(), 5u);
  // 101: Unknown prefix (running at trace start, no switch-in), then the Ready-split wait.
  EXPECT_EQ(v[0].state, ThreadState::Unknown);
  EXPECT_EQ(v[1].state, ThreadState::Waiting);
  EXPECT_EQ(v[1].start, 1'400'000u);
  EXPECT_EQ(v[1].end, 1'475'000u);
  EXPECT_EQ(v[1].woke_by_tid, 102u);
  EXPECT_EQ(v[1].ready_ts, 1'475'000u);
  EXPECT_EQ(v[2].state, ThreadState::Ready);
  EXPECT_EQ(v[2].ready_ts, 1'475'000u);
  EXPECT_EQ(v[2].blocking_stack, v[1].blocking_stack);
  EXPECT_EQ(v[2].unblocking_stack, v[1].unblocking_stack);
  EXPECT_EQ(v[3].state, ThreadState::Running);
  EXPECT_EQ(v[3].cpu, 0u);  // thread 101 runs on cpu 0 (scenario index % 8)
  // stateAt agrees with the list.
  EXPECT_EQ(stateAt(m, t101, 1'474'999)->state, ThreadState::Waiting);
  EXPECT_EQ(stateAt(m, t101, 1'475'000)->state, ThreadState::Ready);
  EXPECT_EQ(stateAt(m, t101, 1'480'000)->state, ThreadState::Running);
  EXPECT_EQ(stateAt(m, t101, m.header.start_ts)->state, ThreadState::Unknown);
  EXPECT_EQ(stateAt(m, t101, m.header.end_ts), nullptr);
  EXPECT_EQ(stateAt(m, t101, m.header.start_ts - 1), nullptr);
  EXPECT_EQ(stateAt(m, ThreadIdx::invalid(), 1'400'000), nullptr);
}

}  // namespace
}  // namespace tachyon::analysis
