#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "testbed/calibration.h"
#include "testbed/scenarios.h"

using namespace tachyon::testbed;

TEST(TestbedCalibration, LoopCountsAreProportionalToShares) {
  const double shares[4] = {0.4, 0.2, 0.1, 0.3};
  const auto counts = loopCountsForShares(shares, 2'000'000, 0.5);  // 1e6 iterations per cycle
  ASSERT_EQ(counts.size(), 4u);
  EXPECT_EQ(counts[0], 400'000u);
  EXPECT_EQ(counts[1], 200'000u);
  EXPECT_EQ(counts[2], 100'000u);
  EXPECT_EQ(counts[3], 300'000u);
  EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), uint64_t{0}), 1'000'000u);
}

TEST(TestbedCalibration, ZeroAndRoundingBehaviour) {
  const double shares[3] = {0.0, 1.0 / 3.0, 2.0 / 3.0};
  const auto counts = loopCountsForShares(shares, 3, 1.0);
  EXPECT_EQ(counts[0], 0u);
  EXPECT_EQ(counts[1], 1u);
  EXPECT_EQ(counts[2], 2u);
  EXPECT_TRUE(loopCountsForShares({}, 100, 1.0).empty());
}

TEST(TestbedCalibration, CalibrateProducesPlausibleNumbers) {
  const Calibration cal = calibrate(0.02, 0.01);
  EXPECT_GT(cal.traceFrequencyHz, 0u);
  EXPECT_GT(cal.tscHz, 100'000'000u);  // > 100 MHz
  EXPECT_GT(cal.itersPerTsc, 0.0);
  EXPECT_GT(cal.itersPerSecond(), 1e6);
  EXPECT_EQ(secondsToTsc(cal, 1.0), cal.tscHz);
  EXPECT_NEAR(tscToSeconds(cal, cal.tscHz / 2), 0.5, 1e-6);
}

TEST(TestbedCalibration, SpinWorkerHitsRequestedSplit) {
  const Calibration cal = calibrate(0.02, 0.01);
  // Timing on a shared machine is noisy: allow a few attempts and a generous absolute tolerance. Preemption
  // only perturbs the measured split; the enforced loop counts are what this test is about.
  constexpr double kTolerance = 0.10;
  double best = 1.0;
  bool passed = false;
  for (int attempt = 0; attempt < 4 && !passed; ++attempt) {
    const ThreadReport report = spinWorker(cal, 0.25);
    ASSERT_EQ(report.measured.size(), 4u);
    ASSERT_EQ(report.expected.size(), 4u);
    double sum = 0.0, maxError = 0.0;
    for (size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(report.measured[i].function, report.expected[i].function);
      sum += report.measured[i].share;
      maxError = std::max(maxError, std::fabs(report.measured[i].share - report.expected[i].share));
    }
    EXPECT_NEAR(sum, 1.0, 1e-9);
    best = std::min(best, maxError);
    passed = maxError <= kTolerance;
  }
  EXPECT_TRUE(passed) << "best attempt had max share error " << best;
}

TEST(TestbedCalibration, SpinWorkerExpectedNamesAreTheHotFunctions) {
  const Calibration cal = calibrate(0.02, 0.01);
  const ThreadReport report = spinWorker(cal, 0.02);
  ASSERT_EQ(report.expected.size(), 4u);
  EXPECT_EQ(report.expected[0].function, "hot_a");
  EXPECT_EQ(report.expected[1].function, "hot_b");
  EXPECT_EQ(report.expected[2].function, "hot_c");
  EXPECT_EQ(report.expected[3].function, "spin_rest");
  EXPECT_DOUBLE_EQ(report.expected[0].share, 0.40);
  EXPECT_DOUBLE_EQ(report.expected[1].share, 0.20);
  EXPECT_DOUBLE_EQ(report.expected[2].share, 0.10);
  EXPECT_DOUBLE_EQ(report.expected[3].share, 0.30);
}
