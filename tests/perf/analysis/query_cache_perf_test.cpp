// Perf budgets (WP-33), asserted at 2x as per the conventions; measured numbers are printed:
//   * a query-cache hit < 50 us (shared-result API; the by-value API additionally copies the result);
//   * a bottomUpFunctions over a 10% time range of `medium` < 30 ms (cold cache, best of several runs).
#include "analysis/query.h"

#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace tachyon::analysis {
namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

struct Prepared {
  model::TraceModel m;
  symbols::SymbolService svc{std::vector<std::string>{}, std::filesystem::path{}};
};

bool prepare(Prepared& p, const std::string& name) {
  const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
  if (!std::filesystem::exists(dir / (name + ".tachyon"))) return false;
  auto r = model::TraceModel::load(dir / (name + ".tachyon"));
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  if (!r.ok()) return false;
  p.m = std::move(*r);
  EXPECT_TRUE(buildThreadStates(p.m).ok());
  EXPECT_TRUE(p.svc.addSymbolFileOverride(dir / (name + ".symbols.json")).ok());
  p.svc.resolveAllAsync(p.m);
  p.svc.waitAll();
  return true;
}

TEST(QueryCachePerf, MediumCacheHitIsFast) {
  Prepared p;
  if (!prepare(p, "medium")) GTEST_SKIP() << "medium fixture not generated";
  QueryContext ctx(p.m, p.svc);
  const uint64_t span = p.m.header.end_ts - p.m.header.start_ts;
  struct Case {
    const char* name;
    Filter filter;
  };
  std::vector<Case> cases(3);
  cases[0].name = "all threads, full range";
  cases[1].name = "10% time range";
  cases[1].filter.t0 = p.m.header.start_ts + span * 4 / 10;
  cases[1].filter.t1 = cases[1].filter.t0 + span / 10;
  cases[2].name = "explicit thread list + time range";
  cases[2].filter.t0 = p.m.header.start_ts + span / 10;
  cases[2].filter.t1 = p.m.header.start_ts + span / 2;
  cases[2].filter.threads = std::vector<ThreadIdx>{};
  for (uint32_t t = 0; t < p.m.threads.size() && t < 6; ++t) cases[2].filter.threads->push_back(ThreadIdx(t));

  for (const Case& c : cases) {
    auto first = bottomUpFunctionsShared(ctx, c.filter);
    ASSERT_TRUE(first.ok());
    constexpr int kReps = 2000;
    double best = 1e30, sum = 0;
    for (int batch = 0; batch < 10; ++batch) {
      const auto t0 = Clock::now();
      for (int i = 0; i < kReps / 10; ++i) {
        auto hit = bottomUpFunctionsShared(ctx, c.filter);
        if (!hit.ok() || hit->get() != first->get()) FAIL() << "not a cache hit";
      }
      const double per = seconds(t0) / (kReps / 10);
      best = std::min(best, per);
      sum += per;
    }
    const double avgUs = sum / 10 * 1e6, bestUs = best * 1e6;
    std::printf(
        "[query cache perf] hit (%s): avg %.2f us, best batch %.2f us (target 50 us, assert 100 us)\n",
        c.name, avgUs, bestUs);
    EXPECT_LT(avgUs, 100.0) << c.name;
  }
  // The by-value API copies the rows; informational, with the same bound on small-result queries.
  const auto t0 = Clock::now();
  for (int i = 0; i < 200; ++i) ASSERT_TRUE(bottomUpFunctions(ctx, cases[1].filter).ok());
  std::printf("[query cache perf] by-value hit with copy (10%% range, %zu rows): %.2f us\n",
              (*bottomUpFunctionsShared(ctx, cases[1].filter))->size(), seconds(t0) / 200 * 1e6);
  const CacheStats s = ctx.cacheStats();
  std::printf("[query cache perf] cache: %llu entries, %.1f KiB, %llu result hits, %llu misses\n",
              static_cast<unsigned long long>(s.entries), static_cast<double>(s.bytes) / 1024,
              static_cast<unsigned long long>(s.resultHits), static_cast<unsigned long long>(s.resultMisses));
}

TEST(QueryCachePerf, MediumTenPercentRangeBottomUp) {
  Prepared p;
  if (!prepare(p, "medium")) GTEST_SKIP() << "medium fixture not generated";
  size_t samples = 0;
  for (const auto& s : p.m.samplesByThread) samples += s.ts.size();
  const uint64_t span = p.m.header.end_ts - p.m.header.start_ts;
  double best = 1e30, worst = 0;
  size_t rows = 0;
  constexpr int kWindows = 9;
  for (int w = 0; w < kWindows; ++w) {
    // A different window each time, a fresh context so nothing is cached.
    QueryContext ctx(p.m, p.svc);
    Filter f;
    f.t0 = p.m.header.start_ts + span * static_cast<uint64_t>(w) / 10;
    f.t1 = f.t0 + span / 10;
    const auto t0 = Clock::now();
    auto r = bottomUpFunctions(ctx, f);
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok());
    rows = r->size();
    best = std::min(best, s);
    worst = std::max(worst, s);
    std::printf("[query cache perf] medium 10%% range #%d bottomUp: %.4f s\n", w, s);
  }
  // Full range for comparison, then the same windows warmed by it (different filter: still a scan).
  QueryContext ctx(p.m, p.svc);
  const auto t0 = Clock::now();
  ASSERT_TRUE(bottomUpFunctions(ctx, {}).ok());
  std::printf(
      "[query cache perf] medium 10%% range bottomUp: best %.4f s, worst %.4f s (target 0.030 s, assert "
      "0.060 s, %u hw threads); full range %.4f s; %zu samples, %zu rows\n",
      best, worst, std::thread::hardware_concurrency(), seconds(t0), samples, rows);
  EXPECT_LT(best, 0.030 * 2);
  EXPECT_LT(worst, 0.030 * 4) << "no window should be a gross outlier";
}

}  // namespace
}  // namespace tachyon::analysis
