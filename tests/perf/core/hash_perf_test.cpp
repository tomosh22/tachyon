// Example perf budget test (Part 2.4): budgets are asserted at 2x the target so CI fails on a 2x regression.
#include "core/hash.h"
#include "core/time.h"

#include <gtest/gtest.h>

namespace tachyon::core {
namespace {

TEST(HashPerf, PairHashThroughput) {
  constexpr uint64_t kIterations = 50'000'000;
  constexpr double kBudgetSeconds = 0.5;  // target; assert at 2x
  const uint64_t t0 = nowTicks();
  uint64_t acc = 0;
  for (uint64_t i = 0; i < kIterations; ++i)
    acc += hashU32Pair(static_cast<uint32_t>(i), static_cast<uint32_t>(i >> 7));
  const double seconds = ticksToSeconds(nowTicks() - t0, ticksFrequency());
  RecordProperty("seconds", std::to_string(seconds));
  EXPECT_NE(acc, 0u);
  EXPECT_LT(seconds, 2 * kBudgetSeconds);
}

}  // namespace
}  // namespace tachyon::core
