#include "core/time.h"

#include <gtest/gtest.h>

namespace tachyon::core {
namespace {

TEST(Time, MonotonicAndFrequency) {
  const uint64_t a = nowTicks();
  const uint64_t b = nowTicks();
  EXPECT_LE(a, b);
  EXPECT_GT(ticksFrequency(), 0u);
}

TEST(Time, ConversionsAvoidOverflow) {
  constexpr uint64_t qpc = 10'000'000;  // typical Windows QPC frequency
  static_assert(ticksToNs(qpc, qpc) == 1'000'000'000ull);
  static_assert(nsToTicks(1'000'000'000ull, qpc) == qpc);
  // Ten days of ticks at 10 MHz would overflow a naive ticks*1e9.
  const uint64_t tenDays = 10ull * 86400 * qpc;
  EXPECT_EQ(ticksToNs(tenDays, qpc), 10ull * 86400 * 1'000'000'000ull);
  EXPECT_EQ(ticksToNs(12345, 1'000'000'000ull), 12345u);
  EXPECT_DOUBLE_EQ(ticksToSeconds(qpc / 2, qpc), 0.5);
}

}  // namespace
}  // namespace tachyon::core
