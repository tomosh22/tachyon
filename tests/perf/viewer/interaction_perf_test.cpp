// Perf budget (WP-46): the "N longest waits" scan over the whole of `medium` (all threads, N = 50) in < 50 ms
// (asserted at 2x = 100 ms). The scan is a single pass over the per-thread state intervals with a bounded
// heap; the numbers are printed.
#include "analysis/thread_states.h"
#include "viewer/interaction/interaction_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

using namespace tachyon;
using namespace tachyon::viewer::interaction;

namespace {

constexpr double kBudgetMs = 50.0;  // target; assert at 2x

double bestOfMs(const model::TraceModel& m, const WaitQuery& q, int reps, WaitScanResult* last) {
  double best = 1e30;
  for (int i = 0; i < reps; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    *last = longestWaits(m, q, nullptr);
    best = std::min(best,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  return best;
}

}  // namespace

TEST(InteractionPerf, LongestWaitsOnMedium) {
  auto loaded =
      model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel m = std::move(*loaded);
  ASSERT_TRUE(analysis::buildThreadStates(m).ok());
  size_t intervals = 0;
  for (const auto& v : m.statesByThread) intervals += v.size();

  WaitQuery all;
  all.t0 = m.header.start_ts;
  WaitScanResult r;
  const double bestAll = bestOfMs(m, all, 9, &r);
  ASSERT_FALSE(r.rows.empty());

  WaitQuery big = all;
  big.maxResults = 5000;  // a long list stresses the heap
  WaitScanResult rb;
  const double bestBig = bestOfMs(m, big, 9, &rb);

  WaitQuery half = all;
  half.t0 = m.header.start_ts + (m.header.end_ts - m.header.start_ts) / 4;
  half.t1 = m.header.start_ts + (m.header.end_ts - m.header.start_ts) / 2;
  WaitScanResult rh;
  const double bestHalf = bestOfMs(m, half, 9, &rh);

  std::printf(
      "[interaction perf] medium: %zu threads, %zu intervals, %llu waits; longest-waits N=50 best %.3f ms, "
      "N=5000 best %.3f ms, quarter range best %.3f ms (budget %.0f ms, assert %.0f ms)\n",
      m.threads.size(), intervals, static_cast<unsigned long long>(r.totalWaits), bestAll, bestBig, bestHalf,
      kBudgetMs, kBudgetMs * 2);
  EXPECT_LT(bestAll, kBudgetMs * 2);
  EXPECT_LT(bestBig, kBudgetMs * 2);
  EXPECT_LT(bestHalf, kBudgetMs * 2);
}
