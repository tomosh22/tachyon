// WP-66: the testbed accepts a starved thread (it measured nothing because an overloaded machine gave it no
// cpu while its scenario ran) as a warning instead of an invalid report.
#include "testbed/report.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace tachyon {
namespace {

using nlohmann::json;
using testbed::ScenarioReport;
using testbed::ThreadReport;

testbed::RunReport report(bool starvedThread) {
  ThreadReport healthy;
  healthy.name = "tb-spin";
  healthy.tid = 1234;
  healthy.expected = testbed::makeExpected({{"hot_a", 0.6}, {"hot_b", 0.4}});
  healthy.measured = testbed::makeMeasured({{"hot_a", 610}, {"hot_b", 390}});
  ThreadReport starved;
  starved.name = "tb-lock-1";
  starved.tid = 1235;
  starved.expected = testbed::makeExpected({{"critical", 0.5}, {"wait", 0.5}});
  starved.measured = testbed::makeMeasured(
      starvedThread ? std::vector<std::pair<std::string, uint64_t>>{{"critical", 0}, {"wait", 0}}
                    : std::vector<std::pair<std::string, uint64_t>>{{"critical", 5}, {"wait", 5}});
  ScenarioReport scenario;
  scenario.name = "lock";
  scenario.description = "sample";
  scenario.plannedDurationS = 1.0;
  scenario.actualDurationS = 1.0;
  scenario.threads = {healthy, starved};
  testbed::RunReport run;
  run.platform = "linux";
  run.pid = 42;
  run.requestedDurationS = 1.0;
  run.clockFrequencyHz = 1'000'000'000;
  run.tscFrequencyHz = 2'000'000'000;
  run.kernelItersPerSecond = 4e8;
  run.scenarios.push_back(scenario);
  return run;
}

TEST(TestbedStarvedThread, IsAWarningNotAnInvalidReport) {
  const json j = testbed::toJson(report(/*starvedThread=*/true));
  // The shares of that thread are all zero: that used to fail with "measured shares sum to 0".
  std::vector<std::string> warnings;
  auto valid = testbed::validateReportJson(j, &warnings);
  ASSERT_TRUE(valid.ok()) << valid.error().toString();
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("lock"), std::string::npos) << warnings[0];
  EXPECT_NE(warnings[0].find("tb-lock-1"), std::string::npos) << warnings[0];
  EXPECT_NE(warnings[0].find("overloaded"), std::string::npos) << warnings[0];
  // The single-argument form accepts it too, and a null warning list is fine.
  EXPECT_TRUE(testbed::validateReportJson(j).ok());
  EXPECT_TRUE(testbed::validateReportJson(j, nullptr).ok());
}

TEST(TestbedStarvedThread, AHealthyReportHasNoWarnings) {
  const json j = testbed::toJson(report(/*starvedThread=*/false));
  std::vector<std::string> warnings;
  EXPECT_TRUE(testbed::validateReportJson(j, &warnings).ok());
  EXPECT_TRUE(warnings.empty());
}

TEST(TestbedStarvedThread, OtherBrokenSharesAreStillInvalid) {
  // Zero shares but non-zero ticks: not starved, just wrong.
  json j = testbed::toJson(report(/*starvedThread=*/true));
  j["scenarios"][0]["threads"][1]["measured"][0]["tsc_ticks"] = 7;
  EXPECT_FALSE(testbed::validateReportJson(j).ok());
  // Shares that do not add up.
  j = testbed::toJson(report(false));
  j["scenarios"][0]["threads"][1]["measured"][0]["share"] = 0.9;
  EXPECT_FALSE(testbed::validateReportJson(j).ok());
  // The expected list must still sum to 1: a starved measurement does not excuse it.
  j = testbed::toJson(report(true));
  j["scenarios"][0]["threads"][1]["expected"][0]["share"] = 0.1;
  EXPECT_FALSE(testbed::validateReportJson(j).ok());
  // A measured entry without ticks is malformed even when every share is zero.
  j = testbed::toJson(report(true));
  j["scenarios"][0]["threads"][1]["measured"][0].erase("tsc_ticks");
  EXPECT_FALSE(testbed::validateReportJson(j).ok());
}

}  // namespace
}  // namespace tachyon
