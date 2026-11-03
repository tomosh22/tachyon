// WP-67 item 1: warnings that arrive only in `capture_done` (spool caps, refused connections, failed merges)
// also show in the banner of the trace they belong to, and the lost-events / throttle sentences are shown
// once even when the perf converter also writes them as free text in `convert_stats.warnings`.
#include "viewer/status/capture_warnings.h"
#include "viewer/status/warnings_banner.h"

#include "status_test_util.h"

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

namespace {

model::TraceModel modelWithInfo(const std::string& json) {
  model::TraceModel m;
  m.captureInfo.raw_json = json;
  return m;
}

bool has(const std::vector<CaptureWarning>& w, CaptureWarningKind kind, const std::string& needle = "") {
  for (const CaptureWarning& e : w) {
    if (e.kind == kind && e.text.find(needle) != std::string::npos) return true;
  }
  return false;
}

size_t count(const std::vector<CaptureWarning>& w, CaptureWarningKind kind) {
  size_t n = 0;
  for (const CaptureWarning& e : w) n += e.kind == kind;
  return n;
}

// What the perf converter writes: the structured fields and, as free text, the same facts in its own words.
constexpr const char* kPerfInfo = R"json({
  "events_lost": 120,
  "throttle": {"events": 3, "throttled_ms": 40},
  "convert_stats": {"warnings": [
    "120 samples/records were lost because the ring buffers overflowed (PERF_RECORD_LOST): the drain thread could not keep up. Lower sampling_hz, or enlarge the rings (tachyon-agent --perf-ring-pages)",
    "sampling was throttled 3 time(s) for 40 ms in total: lower the sampling rate",
    "2 modules have no build-id (file unreadable or without .note.gnu.build-id)"]}})json";

}  // namespace

TEST(PolishWarnings, LostAndThrottleLinesAreShownOnceEvenWhenTheConverterAlsoWritesThemAsText) {
  const auto w = collectCaptureWarnings(modelWithInfo(kPerfInfo));
  EXPECT_EQ(count(w, CaptureWarningKind::EventsLost), 1u);
  EXPECT_EQ(count(w, CaptureWarningKind::Throttled), 1u);
  EXPECT_EQ(count(w, CaptureWarningKind::ConverterWarning), 1u) << "only the build-id sentence is news";
  EXPECT_TRUE(has(w, CaptureWarningKind::ConverterWarning, "no build-id"));
  EXPECT_EQ(w.size(), 3u);
}

TEST(PolishWarnings, TextWithoutAStructuredTwinIsKept) {
  // No events_lost / throttle fields: the free text is the only statement of the fact.
  const auto w = collectCaptureWarnings(modelWithInfo(
      R"json({"convert_stats": {"warnings": ["sampling was throttled 3 time(s)", "ring buffers overflowed"]}})json"));
  EXPECT_EQ(count(w, CaptureWarningKind::ConverterWarning), 2u);
  // Exact repeats collapse.
  const auto d = collectCaptureWarnings(
      modelWithInfo(R"({"convert_stats": {"warnings": ["same", "same"]}, "warnings": ["same", "other"]})"));
  EXPECT_EQ(d.size(), 2u);
}

TEST(PolishWarnings, AgentWarningsAreAddedAndDeduplicatedAgainstTheTrace) {
  const std::vector<std::string> agent = {
      "instrumentation spool of pid 7 hit its size cap",
      "120 samples/records were lost because the ring buffers overflowed (PERF_RECORD_LOST)",
      "sampling was throttled 3 time(s) for 40 ms in total: lower the sampling rate",
      "2 modules have no build-id (file unreadable or without .note.gnu.build-id)",  // exact repeat
      "instrumentation spool of pid 7 hit its size cap"};
  const auto w = collectCaptureWarnings(modelWithInfo(kPerfInfo), agent);
  EXPECT_EQ(count(w, CaptureWarningKind::AgentWarning), 1u);
  EXPECT_TRUE(has(w, CaptureWarningKind::AgentWarning, "hit its size cap"));
  EXPECT_EQ(count(w, CaptureWarningKind::EventsLost), 1u);
  EXPECT_EQ(count(w, CaptureWarningKind::Throttled), 1u);
  EXPECT_EQ(count(w, CaptureWarningKind::ConverterWarning), 1u);
  // On a trace with no warnings of its own, they all show (the repeated sentence once).
  const auto only = collectCaptureWarnings(modelWithInfo(R"({"sampling_hz":8000})"), agent);
  EXPECT_EQ(count(only, CaptureWarningKind::AgentWarning), 4u);
}

