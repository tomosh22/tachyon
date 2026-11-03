// WP-62: baseline diff on synthetic reports, thresholds parsing.
#include "report/baseline.h"

#include "report/report_json.h"

#include <gtest/gtest.h>

namespace tachyon::report {
namespace {

FrameSeriesRow series(const std::string& label, double p95, double p99) {
  FrameSeriesRow s;
  s.label = label;
  s.name = label;
  s.frameCount = 100;
  s.markCount = 101;
  s.p50Ms = p95 / 2;
  s.p95Ms = p95;
  s.p99Ms = p99;
  s.maxMs = p99;
  return s;
}

FunctionReportRow fn(const std::string& name, double pct, const std::string& module = "game.exe") {
  FunctionReportRow f;
  f.name = name;
  f.module = module;
  f.exclusivePct = pct;
  f.exclusiveMs = pct;
  f.inclusiveMs = pct;
  return f;
}

ScopeReportRow scope(const std::string& name, double median) {
  ScopeReportRow s;
  s.name = name;
  s.count = 10;
  s.medianMs = median;
  s.p95Ms = median;
  s.maxMs = median;
  s.minMs = median;
  s.totalMs = median * 10;
  return s;
}

Report baseReport() {
  Report r;
  r.trace.platform = "linux";
  r.settings.topN = 20;
  r.frames.push_back(series("Frame@Main", 16.0, 20.0));
  r.functions = {fn("A", 40), fn("B", 30), fn("C", 20)};
  r.scopes = {scope("Update", 4.0), scope("Render", 0.5)};
  return r;
}

const Finding* find(const std::vector<Finding>& v, const std::string& category,
                    const std::string& subject = "") {
  for (const Finding& f : v)
    if (f.category == category && (subject.empty() || f.subject == subject)) return &f;
  return nullptr;
}

TEST(BaselineDiff, IdenticalReportsHaveNoFindings) {
  const Report r = baseReport();
  const Diff d = compareReports(r, r, {});
  EXPECT_FALSE(d.regressed());
  EXPECT_TRUE(d.improvements.empty());
  EXPECT_TRUE(d.notes.empty());
  ASSERT_EQ(d.frames.size(), 1u);
  EXPECT_EQ(d.frames[0].p95Ms, 16.0);
}

TEST(BaselineDiff, FrameP95RegressionNeedsBothRelativeAndAbsoluteChange) {
  const Report base = baseReport();
  Report cur = base;
  cur.frames[0].p95Ms = 19.2;  // +20 %, +3.2 ms
  Diff d = compareReports(base, cur, {});
  ASSERT_TRUE(d.regressed());
  const Finding* f = find(d.regressions, "frame_p95", "Frame@Main");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->baseline, 16.0);
  EXPECT_EQ(f->current, 19.2);
  ASSERT_TRUE(f->change.has_value());
  EXPECT_NEAR(*f->change, 20.0, 1e-9);
  EXPECT_EQ(f->changeUnit, "%");
  EXPECT_EQ(find(d.regressions, "frame_p99"), nullptr);

  cur.frames[0].p95Ms = 17.0;  // +6.25 %: within 10 %
  EXPECT_FALSE(compareReports(base, cur, {}).regressed());

