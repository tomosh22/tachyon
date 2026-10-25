// Perf budget (WP-31): buildThreadStates on `medium` in < 0.5 s (asserted at 2x = 1 s). Measured numbers are
// printed.
#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace tachyon::analysis {
namespace {

constexpr double kBudgetSeconds = 0.5;  // target; assert at 2x

TEST(ThreadStatesPerf, MediumBuildTime) {
  auto loaded =
      model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel& m = *loaded;
  double best = 1e30;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(buildThreadStates(m).ok());
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[analysis perf] medium buildThreadStates #%d: %.4f s\n", i, s);
    best = std::min(best, s);
  }
  size_t intervals = 0, bytes = 0;
  for (const auto& v : m.statesByThread) {
    intervals += v.size();
    bytes += v.capacity() * sizeof(model::ThreadStateInterval);
  }
  std::printf(
      "[analysis perf] medium best: %.4f s (target %.2f s, assert %.2f s, %u hw threads); %zu threads, %zu "
      "context switches, %zu ready events, %zu intervals, %.2f MiB (%zu B/interval)\n",
      best, kBudgetSeconds, kBudgetSeconds * 2, std::thread::hardware_concurrency(), m.threads.size(),
      m.contextSwitches.size(), m.readyEvents.size(), intervals, double(bytes) / (1024.0 * 1024.0),
      sizeof(model::ThreadStateInterval));
  EXPECT_LT(best, kBudgetSeconds * 2);
}

}  // namespace
}  // namespace tachyon::analysis
