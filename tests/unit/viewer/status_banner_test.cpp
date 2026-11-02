// WP-48: the capture warnings banner. collectCaptureWarnings on hand-made CaptureInfo documents, and the
// banner in a headless App: it appears for a trace with events_lost / a dropped-marker / a partial flag, a
// dismissal sticks (also when the same file is opened again), a clean trace shows nothing.
#include "viewer/status/capture_warnings.h"
#include "viewer/status/status_bar.h"
#include "viewer/status/warnings_banner.h"

#include "status_test_util.h"

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

namespace {

model::TraceModel modelWithInfo(const std::string& json, bool partial = false) {
  model::TraceModel m;
  m.captureInfo.raw_json = json;
  m.isPartial = partial;
  return m;
}

bool has(const std::vector<CaptureWarning>& w, CaptureWarningKind kind, const std::string& needle = "") {
  for (const CaptureWarning& e : w) {
    if (e.kind == kind && e.text.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST(StatusWarnings, CleanTraceHasNone) {
  EXPECT_TRUE(collectCaptureWarnings(modelWithInfo(R"({"sampling_hz":8000,"events_lost":0})")).empty());
  EXPECT_TRUE(collectCaptureWarnings(modelWithInfo("")).empty());
  EXPECT_TRUE(collectCaptureWarnings(modelWithInfo("not json {")).empty());
  EXPECT_TRUE(collectCaptureWarnings(modelWithInfo("[1,2,3]")).empty());
}

TEST(StatusWarnings, EventsLostThrottleAndConverterWarnings) {
  const auto w = collectCaptureWarnings(modelWithInfo(R"({
    "events_lost": 12,
    "host": {"etl_buffers_lost": 3},
    "throttle": {"events": 2, "throttled_ms": 40, "estimated_missed_samples": 320},
    "convert_stats": {"warnings": ["module x has no build id", 7, "stack mismatch"]},
    "warnings": ["spool cap reached"]})"));
  EXPECT_TRUE(has(w, CaptureWarningKind::EventsLost, "12 events"));
  EXPECT_TRUE(has(w, CaptureWarningKind::EventsLost, "3 ETW buffers"));
  EXPECT_TRUE(has(w, CaptureWarningKind::Throttled, "2 time(s)"));
  EXPECT_TRUE(has(w, CaptureWarningKind::Throttled, "320 samples"));
  EXPECT_TRUE(has(w, CaptureWarningKind::ConverterWarning, "module x has no build id"));
  EXPECT_TRUE(has(w, CaptureWarningKind::ConverterWarning, "stack mismatch"));
  EXPECT_TRUE(has(w, CaptureWarningKind::ConverterWarning, "spool cap reached"));
  EXPECT_EQ(w.size(), 6u);
}

TEST(StatusWarnings, DroppedInstrumentationAndMergeWarnings) {
  const auto w = collectCaptureWarnings(modelWithInfo(R"({"instrumentation": {"merge_count": 2, "merges": [
    {"dropped_records": 5, "warnings": ["clock mapper fallback"]},
    {"dropped_records": 4, "warnings": []}]}})"));
  EXPECT_TRUE(has(w, CaptureWarningKind::DroppedInstrumentation, "9 records"));
  EXPECT_TRUE(has(w, CaptureWarningKind::InstrumentationWarning, "clock mapper fallback"));
  EXPECT_EQ(w.size(), 2u);
}

TEST(StatusWarnings, PartialFlagAndCap) {
  EXPECT_TRUE(has(collectCaptureWarnings(modelWithInfo("", true)), CaptureWarningKind::Partial, "partial"));
  std::string many = R"({"warnings": [)";
  for (int i = 0; i < 50; ++i) many += std::string(i ? "," : "") + "\"w" + std::to_string(i) + "\"";
  many += "]}";
  const auto w = collectCaptureWarnings(modelWithInfo(many));
  EXPECT_EQ(w.size(), kMaxCaptureWarnings + 1);
  EXPECT_NE(w.back().text.find("18 more"), std::string::npos);
}

TEST(StatusBanner, AppearsForEventsLostAndStaysDismissed) {
  TempDir dir("banner_lost");
  const fs::path lossy = dir.path() / "lossy.tachyon";
  writeTrace(lossy, R"({"sampling_hz":8000,"events_lost":42})");
  const fs::path clean = dir.path() / "clean.tachyon";
  writeTrace(clean, R"({"sampling_hz":8000,"events_lost":0})");

  Harness h;
  ASSERT_TRUE(h.open(clean));
  h.frames(2);
  EXPECT_FALSE(h.app.warningsBanner().visible());
  EXPECT_TRUE(h.app.warningsBanner().warnings().empty());
  EXPECT_FLOAT_EQ(h.app.warningsBanner().lastHeight(), 0.0f);

  ASSERT_TRUE(h.open(lossy));
  h.frames(3);
  WarningsBanner& banner = h.app.warningsBanner();
  ASSERT_TRUE(banner.visible());
  ASSERT_EQ(banner.warnings().size(), 1u);
  EXPECT_EQ(banner.warnings()[0].kind, CaptureWarningKind::EventsLost);
  EXPECT_NE(banner.warnings()[0].text.find("42 events"), std::string::npos);
  EXPECT_GT(banner.lastHeight(), 0.0f);
  EXPECT_NE(ImGui::FindWindowByName("##CaptureWarnings"), nullptr);

  // Dismiss; it stays dismissed over many frames.
  banner.dismiss();
  h.frames(5);
  EXPECT_FALSE(banner.visible());
  EXPECT_TRUE(banner.dismissed());
  EXPECT_FLOAT_EQ(banner.lastHeight(), 0.0f);

  // Another document: its own state. Coming back to the dismissed one: still dismissed.
  ASSERT_TRUE(h.open(clean));
  h.frames(2);
  EXPECT_FALSE(banner.visible());
  ASSERT_TRUE(h.open(lossy));
  h.frames(3);
  EXPECT_FALSE(banner.warnings().empty());
  EXPECT_FALSE(banner.visible());
  EXPECT_TRUE(banner.dismissed());
}

TEST(StatusBanner, DismissButtonClick) {
  TempDir dir("banner_click");
  const fs::path lossy = dir.path() / "lossy.tachyon";
  writeTrace(lossy, R"({"events_lost":3})");
  Harness h;
  ASSERT_TRUE(h.open(lossy));
  h.frames(3);
  ASSERT_TRUE(h.app.warningsBanner().visible());
  const ImGuiWindow* w = ImGui::FindWindowByName("##CaptureWarnings");
  ASSERT_NE(w, nullptr);
  // The Dismiss button sits at the right edge of the banner's first line.
  h.click(w->Pos.x + w->Size.x - ImGui::GetFontSize() * 3.0f, w->Pos.y + ImGui::GetFontSize() * 1.2f);
  h.frames(2);
  EXPECT_FALSE(h.app.warningsBanner().visible());
}

TEST(StatusBanner, DroppedInstrumentationMarkers) {
  TempDir dir("banner_dropped");
  const fs::path p = dir.path() / "dropped.tachyon";
  writeTrace(p, R"({"sampling_hz":8000})", /*finish=*/true, /*droppedMarkers=*/3);
  Harness h;
  ASSERT_TRUE(h.open(p));
  h.frames(2);
  ASSERT_TRUE(h.app.warningsBanner().visible());
  EXPECT_TRUE(has(h.app.warningsBanner().warnings(), CaptureWarningKind::DroppedInstrumentation,
                  "3 tachyon.dropped"));
}

TEST(StatusBanner, PartialTrace) {
  Harness h;
  ASSERT_TRUE(h.open(fixture("partial.tachyon")));
  h.frames(2);
  ASSERT_TRUE(h.app.document()->model().isPartial);
  EXPECT_TRUE(h.app.warningsBanner().visible());
  EXPECT_TRUE(has(h.app.warningsBanner().warnings(), CaptureWarningKind::Partial));
}

TEST(StatusBanner, UnfinishedWriterGivesPartialBanner) {
  TempDir dir("banner_unfinished");
  const fs::path p = dir.path() / "unfinished.tachyon";
  writeTrace(p, "", /*finish=*/false);
  Harness h;
  ASSERT_TRUE(h.open(p));
  h.frames(2);
  EXPECT_TRUE(has(h.app.warningsBanner().warnings(), CaptureWarningKind::Partial));
}

TEST(StatusBanner, FinishedFixtureHasNoBanner) {
  Harness h;
  ASSERT_TRUE(h.open(fixture("tiny.tachyon")));
  h.frames(3);
  EXPECT_FALSE(h.app.warningsBanner().visible());
}