  Report tiny = base;  // +50 % but only +0.4 ms: below the 0.5 ms floor
  tiny.frames[0].p95Ms = 0.8;
  Report tinyCur = tiny;
  tinyCur.frames[0].p95Ms = 1.2;
  EXPECT_FALSE(compareReports(tiny, tinyCur, {}).regressed());
}

TEST(BaselineDiff, ExactlyAtTheThresholdIsNotARegression) {
  Report base = baseReport();
  base.frames[0].p95Ms = 20.0;
  Report cur = base;
  cur.frames[0].p95Ms = 22.0;  // exactly +10 %
  EXPECT_FALSE(compareReports(base, cur, {}).regressed());
  cur.frames[0].p95Ms = 22.01;
  EXPECT_TRUE(compareReports(base, cur, {}).regressed());
}

TEST(BaselineDiff, FrameP99AndImprovements) {
  const Report base = baseReport();
  Report cur = base;
  cur.frames[0].p99Ms = 30.0;  // +50 %
  cur.frames[0].p95Ms = 12.0;  // -25 %: an improvement
  const Diff d = compareReports(base, cur, {});
  ASSERT_NE(find(d.regressions, "frame_p99"), nullptr);
  EXPECT_EQ(find(d.regressions, "frame_p95"), nullptr);
  const Finding* imp = find(d.improvements, "frame_p95");
  ASSERT_NE(imp, nullptr);
  EXPECT_NEAR(*imp->change, -25.0, 1e-9);
  EXPECT_EQ(d.regressions.size(), 1u);
}

TEST(BaselineDiff, FunctionExclusiveShareInPercentagePoints) {
  const Report base = baseReport();
  Report cur = base;
  cur.functions = {fn("A", 43.5), fn("B", 27), fn("C", 20)};  // A +3.5 pp, B -3 pp
  Diff d = compareReports(base, cur, {});
  const Finding* r = find(d.regressions, "function_exclusive", "game.exe!A");
  ASSERT_NE(r, nullptr);
  EXPECT_NEAR(*r->change, 3.5, 1e-9);
  EXPECT_EQ(r->changeUnit, "pp");
  EXPECT_EQ(r->valueUnit, "% of total");
  ASSERT_NE(find(d.improvements, "function_exclusive", "game.exe!B"), nullptr);
  EXPECT_EQ(find(d.regressions, "function_exclusive", "game.exe!C"), nullptr);

  cur.functions = {fn("A", 41.5), fn("B", 30), fn("C", 20)};  // +1.5 pp: within 2 pp
  EXPECT_FALSE(compareReports(base, cur, {}).regressed());
}

TEST(BaselineDiff, FunctionsMatchByNameAndModule) {
  Report base = baseReport();
  base.functions = {fn("A", 40, "game.exe"), fn("A", 10, "render.dll")};
  Report cur = base;
  cur.functions = {fn("A", 40, "game.exe"), fn("A", 14, "render.dll")};
  const Diff d = compareReports(base, cur, {});
  ASSERT_EQ(d.regressions.size(), 1u);
  EXPECT_EQ(d.regressions[0].subject, "render.dll!A");
}

TEST(BaselineDiff, NewFunctionWithBigShareRegressesAgainstACompleteBaseline) {
  const Report base = baseReport();  // 3 of 20 listed: complete, an unlisted function had 0 %
  Report cur = base;
  cur.functions = {fn("A", 40), fn("NewHot", 12), fn("B", 18)};
  const Diff d = compareReports(base, cur, {});
  const Finding* f = find(d.regressions, "function_exclusive", "game.exe!NewHot");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->baseline, 0.0);
  EXPECT_TRUE(f->baselineIsBound);
  EXPECT_FALSE(f->currentIsBound);
  // C (20 -> unlisted, bound 0 in a complete list) is reported as an improvement; B dropped 12 pp.
  EXPECT_NE(find(d.improvements, "function_exclusive", "game.exe!B"), nullptr);
}

TEST(BaselineDiff, UnlistedFunctionsAreBoundedByTheSmallestListedShare) {
  Report base = baseReport();
  base.settings.topN = 3;  // the list is full: unlisted functions have at most 20 %
  Report cur = base;
  cur.functions = {fn("A", 40), fn("B", 30), fn("NewHot", 21)};  // C fell out of the top 3
  const Diff d = compareReports(base, cur, {});
  // NewHot: baseline bound 20 -> 21 is within 2 pp: no regression. C: 20 -> bound 21: no improvement claimed.
  EXPECT_FALSE(d.regressed());
  EXPECT_TRUE(d.improvements.empty());
  cur.functions = {fn("A", 40), fn("B", 30), fn("NewHot", 25)};
  const Diff d2 = compareReports(base, cur, {});
  const Finding* f = find(d2.regressions, "function_exclusive", "game.exe!NewHot");
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->baselineIsBound);
  EXPECT_EQ(f->baseline, 20.0);
}

TEST(BaselineDiff, ScopeMedianRegression) {
  const Report base = baseReport();
  Report cur = base;
  cur.scopes = {scope("Update", 6.0), scope("Render", 0.9)};  // Update +50 %; Render +80 % but +0.4 ms
  Diff d = compareReports(base, cur, {});
  ASSERT_EQ(d.regressions.size(), 2u);  // Render exceeds the 0.05 ms floor as well
  EXPECT_EQ(d.regressions[0].category, "scope_median");
  EXPECT_EQ(d.regressions[0].subject, "scope:Render");  // larger relative change first
  EXPECT_EQ(d.regressions[1].subject, "scope:Update");

  cur.scopes = {scope("Update", 4.5), scope("Render", 0.53)};  // +12.5 % and +6 % (+0.03 ms)
  EXPECT_FALSE(compareReports(base, cur, {}).regressed());

  cur.scopes = {scope("Update", 4.0), scope("Render", 0.52), scope("Brand New", 99)};
  EXPECT_FALSE(compareReports(base, cur, {}).regressed());  // new scopes have no baseline
}

TEST(BaselineDiff, LostEvents) {
  Report base = baseReport();
  Report cur = base;
  cur.trace.eventsLost = 12;
  Diff d = compareReports(base, cur, {});
  const Finding* f = find(d.regressions, "lost_events");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->baseline, 0.0);
  EXPECT_EQ(f->current, 12.0);
  EXPECT_EQ(*f->change, 12.0);

  Thresholds t;
  t.lostEventsAllowed = 12;
  EXPECT_FALSE(compareReports(base, cur, t).regressed());
  t.lostEventsAllowed = 11;
  EXPECT_TRUE(compareReports(base, cur, t).regressed());

  base.trace.eventsLost = 20;
  const Diff better = compareReports(base, cur, {});
  EXPECT_FALSE(better.regressed());
  ASSERT_NE(find(better.improvements, "lost_events"), nullptr);
}

