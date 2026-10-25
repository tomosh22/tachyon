// SDK overhead budgets (Part 2.4): median Begin+End pair cost on one thread, measured in batches with
// core::nowTicks. Targets: < 40 ns connected, < 10 ns runtime-disabled; asserted at 2x (80 / 20 ns).
#include "core/time.h"
#include "fake_agent.h"
#include "sdk_internal.h"
#include "test_env.h"

#include <gtest/gtest.h>

#include <tachyon/tachyon.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace tachyon {
namespace {

// Median nanoseconds per Begin+End pair. `pace` sleeps between batches so the ring never fills (the
// connected case must measure the recording path, not the drop path).
double medianPairNs(int batches, int pairsPerBatch, bool pace) {
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(batches));
  const double ticksToNs = 1e9 / static_cast<double>(core::ticksFrequency());
  for (int b = 0; b < batches + 200; ++b) {  // 200 warm-up batches
    const uint64_t t0 = core::nowTicks();
    for (int i = 0; i < pairsPerBatch; ++i) {
      Tachyon_BeginEvent("perf-scope", nullptr, TACHYON_DEFAULT_COLOR);
      (void)Tachyon_EndEvent();
    }
    const uint64_t t1 = core::nowTicks();
    if (b >= 200) samples.push_back(static_cast<double>(t1 - t0) * ticksToNs / pairsPerBatch);
    if (pace) std::this_thread::sleep_for(std::chrono::microseconds(150));
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

TEST(SdkOverheadPerf, ConnectedBeginEndPairUnder40ns) {
  constexpr double kTargetNs = 40.0;
  sdk_testing::unsetEnv("TACHYON_DISABLE");
  sdk_testing::unsetEnv("TACHYON_RING_KB");
  sdk_testing::FakeAgent agent(sdk_testing::uniqueAgentAddress("perf"), [](const uint8_t*, size_t) {});
  ASSERT_TRUE(agent.ok());
  sdk_testing::setEnv("TACHYON_AGENT_PIPE", agent.address());
  sdk_testing::setEnv("TACHYON_SIDECAR", "off");
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 1);
  const double ns = medianPairNs(3000, 100, true);
  const uint64_t drops = Tachyon_Internal_ThreadDropCount();
  std::printf(
      "[perf] sdk connected Begin+End median: %.1f ns per pair (target %.0f, asserted %.0f), drops=%llu\n",
      ns, kTargetNs, 2 * kTargetNs, static_cast<unsigned long long>(drops));
  RecordProperty("median_ns", std::to_string(ns));
  EXPECT_EQ(drops, 0u) << "measured the drop path, not the recording path";
  EXPECT_LT(ns, 2 * kTargetNs);
  ADD_FAILURE() << "TEMPORARY PROBE connected median ns=" << ns;
  Tachyon_Internal_Restart();
}

TEST(SdkOverheadPerf, RuntimeDisabledBeginEndPairUnder10ns) {
  constexpr double kTargetNs = 10.0;
  sdk_testing::setEnv("TACHYON_DISABLE", "1");
  Tachyon_Internal_Restart();
  const double ns = medianPairNs(2000, 1000, false);
  std::printf("[perf] sdk disabled Begin+End median: %.2f ns per pair (target %.0f, asserted %.0f)\n", ns,
              kTargetNs, 2 * kTargetNs);
  RecordProperty("median_ns", std::to_string(ns));
  EXPECT_LT(ns, 2 * kTargetNs);
  ADD_FAILURE() << "TEMPORARY PROBE disabled median ns=" << ns;
  sdk_testing::unsetEnv("TACHYON_DISABLE");
  Tachyon_Internal_Restart();
}

}  // namespace
}  // namespace tachyon
