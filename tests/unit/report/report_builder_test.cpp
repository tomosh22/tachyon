// WP-62: buildReport on the tracegen fixtures `frames`, `scopes` and `medium` against the expected.json
// ground truth and against the analysis queries computed directly; locks and trace metadata on a hand model.
#include "report/report_builder.h"

#include "aggregation_test_util.h"
#include "analysis/instances.h"
#include "report_test_util.h"
#include "scope_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace tachyon::report {
namespace {

using analysis::AggregationOptions;
using analysis::Filter;
using analysis::QueryContext;
using testutil::Loaded;

constexpr double kTol = 1e-6;
constexpr double kTicksPerMs = 10000.0;  // the fixtures run at a 10 MHz trace clock

Report buildOrDie(const Loaded& l, ReportOptions o = {}) {
  auto r = buildReport(l.m, l.svc, o);
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  return r.ok() ? std::move(*r) : Report{};
}

// Independent lower order statistic: q = pct/100, index floor(q * (n-1)).
double percentileMs(std::vector<uint64_t> d, int pct) {
  std::sort(d.begin(), d.end());
  const size_t idx = (d.size() - 1) * static_cast<size_t>(pct) / 100;
  return static_cast<double>(d[idx]) / kTicksPerMs;
}

TEST(LowerQuantile, UsesTheLowerOrderStatistic) {
  const std::vector<uint64_t> d = {1, 2, 3, 4};
  EXPECT_EQ(lowerQuantile(d, 50, 100), 2u);  // floor(0.5 * 3) = 1
  EXPECT_EQ(lowerQuantile(d, 95, 100), 3u);  // floor(0.95 * 3) = 2
  EXPECT_EQ(lowerQuantile(d, 99, 100), 3u);
  EXPECT_EQ(lowerQuantile(d, 100, 100), 4u);
  std::vector<uint64_t> hundred(101);
  std::iota(hundred.begin(), hundred.end(), 0u);
  EXPECT_EQ(lowerQuantile(hundred, 95, 100), 95u);  // 0.95 * 100 must not round down to 94
  EXPECT_EQ(lowerQuantile(hundred, 99, 100), 99u);
  EXPECT_EQ(lowerQuantile({}, 50, 100), 0u);
}

TEST(ReportFrames, FramesFixtureMatchesExpectedDurations) {
  Loaded l("frames");
  ReportOptions o;
  o.topN = 5;
  const Report r = buildOrDie(l, o);
  ASSERT_EQ(r.frames.size(), 2u);
  EXPECT_EQ(r.frameSeriesTotal, 2u);

  std::vector<uint64_t> main, render;
  for (const auto& f : l.expected["frames"]) {
    const uint64_t dur = f["end"].get<uint64_t>() - f["start"].get<uint64_t>();
    if (f["name"] == "Frame" && f["mark_tid"] == 501) main.push_back(dur);
    if (f["name"] == "RenderFrame") render.push_back(dur);
  }
  ASSERT_EQ(main.size(), 116u);

  const FrameSeriesRow& m = r.frames[0];
  EXPECT_EQ(m.label, "Frame@Main");
  EXPECT_TRUE(m.isDefault);
  ASSERT_TRUE(m.threadTid.has_value());
  EXPECT_EQ(*m.threadTid, 501u);
  EXPECT_EQ(m.frameCount, main.size());
  EXPECT_EQ(m.markCount, main.size() + 1);
  EXPECT_NEAR(m.p50Ms, percentileMs(main, 50), kTol);
  EXPECT_NEAR(m.p95Ms, percentileMs(main, 95), kTol);
  EXPECT_NEAR(m.p99Ms, percentileMs(main, 99), kTol);
  EXPECT_NEAR(m.maxMs, static_cast<double>(*std::max_element(main.begin(), main.end())) / kTicksPerMs, kTol);
  const double mean = static_cast<double>(std::accumulate(main.begin(), main.end(), uint64_t{0})) /
                      static_cast<double>(main.size()) / kTicksPerMs;
  EXPECT_NEAR(m.meanMs, mean, kTol);
  // One hitch frame of 50.666 ms; every other frame is 16.666 ms, under the 16.67 ms default budget.
  EXPECT_EQ(m.overBudget, 1u);
  EXPECT_EQ(m.slowestFrameNumber, l.expected["hitch_frames"][0]["number"].get<uint64_t>());

  EXPECT_FALSE(r.frames[1].isDefault);
  EXPECT_EQ(r.frames[1].name, "RenderFrame");
  EXPECT_EQ(r.frames[1].frameCount, render.size());
  EXPECT_NEAR(r.frames[1].p95Ms, percentileMs(render, 95), kTol);
}

TEST(ReportFrames, MatchesTheFramesQueryAndHonoursTheBudget) {
  Loaded l("frames");
  ReportOptions o;
  o.frameBudgetMs = 20.0;
  const Report r = buildOrDie(l, o);
  const QueryContext ctx(l.m, l.svc);
  auto frames = analysis::frames(ctx, Filter{});  // default series
  ASSERT_TRUE(frames.ok());
  std::vector<uint64_t> d;
  uint64_t over = 0;
  for (const auto& f : *frames) {
    d.push_back(f.duration);
    if (static_cast<double>(f.duration) / kTicksPerMs > 20.0) ++over;
  }
  EXPECT_EQ(r.frames[0].frameCount, d.size());
  EXPECT_NEAR(r.frames[0].p99Ms, percentileMs(d, 99), kTol);
  EXPECT_EQ(r.frames[0].overBudget, over);
  EXPECT_EQ(r.settings.frameBudgetMs, 20.0);
}

TEST(ReportFrames, SeriesFilterAndUnknownSeries) {
  Loaded l("frames");
  ReportOptions o;
  o.frameSeries = "RenderFrame";
  const Report r = buildOrDie(l, o);
  ASSERT_EQ(r.frames.size(), 1u);
  EXPECT_EQ(r.frames[0].name, "RenderFrame");
  EXPECT_TRUE(r.frames[0].isDefault);  // the first listed series
  ASSERT_TRUE(r.settings.frameSeriesFilter.has_value());

  o.frameSeries = "NoSuchSeries";
  auto bad = buildReport(l.m, l.svc, o);
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
}

TEST(ReportFrames, TraceWithoutFrameMarksHasNoSeries) {
  Loaded l("medium");
  const Report r = buildOrDie(l);
  EXPECT_TRUE(r.frames.empty());
  EXPECT_EQ(r.frameSeriesTotal, 0u);
}

TEST(ReportThreads, MatchThreadStateTotalsAndExpectedStateTicks) {
  Loaded l("frames");
  const Report r = buildOrDie(l);
  ASSERT_EQ(r.threads.size(), l.expected["threads"].size());
  const QueryContext ctx(l.m, l.svc);
  std::map<uint32_t, ThreadRow> byTid;
  for (const ThreadRow& t : r.threads) byTid[t.tid] = t;
  for (const auto& et : l.expected["threads"]) {
    const uint32_t tid = et["tid"].get<uint32_t>();
    ASSERT_TRUE(byTid.count(tid));
    const ThreadRow& row = byTid[tid];
    auto direct = analysis::threadStateTotals(ctx, Filter{}, l.m.tidToThread.at(tid));
    ASSERT_TRUE(direct.ok());
    EXPECT_NEAR(row.runningMs, static_cast<double>(direct->runningTicks) / kTicksPerMs, kTol);
    EXPECT_NEAR(row.readyMs, static_cast<double>(direct->readyTicks) / kTicksPerMs, kTol);
    EXPECT_NEAR(row.waitingMs, static_cast<double>(direct->waitingTicks) / kTicksPerMs, kTol);
    EXPECT_NEAR(row.runningMs, et["state_ticks"]["running"].get<double>() / kTicksPerMs, 0.2);
    EXPECT_EQ(row.sampleCount, et["samples"].get<uint64_t>());
    EXPECT_EQ(row.name, et["name"].get<std::string>());
  }
  // Sorted by running time, descending.
  for (size_t i = 1; i < r.threads.size(); ++i) EXPECT_GE(r.threads[i - 1].runningMs, r.threads[i].runningMs);
}

struct Named {
  uint64_t exclusive;
  uint64_t inclusive;
  std::string module, name;
  bool scope;
};

// The top N rows computed independently from bottomUpFunctions.
std::vector<Named> expectedTop(const Loaded& l, bool blend, size_t n, uint64_t& total) {
  AggregationOptions ao;
  ao.blendScopes = blend;
  const QueryContext ctx(l.m, l.svc, ao);
  auto rows = analysis::bottomUpFunctions(ctx, Filter{});
  EXPECT_TRUE(rows.ok());
  std::vector<Named> all;
  total = 0;
  for (const auto& row : *rows) {
    total += row.exclusiveTicks;
    if (row.exclusiveTicks == 0) continue;
    Named x;
    x.exclusive = row.exclusiveTicks;
    x.inclusive = row.inclusiveTicks;
    x.scope = row.key.isScope();
    const std::string full = analysis::functionKeyName(ctx, row.key);
    if (x.scope) {
      x.name = full.substr(std::string("scope:").size());
    } else if (const size_t bang = full.find('!'); bang != std::string::npos) {
      x.module = full.substr(0, bang);
      x.name = full.substr(bang + 1);
    } else {
      x.name = full;
    }
    all.push_back(std::move(x));
  }
  std::stable_sort(all.begin(), all.end(), [](const Named& a, const Named& b) {
    if (a.exclusive != b.exclusive) return a.exclusive > b.exclusive;
    if (a.name != b.name) return a.name < b.name;
    return a.module < b.module;
  });
  if (all.size() > n) all.resize(n);
  return all;
}

void expectFunctionsMatch(const std::string& fixture, bool blend, uint32_t topN) {
  Loaded l(fixture);
  ReportOptions o;
  o.topN = topN;
  o.blendScopes = blend;
  const Report r = buildOrDie(l, o);
  uint64_t total = 0;
  const std::vector<Named> want = expectedTop(l, blend, topN, total);
  ASSERT_EQ(r.functions.size(), want.size()) << fixture;
  EXPECT_NEAR(r.totalExclusiveMs, static_cast<double>(total) / kTicksPerMs, kTol);
  const double hz = static_cast<double>(l.m.header.clock_frequency_hz);
  for (size_t i = 0; i < want.size(); ++i) {
    const FunctionReportRow& got = r.functions[i];
    EXPECT_EQ(got.name, want[i].name) << fixture << " #" << i;
    EXPECT_EQ(got.module, want[i].module) << fixture << " #" << i;
    EXPECT_EQ(got.isScope, want[i].scope);
    EXPECT_NEAR(got.exclusiveMs, static_cast<double>(want[i].exclusive) * 1000.0 / hz, kTol);
    EXPECT_NEAR(got.inclusiveMs, static_cast<double>(want[i].inclusive) * 1000.0 / hz, kTol);
    EXPECT_NEAR(got.exclusivePct, 100.0 * static_cast<double>(want[i].exclusive) / static_cast<double>(total),
                1e-4);
  }
}

TEST(ReportFunctions, MediumTopMatchesBottomUpQuery) {
  expectFunctionsMatch("medium", true, 10);
}

TEST(ReportFunctions, FramesFixtureIncludingAllFunctions) {
  expectFunctionsMatch("frames", true, 50);
}

TEST(ReportFunctions, ScopesFixtureBlendedAndNot) {
  expectFunctionsMatch("scopes", true, 30);
  expectFunctionsMatch("scopes", false, 30);
}

// WP-69: the baseline function set holds every function with at least 0.5% exclusive share, whatever topN is.
TEST(ReportFunctions, SignificantSetIsIndependentOfTopN) {
  for (const char* fixture : {"frames", "medium", "scopes"}) {
    Loaded l(fixture);
    ReportOptions small;
    small.topN = 2;
    ReportOptions large;
    large.topN = 500;
    const Report a = buildOrDie(l, small), b = buildOrDie(l, large);
    EXPECT_EQ(a.significantFloorPct, kSignificantFunctionPct) << fixture;
    ASSERT_EQ(a.significantFunctions.size(), b.significantFunctions.size()) << fixture;
    ASSERT_FALSE(a.significantFunctions.empty()) << fixture;
    for (size_t i = 0; i < a.significantFunctions.size(); ++i) {
      EXPECT_EQ(a.significantFunctions[i].name, b.significantFunctions[i].name) << fixture;
      EXPECT_GE(a.significantFunctions[i].exclusivePct, kSignificantFunctionPct) << fixture;
    }
    // Every function of the (complete) large report with a share of at least 0.5% is in the set, nothing
    // else.
    size_t want = 0;
    for (const FunctionReportRow& f : b.functions)
      if (f.exclusivePct >= kSignificantFunctionPct) ++want;
    EXPECT_EQ(a.significantFunctions.size(), want) << fixture;
    // The top rows of the small report agree with the set's leading rows.
    for (size_t i = 0; i < a.functions.size() && i < a.significantFunctions.size(); ++i)
      EXPECT_EQ(a.functions[i].name, a.significantFunctions[i].name) << fixture;
  }
}

TEST(ReportFunctions, TotalCpuIsTheRunningTimeOfAllThreads) {
  Loaded l("medium");
  const Report r = buildOrDie(l);
  double running = 0;
  for (const ThreadRow& t : r.threads) running += t.runningMs;
  EXPECT_GT(r.trace.totalCpuMs, 0.0);
  EXPECT_NEAR(r.trace.totalCpuMs, running, 1e-3 * static_cast<double>(r.threads.size() + 1));
}

TEST(ReportFunctions, BlendingIsReportedAsEffective) {
  Loaded scopes("scopes");
  EXPECT_TRUE(buildOrDie(scopes).settings.blendScopes);
  ReportOptions off;
  off.blendScopes = false;
  EXPECT_FALSE(buildOrDie(scopes, off).settings.blendScopes);
  Loaded frames("frames");  // no instrumentation: blending is moot
  EXPECT_FALSE(buildOrDie(frames).settings.blendScopes);
}

TEST(ReportScopes, ScopesFixtureMatchesExpectedStatsAndInstances) {
  Loaded l("scopes");
  ReportOptions o;
  o.topN = 50;
  const Report r = buildOrDie(l, o);
  const QueryContext ctx(l.m, l.svc);
  ASSERT_FALSE(l.expected["scopes"].empty());
  EXPECT_EQ(r.scopeCount, l.expected["scopes"].size());
  EXPECT_EQ(r.scopes.size(), l.expected["scopes"].size());
  std::map<std::string, ScopeReportRow> byName;
  for (const ScopeReportRow& s : r.scopes) byName[s.name] = s;
  for (const auto& e : l.expected["scopes"]) {
    const std::string id = e["id"].get<std::string>();
    ASSERT_TRUE(byName.count(id)) << id;
    const ScopeReportRow& s = byName[id];
    EXPECT_EQ(s.count, e["instances"].get<uint64_t>()) << id;
    EXPECT_NEAR(s.minMs, e["min_ticks"].get<double>() / kTicksPerMs, kTol) << id;
    EXPECT_NEAR(s.medianMs, e["median_ticks"].get<double>() / kTicksPerMs, kTol) << id;
    EXPECT_NEAR(s.maxMs, e["max_ticks"].get<double>() / kTicksPerMs, kTol) << id;
    EXPECT_NEAR(s.totalMs, e["total_ticks"].get<double>() / kTicksPerMs, kTol) << id;
    // p95 from the instance list, sorted independently.
    auto inst = analysis::scopeInstances(ctx, Filter{}, l.m.strings.find(id));
    ASSERT_TRUE(inst.ok());
    std::vector<uint64_t> d;
    for (const auto& i : **inst) d.push_back(i.end - i.begin);
    EXPECT_NEAR(s.p95Ms, percentileMs(d, 95), kTol) << id;
    // And the stats query itself.
    auto st = analysis::scopeStats(ctx, Filter{}, l.m.strings.find(id));
    ASSERT_TRUE(st.ok());
    EXPECT_NEAR(s.medianMs, st->median / kTicksPerMs, kTol) << id;
    EXPECT_EQ(s.unterminated, st->unterminated) << id;
    EXPECT_EQ(s.orphanEnds, st->orphanEnds) << id;
  }
  // Sorted by total time, descending.
  for (size_t i = 1; i < r.scopes.size(); ++i) EXPECT_GE(r.scopes[i - 1].totalMs, r.scopes[i].totalMs);
}

TEST(ReportScopes, TopNLimitsScopes) {
  Loaded l("scopes");
  ReportOptions o;
  o.topN = 3;
  const Report r = buildOrDie(l, o);
  EXPECT_EQ(r.scopes.size(), 3u);
  EXPECT_GT(r.scopeCount, 3u);
  EXPECT_EQ(r.scopes[0].name, "Frame");  // the largest total
}

// ---- hand model: locks and trace metadata ----------------------------------------------------------------

constexpr uint32_t kLockColor = 0x4C4F43FFu;

struct LockModel {
  analysis::testutil::HandModel h{2};
  LockModel() {
    h.m.header.clock_frequency_hz = 1'000'000'000ull;  // 1 tick = 1 ns
    h.m.header.platform = trace::kPlatformLinux;
    h.m.captureInfo.sampling_hz = 4000;
    h.m.captureInfo.events_lost = 7;
    h.m.captureInfo.raw_json =
        R"({"sampling_hz":4000,"events_lost":7,"warnings":["spool cap hit",5,"second"],"throttle":{"events":3}})";
    for (uint32_t t = 0; t < 2; ++t) h.state(t, 0, 100000, model::ThreadState::Running);
    h.m.threads[0].name = "Main";
    analysis::testutil::ScopeBuilder sc(h);
    sc.begin(1000, "Mutex.Scene", 0x10, 100, "0x1000", kLockColor);
    sc.end(11000, 100);  // 10 us
    sc.begin(20000, "Mutex.Scene", 0x10, 101, "0x1000", kLockColor);
    sc.end(50000, 101);  // 30 us
    sc.begin(60000, "Mutex.Audio", 0x10, 101, "0x2000", kLockColor);
    sc.end(65000, 101);  // 5 us
    sc.sort();
    h.finish();
  }
};

TEST(ReportLocks, MatchLockSummary) {
  LockModel lm;
  ReportOptions o;
  o.topN = 5;
  auto rr = buildReport(lm.h.m, *lm.h.svc, o);
  ASSERT_TRUE(rr.ok()) << rr.error().toString();
  const Report& r = *rr;
  const QueryContext ctx(lm.h.m, *lm.h.svc);
  auto direct = analysis::lockSummary(ctx, Filter{});
  ASSERT_TRUE(direct.ok());
  ASSERT_EQ(direct->locks.size(), 2u);
  ASSERT_EQ(r.locks.size(), 2u);
  EXPECT_EQ(r.lockCount, 2u);
  EXPECT_EQ(r.locks[0].name, "Mutex.Scene");
  EXPECT_EQ(r.locks[0].address, "0x1000");
  EXPECT_EQ(r.locks[0].contentions, 2u);
  EXPECT_NEAR(r.locks[0].totalWaitMs, 0.040, kTol);
  EXPECT_NEAR(r.locks[0].maxWaitMs, 0.030, kTol);
  ASSERT_EQ(r.locks[0].topThreads.size(), 2u);
  EXPECT_EQ(r.locks[0].topThreads[0].tid, 101u);  // the longer waiter first
  EXPECT_NEAR(r.locks[0].topThreads[0].waitMs, 0.030, kTol);
  EXPECT_EQ(r.locks[0].topThreads[1].name, "Main");
  EXPECT_EQ(r.locks[1].name, "Mutex.Audio");
  EXPECT_EQ(r.locks[1].address, "0x2000");
  EXPECT_NEAR(r.locks[1].totalWaitMs, 0.005, kTol);
  EXPECT_EQ(direct->locks[0].totalWaitTicks, 40000u);

  o.topN = 1;
  auto one = buildReport(lm.h.m, *lm.h.svc, o);
  ASSERT_TRUE(one.ok());
  EXPECT_EQ(one->locks.size(), 1u);
  EXPECT_EQ(one->lockCount, 2u);
}

TEST(ReportTrace, MetadataFromHeaderAndCaptureInfo) {
  LockModel lm;
  auto rr = buildReport(lm.h.m, *lm.h.svc, {});
  ASSERT_TRUE(rr.ok());
  const TraceSummary& t = rr->trace;
  EXPECT_EQ(t.platform, "linux");
  EXPECT_NEAR(t.durationMs, 0.1, kTol);
  EXPECT_EQ(t.clockFrequencyHz, 1'000'000'000ull);
  EXPECT_EQ(t.samplingHz, 4000u);
  EXPECT_EQ(t.eventsLost, 7u);
  EXPECT_EQ(t.throttleEvents, 3u);
  EXPECT_FALSE(t.partial);
  EXPECT_TRUE(t.hasInstrumentation);
  EXPECT_EQ(t.threadCount, 2u);
  ASSERT_EQ(t.warnings.size(), 2u);  // the non-string entry is skipped
  EXPECT_EQ(t.warnings[0], "spool cap hit");
  EXPECT_EQ(t.warnings[1], "second");
}

TEST(ReportTrace, PartialFlagFromTornFixture) {
  Loaded l("partial_torn");
  EXPECT_TRUE(l.m.isPartial);
  EXPECT_TRUE(buildOrDie(l).trace.partial);
  Loaded ok("frames");
  EXPECT_FALSE(buildOrDie(ok).trace.partial);
}

}  // namespace
}  // namespace tachyon::report