TEST(BaselineDiff, CustomThresholds) {
  const Report base = baseReport();
  Report cur = base;
  cur.frames[0].p95Ms = 17.0;  // +6.25 %
  Thresholds t;
  t.frameP95RelPct = 5.0;
  t.frameAbsFloorMs = 0.1;
  EXPECT_TRUE(compareReports(base, cur, t).regressed());
  EXPECT_EQ(compareReports(base, cur, t).thresholds.frameP95RelPct, 5.0);
}

TEST(BaselineDiff, UnmatchedSeriesAndPlatformAreNotes) {
  Report base = baseReport();
  Report cur = base;
  cur.frames.push_back(series("Other@Render", 5, 6));
  cur.trace.platform = "windows";
  const Diff d = compareReports(base, cur, {});
  EXPECT_FALSE(d.regressed());
  ASSERT_EQ(d.notes.size(), 2u);
}

TEST(BaselineDiff, FindingOrderIsDeterministic) {
  const Report base = baseReport();
  Report cur = base;
  cur.frames[0].p95Ms = 20.0;
  cur.frames[0].p99Ms = 40.0;
  cur.functions = {fn("A", 45), fn("B", 35), fn("C", 20)};
  cur.scopes = {scope("Update", 8.0), scope("Render", 0.5)};
  cur.trace.eventsLost = 3;
  const Diff d = compareReports(base, cur, {});
  ASSERT_EQ(d.regressions.size(), 6u);
  const char* order[] = {"frame_p95",          "frame_p99",    "function_exclusive",
                         "function_exclusive", "scope_median", "lost_events"};
  for (size_t i = 0; i < 6; ++i) EXPECT_EQ(d.regressions[i].category, order[i]) << i;
  EXPECT_EQ(d.regressions[2].subject, "game.exe!A");  // +5 pp before +5 pp: ties by subject
  EXPECT_EQ(d.regressions[3].subject, "game.exe!B");
}

TEST(Thresholds, ParseDefaultsAndOverrides) {
  auto t = thresholdsFromJson(nlohmann::json::parse("{}"));
  ASSERT_TRUE(t.ok());
  EXPECT_EQ(t->frameP95RelPct, 10.0);
  EXPECT_EQ(t->frameP99RelPct, 15.0);
  EXPECT_EQ(t->frameAbsFloorMs, 0.5);
  EXPECT_EQ(t->functionExclusivePp, 2.0);
  EXPECT_EQ(t->scopeMedianRelPct, 25.0);
  EXPECT_EQ(t->scopeAbsFloorMs, 0.05);
  EXPECT_EQ(t->lostEventsAllowed, 0u);

  auto o = thresholdsFromJson(nlohmann::json::parse(
      R"({"frame_p95_rel_pct": 3, "function_exclusive_pp": 0.5, "lost_events_allowed": 4})"));
  ASSERT_TRUE(o.ok());
  EXPECT_EQ(o->frameP95RelPct, 3.0);
  EXPECT_EQ(o->functionExclusivePp, 0.5);
  EXPECT_EQ(o->lostEventsAllowed, 4u);
  EXPECT_EQ(o->frameP99RelPct, 15.0);
}

TEST(Thresholds, StrictParsing) {
  EXPECT_FALSE(thresholdsFromJson(nlohmann::json::parse("[]")).ok());
  EXPECT_FALSE(thresholdsFromJson(nlohmann::json::parse(R"({"typo_key": 1})")).ok());
  EXPECT_FALSE(thresholdsFromJson(nlohmann::json::parse(R"({"frame_p95_rel_pct": "10"})")).ok());
  EXPECT_FALSE(thresholdsFromJson(nlohmann::json::parse(R"({"frame_p95_rel_pct": -1})")).ok());
  EXPECT_FALSE(thresholdsFromJson(nlohmann::json::parse(R"({"lost_events_allowed": 1.5})")).ok());
  auto bad = thresholdsFromJson(nlohmann::json::parse(R"({"nope": 1})"));
  EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
}

TEST(Thresholds, RoundTripThroughJson) {
  Thresholds t;
  t.frameP95RelPct = 7;
  t.lostEventsAllowed = 9;
  auto back = thresholdsFromJson(nlohmann::json::parse(thresholdsToJson(t).dump()));
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->frameP95RelPct, 7.0);
  EXPECT_EQ(back->lostEventsAllowed, 9u);
}

}  // namespace
}  // namespace tachyon::report
