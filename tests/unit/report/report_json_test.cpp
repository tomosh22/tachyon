// WP-62: report JSON schema, round trip and determinism.
#include "report/report_json.h"

#include "report/markdown.h"
#include "report_test_util.h"

#include <gtest/gtest.h>

namespace tachyon::report {
namespace {

using testutil::Loaded;

Report reportOf(const std::string& fixture, ReportOptions o = {}) {
  Loaded l(fixture);
  auto r = buildReport(l.m, l.svc, o);
  EXPECT_TRUE(r.ok());
  return r.ok() ? std::move(*r) : Report{};
}

TEST(ReportJson, FixtureReportsValidateAgainstTheSchema) {
  const auto schema = testutil::reportSchema();
  for (const char* fixture : {"frames", "scopes", "medium", "tiny", "partial_torn"}) {
    const Report r = reportOf(fixture);
    const auto errors = schema.validate(nlohmann::json::parse(dumpJson(reportToJson(r))));
    EXPECT_TRUE(errors.empty()) << fixture << ": " << (errors.empty() ? "" : errors.front());
  }
}

TEST(ReportJson, ReportWithBaselineDiffValidates) {
  const auto schema = testutil::reportSchema();
  const Report base = reportOf("frames");
  Report cur = base;
  cur.frames[0].p95Ms *= 2;
  cur.frames[0].p99Ms *= 2;
  cur.trace.eventsLost = 5;
  cur.functions[0].exclusivePct += 10;
  const Diff d = compareReports(base, cur, {});
  ASSERT_TRUE(d.regressed());
  const nlohmann::json doc = nlohmann::json::parse(dumpJson(reportToJson(cur, &d)));
  const auto errors = schema.validate(doc);
  EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
  EXPECT_EQ(doc["baseline_diff"]["verdict"], "regression");
  EXPECT_EQ(doc["baseline_diff"]["regression_count"], d.regressions.size());
}

TEST(ReportJson, SchemaValidatorRejectsBrokenDocuments) {
  const auto schema = testutil::reportSchema();
  nlohmann::json doc = nlohmann::json::parse(dumpJson(reportToJson(reportOf("frames"))));
  ASSERT_TRUE(schema.validate(doc).empty());
  nlohmann::json missing = doc;
  missing["frames"].erase("series_total");
  EXPECT_FALSE(schema.validate(missing).empty());
  nlohmann::json wrongType = doc;
  wrongType["trace"]["events_lost"] = "none";
  EXPECT_FALSE(schema.validate(wrongType).empty());
  nlohmann::json extra = doc;
  extra["threads"][0]["bogus"] = 1;
  EXPECT_FALSE(schema.validate(extra).empty());
  nlohmann::json badVersion = doc;
  badVersion["schema_version"] = 2;
  EXPECT_FALSE(schema.validate(badVersion).empty());
  nlohmann::json negative = doc;
  negative["frames"]["series"][0]["p95_ms"] = -1;
  EXPECT_FALSE(schema.validate(negative).empty());
}

TEST(ReportJson, RoundTripIsLossless) {
  for (const char* fixture : {"frames", "scopes"}) {
    const Report r = reportOf(fixture);
    const std::string text = dumpJson(reportToJson(r));
    auto back = reportFromJson(nlohmann::json::parse(text));
    ASSERT_TRUE(back.ok()) << back.error().toString();
    EXPECT_EQ(dumpJson(reportToJson(*back)), text) << fixture;
  }
}

TEST(ReportJson, ReaderIgnoresBaselineDiffAndRejectsOtherVersions) {
  const Report r = reportOf("frames");
  const Diff d = compareReports(r, r, {});
  const nlohmann::json withDiff = nlohmann::json::parse(dumpJson(reportToJson(r, &d)));
  EXPECT_TRUE(reportFromJson(withDiff).ok());

  nlohmann::json v2 = withDiff;
  v2["schema_version"] = 2;
  auto bad = reportFromJson(v2);
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().code, ErrorCode::CorruptData);
  EXPECT_NE(bad.error().message.find("schema_version"), std::string::npos);

  nlohmann::json broken = withDiff;
  broken["functions"]["top"][0].erase("exclusive_pct");
  auto b2 = reportFromJson(broken);
  ASSERT_FALSE(b2.ok());
  EXPECT_NE(b2.error().message.find("exclusive_pct"), std::string::npos);

  EXPECT_FALSE(reportFromJson(nlohmann::json::parse("[1]")).ok());
  EXPECT_FALSE(reportFromJson(nlohmann::json::parse("{}")).ok());
}

// WP-69: a baseline written before the baseline function set and total CPU existed still loads (and the
// fields are absent from the result), and one with them round-trips.
TEST(ReportJson, OldBaselinesWithoutTheNewFieldsStillLoad) {
  const Report r = reportOf("medium");
  nlohmann::json doc = nlohmann::json::parse(dumpJson(reportToJson(r)));
  ASSERT_TRUE(doc["functions"].contains("significant"));
  ASSERT_TRUE(doc["trace"].contains("total_cpu_ms"));
  EXPECT_TRUE(testutil::reportSchema().validate(doc).empty());
  doc["functions"].erase("significant");
  doc["functions"].erase("significant_floor_pct");
  doc["trace"].erase("total_cpu_ms");
  EXPECT_TRUE(testutil::reportSchema().validate(doc).empty());  // the schema keeps them optional
  auto old = reportFromJson(doc);
  ASSERT_TRUE(old.ok()) << old.error().toString();
  EXPECT_LT(old->significantFloorPct, 0);
  EXPECT_TRUE(old->significantFunctions.empty());
  EXPECT_EQ(old->trace.totalCpuMs, 0.0);
  EXPECT_EQ(old->functions.size(), r.functions.size());

  nlohmann::json bad = nlohmann::json::parse(dumpJson(reportToJson(r)));
  bad["functions"].erase("significant_floor_pct");
  EXPECT_FALSE(reportFromJson(bad).ok());  // the set without its floor is malformed
  nlohmann::json badRow = nlohmann::json::parse(dumpJson(reportToJson(r)));
  badRow["functions"]["significant"][0].erase("exclusive_ms");
  EXPECT_FALSE(reportFromJson(badRow).ok());
}

TEST(ReportJson, NewFieldsRoundTrip) {
  const Report r = reportOf("medium");
  ASSERT_FALSE(r.significantFunctions.empty());
  const std::string text = dumpJson(reportToJson(r));
  auto back = reportFromJson(nlohmann::json::parse(text));
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->significantFunctions.size(), r.significantFunctions.size());
  EXPECT_EQ(back->significantFloorPct, r.significantFloorPct);
  EXPECT_EQ(back->trace.totalCpuMs, r.trace.totalCpuMs);
}

TEST(ReportJson, TwoIndependentRunsAreByteIdentical) {
  for (const char* fixture : {"frames", "scopes", "medium"}) {
    const Report a = reportOf(fixture), b = reportOf(fixture);
    EXPECT_EQ(dumpJson(reportToJson(a)), dumpJson(reportToJson(b))) << fixture;
    EXPECT_EQ(renderMarkdown(a, nullptr), renderMarkdown(b, nullptr)) << fixture;
  }
}

TEST(ReportJson, InvalidUtf8InNamesDoesNotThrow) {
  Report r = reportOf("frames");
  r.functions[0].name = std::string("bad\xff\xfe name");
  const std::string text = dumpJson(reportToJson(r));
  EXPECT_FALSE(text.empty());
  EXPECT_FALSE(nlohmann::json::parse(text, nullptr, false).is_discarded());
}

}  // namespace
}  // namespace tachyon::report
