#include <gtest/gtest.h>

#include "testbed/report.h"

using namespace tachyon::testbed;
using nlohmann::json;

namespace {

RunReport sampleReport() {
  ThreadReport thread;
  thread.name = "tb-spin";
  thread.tid = 1234;
  thread.expected = makeExpected({{"hot_a", 0.4}, {"hot_b", 0.2}, {"hot_c", 0.1}, {"spin_rest", 0.3}});
  thread.measured = makeMeasured({{"hot_a", 400}, {"hot_b", 201}, {"hot_c", 99}, {"spin_rest", 300}});
  ScenarioReport scenario;
  scenario.name = "spin";
  scenario.description = "sample";
  scenario.plannedDurationS = 1.0;
  scenario.actualDurationS = 1.01;
  scenario.threads.push_back(thread);
  RunReport run;
  run.platform = "linux";
  run.pid = 42;
  run.requestedDurationS = 1.0;
  run.clockFrequencyHz = 1'000'000'000;
  run.tscFrequencyHz = 2'000'000'000;
  run.kernelItersPerSecond = 4e8;
  run.scenarios.push_back(scenario);
  return run;
}

}  // namespace

TEST(TestbedReport, MakeMeasuredSharesSumToOne) {
  const auto measured = makeMeasured({{"a", 1}, {"b", 2}, {"c", 3}});
  double sum = 0.0;
  for (const auto& m : measured) sum += m.share;
  EXPECT_NEAR(sum, 1.0, 1e-12);
  EXPECT_EQ(measured[2].tscTicks, 3u);
  EXPECT_NEAR(measured[2].share, 0.5, 1e-12);
}

TEST(TestbedReport, ZeroTotalGivesZeroShares) {
  const auto measured = makeMeasured({{"a", 0}, {"b", 0}});
  EXPECT_EQ(measured[0].share, 0.0);
}

TEST(TestbedReport, EmittedJsonMatchesDocumentedSchema) {
  const json j = toJson(sampleReport());
  EXPECT_EQ(j["schema"], "tachyon-testbed/1");
  EXPECT_EQ(j["tool"], "tachyon-testbed");
  EXPECT_EQ(j["platform"], "linux");
  EXPECT_EQ(j["pid"], 42u);
  EXPECT_EQ(j["clock_frequency_hz"], 1'000'000'000u);
  EXPECT_EQ(j["tsc_frequency_hz"], 2'000'000'000u);
  ASSERT_EQ(j["scenarios"].size(), 1u);
  const json& scenario = j["scenarios"][0];
  EXPECT_EQ(scenario["name"], "spin");
  EXPECT_TRUE(scenario["details"].is_object());
  const json& thread = scenario["threads"][0];
  EXPECT_EQ(thread["name"], "tb-spin");
  EXPECT_EQ(thread["tid"], 1234u);
  EXPECT_TRUE(thread["enforced"].is_boolean());
  EXPECT_EQ(thread["expected"][0]["function"], "hot_a");
  EXPECT_DOUBLE_EQ(thread["expected"][0]["share"].get<double>(), 0.4);
  EXPECT_EQ(thread["measured"][0]["tsc_ticks"], 400u);

  for (const char* key : {"expected", "measured"}) {
    double sum = 0.0;
    for (const auto& entry : thread[key]) sum += entry["share"].get<double>();
    EXPECT_NEAR(sum, 1.0, 1e-9) << key;
  }
  EXPECT_TRUE(validateReportJson(j).ok());
}

TEST(TestbedReport, SurvivesTextRoundTrip) {
  const json j = json::parse(toJson(sampleReport()).dump());
  EXPECT_TRUE(validateReportJson(j).ok());
}

TEST(TestbedReport, ValidationRejectsBrokenReports) {
  const json good = toJson(sampleReport());

  json badSum = good;
  badSum["scenarios"][0]["threads"][0]["expected"][0]["share"] = 0.5;
  EXPECT_FALSE(validateReportJson(badSum).ok());

  json badMeasured = good;
  badMeasured["scenarios"][0]["threads"][0]["measured"][1]["share"] = 0.9;
  EXPECT_FALSE(validateReportJson(badMeasured).ok());

  json noSchema = good;
  noSchema.erase("schema");
  EXPECT_FALSE(validateReportJson(noSchema).ok());

  json noThreads = good;
  noThreads["scenarios"][0]["threads"] = json::array();
  EXPECT_FALSE(validateReportJson(noThreads).ok());

  json zeroTid = good;
  zeroTid["scenarios"][0]["threads"][0]["tid"] = 0;
  EXPECT_FALSE(validateReportJson(zeroTid).ok());

  json noTicks = good;
  noTicks["scenarios"][0]["threads"][0]["measured"][0].erase("tsc_ticks");
  EXPECT_FALSE(validateReportJson(noTicks).ok());

  EXPECT_FALSE(validateReportJson(json::array()).ok());
}
