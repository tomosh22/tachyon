// Loads every fixture and checks the model against the tracegen expected.json.
#include "model/model.h"
#include "model_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

namespace tachyon::model {
namespace {

using testing_util::kAllFixtures;
using testing_util::loadExpected;

class ModelLoad : public ::testing::TestWithParam<const char*> {};

TEST_P(ModelLoad, CountsMatchExpected) {
  const std::string name = GetParam();
  const auto expected = loadExpected(name);
  auto r = TraceModel::load(testing_util::fixturePath(name));
  ASSERT_TRUE(r.ok()) << r.error().toString();
  const TraceModel& m = *r;
  const auto& counts = expected["counts"];

  EXPECT_EQ(m.isPartial, expected["trace"]["partial"].get<bool>());
  EXPECT_EQ(m.header.start_ts, expected["trace"]["start_ts"].get<uint64_t>());
  EXPECT_EQ(m.header.end_ts, expected["trace"]["end_ts"].get<uint64_t>());
  EXPECT_EQ(m.header.clock_frequency_hz, expected["clock_frequency_hz"].get<uint64_t>());
  EXPECT_EQ(m.sampleIntervalTicks, expected["sample_interval_ticks"].get<uint64_t>());
  EXPECT_EQ(m.captureInfo.sampling_hz, expected["sampling_hz"].get<uint64_t>());
  EXPECT_FALSE(m.captureInfo.raw_json.empty());

  // Threads and per-thread sample counts.
  const auto& threads = expected["threads"];
  ASSERT_EQ(m.threads.size(), threads.size());
  uint64_t totalSamples = 0;
  for (const auto& t : threads) {
    const uint32_t tid = t["tid"].get<uint32_t>();
    auto it = m.tidToThread.find(tid);
    ASSERT_NE(it, m.tidToThread.end()) << "tid " << tid;
    EXPECT_EQ(m.samplesByThread[it->second.get()].ts.size(), t["samples"].get<uint64_t>()) << "tid " << tid;
    totalSamples += m.samplesByThread[it->second.get()].ts.size();
  }
  EXPECT_EQ(totalSamples, counts["samples"].get<uint64_t>());

  // Event streams.
  EXPECT_EQ(m.contextSwitches.size(), counts["context_switches"].get<size_t>());
  EXPECT_EQ(m.readyEvents.size(), counts["ready_events"].get<size_t>());
  EXPECT_EQ(m.scopeEvents.size(), counts["scope_events"].get<size_t>());
  EXPECT_EQ(m.fiberSwitches.size(), counts["fiber_events"].get<size_t>());
  EXPECT_EQ(m.frameMarks.size(), counts["frame_marks"].get<size_t>());
  EXPECT_EQ(m.markers.size(), counts["markers"].get<size_t>());
  size_t plotSamples = 0;
  for (const PlotSeries& p : m.plots) {
    EXPECT_EQ(p.ts.size(), p.value.size());
    plotSamples += p.ts.size();
  }
  EXPECT_EQ(plotSamples, counts["plot_samples"].get<size_t>());

  // Plot series by name.
  const auto& plots = expected["plots"];
  ASSERT_EQ(m.plots.size(), plots.size());
  for (const auto& ep : plots) {
    const std::string pname = ep["name"].get<std::string>();
    auto it = std::find_if(m.plots.begin(), m.plots.end(),
                           [&](const PlotSeries& p) { return m.strings.get(p.name) == pname; });
    ASSERT_NE(it, m.plots.end()) << pname;
    EXPECT_EQ(it->ts.size(), ep["count"].get<size_t>());
    EXPECT_EQ(it->ts.front(), ep["first_ts"].get<uint64_t>());
    EXPECT_EQ(it->ts.back(), ep["last_ts"].get<uint64_t>());
    EXPECT_EQ(*std::min_element(it->value.begin(), it->value.end()), ep["min"].get<double>());
    EXPECT_EQ(*std::max_element(it->value.begin(), it->value.end()), ep["max"].get<double>());
  }

  // Modules in ModuleIdx order.
  const auto& modules = expected["modules"];
  ASSERT_EQ(m.modules.size(), modules.size());
  for (const auto& em : modules) {
    const Module& mod = m.modules[em["index"].get<size_t>()];
    EXPECT_EQ(mod.base, em["base"].get<uint64_t>());
    EXPECT_NE(mod.path.find(em["name"].get<std::string>()), std::string::npos);
    EXPECT_EQ(mod.state, SymbolState::Pending);
    EXPECT_NE(mod.id.kind, 0);
  }
}

TEST_P(ModelLoad, StructuralInvariants) {
  const std::string name = GetParam();
  auto r = TraceModel::load(testing_util::fixturePath(name));
  ASSERT_TRUE(r.ok()) << r.error().toString();
  const TraceModel& m = *r;

  // tidToThread is consistent with the thread table.
  EXPECT_EQ(m.tidToThread.size(), m.threads.size());
  for (size_t i = 0; i < m.threads.size(); ++i) {
    auto it = m.tidToThread.find(m.threads[i].tid);
    ASSERT_NE(it, m.tidToThread.end());
    EXPECT_EQ(it->second.get(), i);
  }
  for (const auto& [tid, idx] : m.tidToThread) {
    ASSERT_LT(idx.get(), m.threads.size());
    EXPECT_EQ(m.threads[idx.get()].tid, tid);
  }

  // Per-thread columns are parallel, sorted and refer to valid nodes.
  ASSERT_EQ(m.samplesByThread.size(), m.threads.size());
  for (const SampleColumns& s : m.samplesByThread) {
    ASSERT_EQ(s.callstack.size(), s.ts.size());
    ASSERT_EQ(s.cpu.size(), s.ts.size());
    ASSERT_EQ(s.flags.size(), s.ts.size());
    EXPECT_TRUE(std::is_sorted(s.ts.begin(), s.ts.end()));
    for (CallstackIdx cs : s.callstack) ASSERT_TRUE(!cs.valid() || cs.get() < m.nodes.size());
  }

  // Reserved for WP-31 / WP-53.
  ASSERT_EQ(m.statesByThread.size(), m.threads.size());
  ASSERT_EQ(m.scopesByThread.size(), m.threads.size());
  for (const auto& v : m.statesByThread) EXPECT_TRUE(v.empty());
  for (const auto& v : m.scopesByThread) EXPECT_TRUE(v.empty());

  // Interned tables.
  ASSERT_FALSE(m.nodes.empty());
  EXPECT_FALSE(m.nodes[0].parent.valid());
  EXPECT_FALSE(m.nodes[0].frame.valid());
  for (size_t i = 1; i < m.nodes.size(); ++i) {
    ASSERT_LT(m.nodes[i].parent.get(), i);
    ASSERT_LT(m.nodes[i].frame.get(), m.frames.size());
  }
  for (const Frame& f : m.frames) ASSERT_TRUE(!f.module.valid() || f.module.get() < m.modules.size());
  EXPECT_EQ(m.strings.get(StringIdx(0)), "");

  // Event streams are sorted.
  EXPECT_TRUE(std::is_sorted(m.contextSwitches.begin(), m.contextSwitches.end(),
                             [](const auto& a, const auto& b) { return a.ts < b.ts; }));
  EXPECT_TRUE(std::is_sorted(m.scopeEvents.begin(), m.scopeEvents.end(),
                             [](const auto& a, const auto& b) { return a.ts < b.ts; }));
  EXPECT_TRUE(std::is_sorted(m.frameMarks.begin(), m.frameMarks.end(),
                             [](const auto& a, const auto& b) { return a.ts < b.ts; }));
  for (const FrameMark& f : m.frameMarks) ASSERT_TRUE(!f.thread.valid() || f.thread.get() < m.threads.size());
  for (const Marker& k : m.markers) ASSERT_TRUE(!k.thread.valid() || k.thread.get() < m.threads.size());
  for (const FiberSwitch& f : m.fiberSwitches)
    ASSERT_TRUE(!f.thread.valid() || f.thread.get() < m.threads.size());

  // Thread time ranges lie within the trace and are ordered.
  for (size_t i = 0; i < m.threads.size(); ++i) {
    const Thread& t = m.threads[i];
    EXPECT_LE(t.first_ts, t.last_ts) << "tid " << t.tid;
  }

  // Memory accounting is positive and at least covers the sample columns (16 bytes per sample).
  size_t samples = 0;
  for (const SampleColumns& s : m.samplesByThread) samples += s.ts.size();
  EXPECT_GE(m.residentBytes(), samples * 16);
  // Vectors were shrunk to fit.
  for (const SampleColumns& s : m.samplesByThread) EXPECT_EQ(s.ts.capacity(), s.ts.size());
  EXPECT_EQ(m.nodes.capacity(), m.nodes.size());
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ModelLoad, ::testing::ValuesIn(kAllFixtures),
                         [](const auto& info) { return std::string(info.param); });

TEST(ModelLoadTiny, ThreadMetadataAndRoles) {
  auto r = TraceModel::load(testing_util::fixturePath("tiny"));
  ASSERT_TRUE(r.ok());
  const TraceModel& m = *r;
  ASSERT_EQ(m.processes.size(), 1u);
  EXPECT_EQ(m.processes[0].name, "tiny.exe");
  const Thread& main = m.threads[m.tidToThread.at(101).get()];
  const Thread& worker = m.threads[m.tidToThread.at(102).get()];
  EXPECT_EQ(main.name, "Main");
  EXPECT_EQ(main.role, ThreadRole::Main);
  EXPECT_TRUE(main.is_target);
  // The ThreadNames chunk renames thread 102 (latest rename wins).
  EXPECT_EQ(worker.name, "Worker-renamed");
  EXPECT_EQ(worker.role, ThreadRole::Worker);
  EXPECT_EQ(m.modules[0].state, SymbolState::Pending);
  EXPECT_EQ(m.sampleIntervalTicks, 1250u);
  // The tid 0 marker ("Shutdown") is global: no thread.
  size_t global = 0;
  for (const Marker& k : m.markers) global += !k.thread.valid();
  EXPECT_EQ(global, 1u);
}

TEST(ModelLoadPartial, RecoveredCountsAndFlag) {
  for (const char* name : {"partial", "partial_torn"}) {
    auto r = TraceModel::load(testing_util::fixturePath(name));
    ASSERT_TRUE(r.ok()) << name << ": " << r.error().toString();
    EXPECT_TRUE(r->isPartial) << name;
    EXPECT_NE(r->header.flags & trace::kFlagPartial, 0u) << name;
  }
}

TEST(ModelThreadRole, Heuristic) {
  EXPECT_EQ(classifyThreadRole(""), ThreadRole::Unknown);
  EXPECT_EQ(classifyThreadRole("MainThread"), ThreadRole::Main);
  EXPECT_EQ(classifyThreadRole("game-main"), ThreadRole::Main);
  EXPECT_EQ(classifyThreadRole("RenderThread"), ThreadRole::Render);
  EXPECT_EQ(classifyThreadRole("Worker 3"), ThreadRole::Worker);
  EXPECT_EQ(classifyThreadRole("JobSystem-7"), ThreadRole::Worker);
  EXPECT_EQ(classifyThreadRole("Audio"), ThreadRole::Other);
}

}  // namespace
}  // namespace tachyon::model
