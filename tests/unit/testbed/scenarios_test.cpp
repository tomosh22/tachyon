#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <set>

#include "testbed/scenarios.h"

using namespace tachyon::testbed;

TEST(TestbedScenarios, FrameExpectationSumsToOneAndHitchShareMatchesModel) {
  const FrameModel model;  // 60 Hz, 3 ms update, 4 ms render, 50 ms hitch every 2 s
  const FrameExpectation e = frameExpectation(model);
  EXPECT_NEAR(e.update + e.render + e.hitch + e.wait, 1.0, 1e-12);
  EXPECT_EQ(e.framesPerHitch, 120u);
  // 119 normal frames + one 57 ms hitch frame.
  EXPECT_NEAR(e.cycleMs, 119.0 * model.periodMs + 57.0, 1e-9);
  EXPECT_NEAR(e.hitch, 50.0 / e.cycleMs, 1e-12);
  EXPECT_NEAR(e.update, 120.0 * 3.0 / e.cycleMs, 1e-12);
  EXPECT_GT(e.wait, 0.4);
}

TEST(TestbedScenarios, FrameExpectationWithoutOverrunHasNoNegativeWait) {
  FrameModel model;
  model.hitchMs = 1.0;  // hitch frame still fits inside the period
  const FrameExpectation e = frameExpectation(model);
  EXPECT_NEAR(e.update + e.render + e.hitch + e.wait, 1.0, 1e-12);
  EXPECT_GE(e.wait, 0.0);
}

TEST(TestbedScenarios, LockExpectationSumsToOne) {
  const LockExpectation e = lockExpectation(LockModel{});
  EXPECT_NEAR(e.critical + e.outside + e.wait, 1.0, 1e-12);
  EXPECT_NEAR(e.critical, 0.5, 1e-12);
  EXPECT_NEAR(e.outside, 0.1, 1e-12);
  EXPECT_NEAR(e.wait, 0.4, 1e-12);
}

TEST(TestbedScenarios, PlanDurationsSplitsTotalAndWeightsFrames) {
  const auto names = scenarioNames();
  ASSERT_EQ(names.size(), 9u);
  const auto durations = planDurations(names, 5.0);
  EXPECT_NEAR(std::accumulate(durations.begin(), durations.end(), 0.0), 5.0, 1e-12);
  const auto frames = std::find(names.begin(), names.end(), "frames") - names.begin();
  EXPECT_NEAR(durations[static_cast<size_t>(frames)], 4.0 * durations[0], 1e-12);
}

TEST(TestbedScenarios, ScenarioNamesCoverTheSpec) {
  const auto names = scenarioNames();
  const std::set<std::string> expected = {"spin",   "sleep",  "lock", "io",    "recursion",
                                          "inline", "fibers", "jobs", "frames"};
  EXPECT_EQ(std::set<std::string>(names.begin(), names.end()), expected);
}

TEST(TestbedScenarios, UnknownScenarioIsAnError) {
  const Calibration cal = calibrate(0.01, 0.005);
  auto result = runScenario("nope", RunConfig{}, cal);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, tachyon::ErrorCode::InvalidArgument);
}

TEST(TestbedScenarios, EveryScenarioRunsAndProducesValidReport) {
  const Calibration cal = calibrate(0.02, 0.01);
  auto run = runScenarios(scenarioNames(), 0.6, 0.0, cal);
  ASSERT_TRUE(run.ok()) << run.error().toString();
  const nlohmann::json j = toJson(run.value());
  const auto valid = validateReportJson(j);
  EXPECT_TRUE(valid.ok()) << (valid.ok() ? "" : valid.error().toString());
  ASSERT_EQ(run.value().scenarios.size(), 9u);

  std::set<uint64_t> tids;
  for (const auto& scenario : run.value().scenarios) {
    for (const auto& thread : scenario.threads) {
      EXPECT_EQ(thread.name.rfind("tb-", 0), 0u) << thread.name;
      EXPECT_NE(thread.tid, 0u);
      tids.insert(thread.tid);
    }
    if (scenario.name == "jobs") EXPECT_EQ(scenario.threads.size(), 8u);
    if (scenario.name == "lock") EXPECT_EQ(scenario.threads.size(), 2u);
    if (scenario.name == "fibers") EXPECT_EQ(scenario.threads[0].measured.size(), 4u);
  }
  EXPECT_GE(tids.size(), 9u);

  // Spot-check enforced scenarios loosely (noisy machine): fibers get equal slices, inline follows 5:3:2.
  for (const auto& scenario : run.value().scenarios) {
    if (scenario.name != "fibers" && scenario.name != "inline") continue;
    for (size_t i = 0; i < scenario.threads[0].measured.size(); ++i) {
      EXPECT_NEAR(scenario.threads[0].measured[i].share, scenario.threads[0].expected[i].share, 0.15)
          << scenario.name << " " << scenario.threads[0].measured[i].function;
    }
  }
}
