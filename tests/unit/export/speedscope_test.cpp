// WP-63: speedscope exporter. Hand-built models pin the exact output; tracegen fixtures check the
// invariants (schema, weights against the aggregation, the return-address rule against frameKeyAt).
#include "aggregation_test_util.h"
#include "analysis/attribution.h"
#include "export_test_util.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <sstream>

namespace tachyon::exporters {
namespace {

using analysis::Filter;
using analysis::QueryContext;
using analysis::testutil::HandModel;
using analysis::testutil::TestFunction;
using exporttest::LoadedFixture;
using model::ThreadState;
using nlohmann::json;

const std::vector<TestFunction> kFunctions = {
    {"Main", 0x1000, 0x100}, {"Callee", 0x1100, 0x100}, {"Leaf", 0x1200, 0x100}};

HandModel makeModel(uint32_t threads = 1) {
  HandModel h(threads);
  h.m.header.clock_frequency_hz = 1'000'000'000ull;
  return h;
}

std::vector<std::string> frameNames(const json& doc) {
  std::vector<std::string> out;
  for (const json& f : doc["shared"]["frames"]) out.push_back(f["name"].get<std::string>());
  return out;
}

TEST(Speedscope, HandModelExactOutput) {
  HandModel h = makeModel(2);
  h.m.threads[0].name = "Main";
  // Stack A: Main -> Callee. Stack B: Main, then a return address at the start of Callee (rva - 1 lies in
  // Main), then Leaf.
  const CallstackIdx a = h.stack({0x1010, 0x1104});
  const CallstackIdx b = h.stack({0x1010, 0x1100, 0x1204});
  h.state(0, 0, 100000, ThreadState::Running);
  h.sample(0, 1000, a);
  h.sample(0, 1100, b);
  h.sample(0, 1200, CallstackIdx(0));  // empty stack
  h.state(1, 0, 5000, ThreadState::Running);
  h.state(1, 5000, 100000, ThreadState::Waiting);
  h.sample(1, 4950, a);  // clipped to the end of the Running interval: weight 50
  h.sample(1, 6000, a);  // thread not running: weight 0, omitted
  h.finish(kFunctions);
  QueryContext ctx(h.m, *h.svc);

  exporters::ExportStats stats;
  const std::string text = exporttest::speedscopeText(ctx, {}, &stats);
  const json doc = exporttest::parseOrFail(text);
  EXPECT_EQ(exporttest::loadSpeedscopeSchema().validate(doc), "");

  EXPECT_EQ(doc["$schema"], "https://www.speedscope.app/file-format-schema.json");
  ASSERT_EQ(doc["profiles"].size(), 2u);
  EXPECT_EQ(frameNames(doc), (std::vector<std::string>{"Main", "Callee", "Leaf"}));

  const json& p0 = doc["profiles"][0];
  EXPECT_EQ(p0["type"], "sampled");
  EXPECT_EQ(p0["unit"], "nanoseconds");
  EXPECT_EQ(p0["name"], "Main (100)");
  EXPECT_EQ(p0["samples"], json::parse("[[0,1],[0,0,2],[]]"));
  EXPECT_EQ(p0["weights"], json::parse("[100,100,100]"));
  EXPECT_EQ(p0["startValue"], 0);
  EXPECT_EQ(p0["endValue"], 100000);

  const json& p1 = doc["profiles"][1];
  EXPECT_EQ(p1["name"], "thread (101)");
  EXPECT_EQ(p1["samples"], json::parse("[[0,1]]"));
  EXPECT_EQ(p1["weights"], json::parse("[50]"));

  EXPECT_EQ(stats.threads, 2u);
  EXPECT_EQ(stats.samples, 4u);
  EXPECT_EQ(stats.frames, 3u);
}

TEST(Speedscope, UnresolvedAndUnmappedFramesAreNamedByAddress) {
  HandModel h = makeModel();
  const CallstackIdx s = h.pushAbs(h.stack({0x1010, 0x2040}), 0xFFFFF80000001234ull);
  h.state(0, 0, 100000, ThreadState::Running);
  h.sample(0, 1000, s);
  h.finish();  // no symbols: module "game.exe" never resolves
  QueryContext ctx(h.m, *h.svc);
  const json doc = exporttest::parseOrFail(exporttest::speedscopeText(ctx));
  EXPECT_EQ(exporttest::loadSpeedscopeSchema().validate(doc), "");
  EXPECT_EQ(frameNames(doc),
            (std::vector<std::string>{"game.exe!0x1010", "game.exe!0x2040", "0xfffff80000001234"}));
  EXPECT_EQ(doc["profiles"][0]["samples"], json::parse("[[0,1,2]]"));
}

TEST(Speedscope, KernelLeafSampleUsesExactAddressForInterruptedFrame) {
  HandModel h = makeModel();
  // Main -> 0x1100 (user, below the kernel frames) -> kernel frame. For a kernel-leaf sample the user frame
  // is the interrupted ip: exact lookup gives Callee, whereas a return address 0x1100 - 1 would give Main.
  const CallstackIdx user = h.stack({0x1010, 0x1100});
  const CallstackIdx kernel = h.pushAbs(user, 0xFFFFF80000001000ull);
  h.state(0, 0, 100000, ThreadState::Running);
  h.sample(0, 1000, kernel, trace::kSampleFlagKernelLeaf);
  h.sample(0, 1100, kernel, 0);  // the same stack without the flag: return-address rule for the user frame
  h.finish(kFunctions);
  QueryContext ctx(h.m, *h.svc);
  const json doc = exporttest::parseOrFail(exporttest::speedscopeText(ctx));
  const auto names = frameNames(doc);
  const json& samples = doc["profiles"][0]["samples"];
  ASSERT_EQ(samples.size(), 2u);
  auto nameAt = [&](const json& sample, size_t i) { return names[sample[i].get<size_t>()]; };
  EXPECT_EQ(nameAt(samples[0], 0), "Main");
  EXPECT_EQ(nameAt(samples[0], 1), "Callee");
  EXPECT_EQ(nameAt(samples[0], 2), "0xfffff80000001000");
  EXPECT_EQ(nameAt(samples[1], 1), "Main");  // 0x1100 - 1
}

TEST(Speedscope, TimeRangeAndThreadFilters) {
  HandModel h = makeModel(2);
  const CallstackIdx a = h.stack({0x1010});
  h.state(0, 0, 100000, ThreadState::Running);
  h.state(1, 0, 100000, ThreadState::Running);
  for (uint64_t ts = 1000; ts < 2000; ts += 100) {
    h.sample(0, ts, a);
    h.sample(1, ts, a);
  }
  h.finish(kFunctions);
  QueryContext ctx(h.m, *h.svc);

  Filter byThread;
  byThread.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  json doc = exporttest::parseOrFail(exporttest::speedscopeText(ctx, byThread));
  ASSERT_EQ(doc["profiles"].size(), 1u);
  EXPECT_EQ(doc["profiles"][0]["name"], "thread (101)");
  EXPECT_EQ(doc["profiles"][0]["samples"].size(), 10u);

  Filter range;
  range.t0 = 1200;
  range.t1 = 1500;  // samples at 1200, 1300, 1400
  doc = exporttest::parseOrFail(exporttest::speedscopeText(ctx, range));
  ASSERT_EQ(doc["profiles"].size(), 2u);
  for (const json& p : doc["profiles"]) {
    EXPECT_EQ(p["weights"], json::parse("[100,100,100]"));
    EXPECT_EQ(p["startValue"], 1200);
    EXPECT_EQ(p["endValue"], 1500);
  }

  Filter empty;
  empty.t0 = 50000;
  empty.t1 = 60000;
  doc = exporttest::parseOrFail(exporttest::speedscopeText(ctx, empty));
  EXPECT_EQ(doc["profiles"].size(), 0u);
  EXPECT_EQ(exporttest::loadSpeedscopeSchema().validate(doc), "");
}

TEST(Speedscope, NonNanosecondClockConvertsWeights) {
  HandModel h = makeModel();
  h.m.header.clock_frequency_hz = 10'000'000ull;  // 100 ns per tick
  h.m.sampleIntervalTicks = 30;
  const CallstackIdx a = h.stack({0x1010});
  h.state(0, 0, 100000, ThreadState::Running);
  h.sample(0, 1000, a);
  h.finish(kFunctions);
  QueryContext ctx(h.m, *h.svc);
  const json doc = exporttest::parseOrFail(exporttest::speedscopeText(ctx));
  EXPECT_EQ(doc["profiles"][0]["weights"], json::parse("[3000]"));
  EXPECT_EQ(doc["profiles"][0]["endValue"], 100000 * 100);
}

TEST(Speedscope, ErrorsAreReported) {
  HandModel h = makeModel();
  h.sample(0, 1000, h.stack({0x1010}));
  h.state(0, 0, 100000, ThreadState::Running);
  h.finish(kFunctions);
  QueryContext ctx(h.m, *h.svc);
  std::ostringstream os;

  Filter subtree;
  subtree.subtreeRoot = CallstackIdx(1);
  auto r = exportSpeedscope(ctx, subtree, os);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);