TEST(PolishBanner, AgentWarningsOfACaptureShowInTheBannerOfItsTrace) {
  TempDir dir("polish_banner_agent");
  const fs::path clean = dir.path() / "clean.tachyon";
  writeTrace(clean, R"({"sampling_hz":8000})");
  const fs::path other = dir.path() / "other.tachyon";
  writeTrace(other, R"({"sampling_hz":8000})");
  Harness h;
  // Announced before the trace is open (the usual order: capture_done, then the open).
  h.app.warningsBanner().addAgentWarnings(
      clean, {"instrumentation spool of pid 7 hit its size cap", "refused a connection from pid 9"});
  ASSERT_TRUE(h.open(other));
  h.frames(3);
  EXPECT_FALSE(h.app.warningsBanner().visible()) << "another trace's warnings do not leak";
  ASSERT_TRUE(h.open(clean));
  h.frames(3);
  WarningsBanner& banner = h.app.warningsBanner();
  ASSERT_TRUE(banner.visible());
  EXPECT_EQ(count(banner.warnings(), CaptureWarningKind::AgentWarning), 2u);
  EXPECT_TRUE(has(banner.warnings(), CaptureWarningKind::AgentWarning, "hit its size cap"));
  EXPECT_NE(ImGui::FindWindowByName("##CaptureWarnings"), nullptr);

  // Arriving while the trace is open (a remote transfer that finished later) also shows, and re-shows a
  // dismissed banner.
  banner.dismiss();
  h.frames(2);
  EXPECT_FALSE(banner.visible());
  banner.addAgentWarnings(clean, {"merge of pid 4 failed"});
  h.frames(3);
  EXPECT_TRUE(banner.visible());
  EXPECT_TRUE(has(banner.warnings(), CaptureWarningKind::AgentWarning, "merge of pid 4 failed"));
  EXPECT_FALSE(has(banner.warnings(), CaptureWarningKind::AgentWarning, "hit its size cap"))
      << "the newest capture_done replaces the earlier text for that trace";
}

TEST(PolishBanner, ANoticeSurvivesAnAgentWarningArrivingLater) {
  TempDir dir("polish_banner_notice");
  const fs::path p = dir.path() / "p.tachyon";
  writeTrace(p, R"({"sampling_hz":8000})");
  Harness h;
  ASSERT_TRUE(h.open(p));
  h.frames(2);
  WarningsBanner& banner = h.app.warningsBanner();
  banner.addNotice(CaptureWarningKind::MemoryDegraded, "memory ceiling reached");
  banner.addAgentWarnings(p, {"spool cap"});
  h.frames(3);
  EXPECT_TRUE(has(banner.warnings(), CaptureWarningKind::MemoryDegraded, "ceiling"));
  EXPECT_TRUE(has(banner.warnings(), CaptureWarningKind::AgentWarning, "spool cap"));
}

TEST(PolishBanner, ALossyTraceFromThePerfConverterShowsEachLineOnceInTheBanner) {
  TempDir dir("polish_banner_perf");
  const fs::path p = dir.path() / "perf.tachyon";
  writeTrace(p, kPerfInfo);
  Harness h;
  ASSERT_TRUE(h.open(p));
  h.frames(3);
  const auto& w = h.app.warningsBanner().warnings();
  EXPECT_EQ(count(w, CaptureWarningKind::EventsLost), 1u);
  EXPECT_EQ(count(w, CaptureWarningKind::Throttled), 1u);
  EXPECT_EQ(w.size(), 3u);
}

// The agent re-emits the trace's merge warnings as "instrumentation: <w>"; the trace's copy reads
// "Instrumentation: <w>". Shown once.
TEST(PolishWarnings, AgentCopiesOfMergeWarningsAreShownOnce) {
  const auto w = collectCaptureWarnings(
      modelWithInfo(
          R"json({"instrumentation": {"merges": [{"warnings": ["pid 7: 3 unmatched scope ends"]}]}})json"),
      {"instrumentation: pid 7: 3 unmatched scope ends"});
  EXPECT_EQ(count(w, CaptureWarningKind::InstrumentationWarning), 1u);
  EXPECT_EQ(count(w, CaptureWarningKind::AgentWarning), 0u);
}