  Filter badThread;
  badThread.threads = std::vector<ThreadIdx>{ThreadIdx(7)};
  r = exportSpeedscope(ctx, badThread, os);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);

  h.m.header.clock_frequency_hz = 0;
  r = exportSpeedscope(ctx, {}, os);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  h.m.header.clock_frequency_hz = 1'000'000'000ull;

  std::ostringstream bad;
  bad.setstate(std::ios::badbit);
  r = exportSpeedscope(ctx, {}, bad);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::IoError);
}

TEST(Speedscope, UnbuiltThreadStatesAreRejected) {
  LoadedFixture f("tiny");
  f.m.statesByThread.clear();
  std::ostringstream os;
  auto r = exportSpeedscope(*f.ctx, {}, os);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

class SpeedscopeFixture : public ::testing::TestWithParam<const char*> {};

// Schema, per-thread weight totals against the aggregation, return-address rule against frameKeyAt, and
// determinism, on each fixture.
TEST_P(SpeedscopeFixture, MatchesAggregationAndAttributionRules) {
  LoadedFixture f(GetParam());
  const model::TraceModel& m = f.m;
  const uint64_t hz = m.header.clock_frequency_hz;
  ASSERT_GT(hz, 0u);

  const std::string text = exporttest::speedscopeText(*f.ctx);
  EXPECT_EQ(text, exporttest::speedscopeText(*f.ctx)) << "export is not deterministic";
  const json doc = exporttest::parseOrFail(text);
  EXPECT_EQ(exporttest::loadSpeedscopeSchema().validate(doc), "");

  // Profiles by name for lookup.
  std::map<std::string, const json*> profiles;
  for (const json& p : doc["profiles"]) profiles[p["name"].get<std::string>()] = &p;
  const size_t frameCount = doc["shared"]["frames"].size();

  size_t expectedProfiles = 0;
  std::map<analysis::FunctionKey, uint32_t> keyToFrame;
  std::map<uint32_t, analysis::FunctionKey> frameToKey;
  for (uint32_t ti = 0; ti < m.threads.size(); ++ti) {
    const model::Thread& th = m.threads[ti];
    // Expected samples: the weighted samples (weight > 0), in time order.
    struct Expected {
      CallstackIdx cs;
      uint16_t flags;
      uint64_t weight;
    };
    std::vector<Expected> expected;
    uint64_t sumTicks = 0;
    ASSERT_TRUE(analysis::forEachWeightedSample(m, ThreadIdx(ti), 0, UINT64_MAX, {},
                                                [&](uint64_t, CallstackIdx cs, uint64_t w, uint16_t fl) {
                                                  if (w == 0) return;
                                                  expected.push_back({cs, fl, w});
                                                  sumTicks += w;
                                                }));
    const std::string name =
        (th.name.empty() ? std::string("thread") : th.name) + " (" + std::to_string(th.tid) + ")";
    if (expected.empty()) {
      EXPECT_EQ(profiles.count(name), 0u) << name;
      continue;
    }
    ++expectedProfiles;
    ASSERT_EQ(profiles.count(name), 1u) << name;
    const json& p = *profiles[name];
    ASSERT_EQ(p["samples"].size(), expected.size()) << name;
    ASSERT_EQ(p["weights"].size(), expected.size()) << name;

    // Weight sum equals the aggregation's inclusive root total (ticks -> ns).
    uint64_t sumNs = 0;
    for (const json& w : p["weights"]) sumNs += w.get<uint64_t>();
    Filter one;
    one.threads = std::vector<ThreadIdx>{ThreadIdx(ti)};
    auto tree = analysis::topDownTree(*f.ctx, one);
    ASSERT_TRUE(tree.ok());
    EXPECT_EQ(tree->inclusiveTicks, sumTicks) << name;
    EXPECT_NEAR(static_cast<double>(sumNs), static_cast<double>(sumTicks) * 1e9 / static_cast<double>(hz),
                static_cast<double>(expected.size()))
        << name;

    // Frame identity follows frameKeyAt (return-address rule) position by position.
    for (size_t si = 0; si < expected.size(); ++si) {
      const json& stack = p["samples"][si];
      const bool kernelLeaf = (expected[si].flags & trace::kSampleFlagKernelLeaf) != 0;
      for (size_t i = 0; i < stack.size(); ++i) {
        const uint32_t frame = stack[i].get<uint32_t>();
        ASSERT_LT(frame, frameCount);
        const size_t position = stack.size() - 1 - i;  // leaf = 0
        const analysis::FunctionKey key = analysis::frameKeyAt(*f.ctx, expected[si].cs, position, kernelLeaf);
        ASSERT_NE(key, analysis::kRootKey) << name << " sample " << si;
        const auto [kit, kNew] = keyToFrame.emplace(key, frame);
        EXPECT_EQ(kit->second, frame) << "function key maps to two frames";
        const auto [fit, fNew] = frameToKey.emplace(frame, key);
        EXPECT_EQ(fit->second, key) << "frame covers two function keys";
        (void)kNew;
        (void)fNew;
      }
    }
  }
  EXPECT_EQ(profiles.size(), expectedProfiles);
  EXPECT_EQ(doc["profiles"].size(), expectedProfiles);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, SpeedscopeFixture,
                         ::testing::Values("tiny", "recursion", "scopes", "frames", "fibers", "partial"));

TEST(Speedscope, ResolvedFixtureFramesCarryNames) {
  LoadedFixture f("tiny");
  const json doc = exporttest::parseOrFail(exporttest::speedscopeText(*f.ctx));
  ASSERT_GT(doc["shared"]["frames"].size(), 0u);
  size_t resolved = 0;
  for (const json& fr : doc["shared"]["frames"]) {
    const std::string name = fr["name"].get<std::string>();
    EXPECT_FALSE(name.empty());
    if (name.find("!0x") == std::string::npos && name.rfind("0x", 0) != 0) ++resolved;
  }
  EXPECT_GT(resolved, 0u) << "the fixture's symbols were not applied";
}

TEST(Speedscope, WithoutSymbolsEveryFrameIsAnAddress) {
  LoadedFixture f("tiny", /*withSymbols=*/false);
  const json doc = exporttest::parseOrFail(exporttest::speedscopeText(*f.ctx));
  EXPECT_EQ(exporttest::loadSpeedscopeSchema().validate(doc), "");
  for (const json& fr : doc["shared"]["frames"]) {
    const std::string name = fr["name"].get<std::string>();
    EXPECT_TRUE(name.find("0x") != std::string::npos) << name;
  }
}

}  // namespace
}  // namespace tachyon::exporters
