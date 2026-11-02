// WP-62: Markdown rendering. Golden files live in tests/unit/report/golden; set TACHYON_UPDATE_GOLDEN=1 to
// rewrite them after an intended change.
#include "report/markdown.h"

#include "report/baseline.h"
#include "report_test_util.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace tachyon::report {
namespace {

Report goldenReport() {
  Report r;
  r.trace.durationMs = 12345.678;
  r.trace.platform = "linux";
  r.trace.clockFrequencyHz = 1'000'000'000;
  r.trace.samplingHz = 8000;
  r.trace.eventsLost = 12;
  r.trace.threadCount = 3;
  r.trace.sampleCount = 98765;
  r.trace.throttleEvents = 2;
  r.trace.warnings = {"spool cap hit | twice"};
  r.settings.topN = 20;
  r.settings.frameBudgetMs = 16.67;
  FrameSeriesRow f;
  f.name = "Frame";
  f.label = "Frame@Main";
  f.threadTid = 501;
  f.threadName = "Main";
  f.isDefault = true;
  f.markCount = 741;
  f.frameCount = 740;
  f.p50Ms = 8.25;
  f.p95Ms = 14.5;
  f.p99Ms = 21.125;
  f.maxMs = 50.666;
  f.meanMs = 9.01;
  f.overBudget = 4;
  f.slowestFrameNumber = 59;
  r.frames.push_back(f);
  f.name = "RenderFrame";
  f.label = "RenderFrame@Render";
  f.isDefault = false;
  f.p95Ms = 12.0;
  f.p99Ms = 13.0;
  r.frames.push_back(f);
  r.frameSeriesTotal = 2;
  r.threads = {{501, "Main", 9000.5, 10.25, 3000.125, 0, 1000}, {502, "", 4000, 0, 8000, 0, 500}};
  r.totalExclusiveMs = 10000;
  r.functionCount = 300;
  for (int i = 0; i < 12; ++i) {
    FunctionReportRow fn;
    fn.name = "Engine::fn" + std::to_string(i);
    fn.module = i % 2 ? "engine.exe" : "render.dll";
    fn.exclusiveMs = 1000.0 - i * 50;
    fn.inclusiveMs = 2000.0 - i * 50;
    fn.exclusivePct = fn.exclusiveMs / 100.0;
    fn.samples = 100;
    r.functions.push_back(fn);
  }
  r.functions[3].name = "operator|(int)";
  r.functions[4].isScope = true;
  r.functions[4].name = "Update";
  r.functions[4].module = "";
  ScopeReportRow s;
  s.name = "Update";
  s.count = 740;
  s.minMs = 1;
  s.medianMs = 2.5;
  s.p95Ms = 3.75;
  s.maxMs = 9;
  s.totalMs = 1900.5;
  s.meanMs = 2.568;
  r.scopes.push_back(s);
  r.scopeCount = 1;
  LockReportRow l;
  l.name = "Mutex.Scene";
  l.address = "0x7ffd1234";
  l.contentions = 17;
  l.totalWaitMs = 4.5;
  l.maxWaitMs = 1.25;
  r.locks.push_back(l);
  l.name = "";
  l.address = "0x7ffd5678";
  r.locks.push_back(l);
  r.lockCount = 2;
  return r;
}

Diff goldenDiff() {
  Diff d;
  d.frames.push_back({"Frame@Main", 12.0, 21.0});
  Finding a;
  a.category = "frame_p95";
  a.subject = "Frame@Main";
  a.baseline = 12.0;
  a.current = 14.5;
  a.valueUnit = "ms";
  a.change = 20.833;
  a.changeUnit = "%";
  d.regressions.push_back(a);
  Finding b;
  b.category = "function_exclusive";
  b.subject = "engine.exe!Engine::fn1";
  b.baseline = 1.5;
  b.current = 9.5;
  b.valueUnit = "% of total";
  b.change = 8.0;
  b.changeUnit = "pp";
  b.baselineIsBound = true;
  d.regressions.push_back(b);
  Finding c;
  c.category = "lost_events";
  c.baseline = 0;
  c.current = 12;
  c.valueUnit = "events";
  c.change = 12;
  c.changeUnit = "events";
  d.regressions.push_back(c);
  Finding g;
  g.kind = FindingKind::Improvement;
  g.category = "scope_median";
  g.subject = "scope:Update";
  g.baseline = 4.0;
  g.current = 2.5;
  g.valueUnit = "ms";
  g.change = -37.5;
  g.changeUnit = "%";
  d.improvements.push_back(g);
  d.notes.push_back("frame series 'RenderFrame@Render' is not in the baseline");
  return d;
}

void expectGolden(const std::string& actual, const std::string& file) {
  const std::filesystem::path path = std::filesystem::path(TACHYON_REPORT_GOLDEN_DIR) / file;
  if (const char* up = std::getenv("TACHYON_UPDATE_GOLDEN"); up != nullptr && std::string(up) == "1") {
    testutil::writeText(path, actual);
  }
  EXPECT_EQ(actual, testutil::readText(path)) << "golden file " << file;
}

TEST(Markdown, GoldenWithBaselineRegressions) {
  const Diff d = goldenDiff();
  expectGolden(renderMarkdown(goldenReport(), &d), "with_regressions.md");
}

TEST(Markdown, GoldenPass) {
  Diff d;
  d.frames.push_back({"Frame@Main", 14.0, 21.0});
  Finding g;
  g.kind = FindingKind::Improvement;
  g.category = "frame_p95";
  g.subject = "Frame@Main";
  g.baseline = 20.0;
  g.current = 14.5;
  g.valueUnit = "ms";
  g.change = -27.5;
  g.changeUnit = "%";
  d.improvements.push_back(g);
  Report r = goldenReport();
  r.trace.eventsLost = 0;
  r.trace.throttleEvents = 0;
  r.trace.warnings.clear();
  expectGolden(renderMarkdown(r, &d), "pass.md");
}

TEST(Markdown, GoldenPlainReportWithoutFramesOrBaseline) {
  Report r = goldenReport();
  r.frames.clear();
  r.frameSeriesTotal = 0;
  r.scopes.clear();
  r.locks.clear();
  r.trace.eventsLost = 0;
  r.trace.throttleEvents = 0;
  r.trace.warnings.clear();
  r.trace.partial = true;
  expectGolden(renderMarkdown(r, nullptr), "plain.md");
}

TEST(Markdown, IsDeterministicAndFreeOfMachineSpecificData) {
  const Diff d = goldenDiff();
  const std::string a = renderMarkdown(goldenReport(), &d);
  EXPECT_EQ(a, renderMarkdown(goldenReport(), &d));
  EXPECT_EQ(a.find("0x7ffd"), std::string::npos) << "lock addresses must not appear";
  EXPECT_EQ(a.find("/home"), std::string::npos);
  EXPECT_EQ(a.find('\r'), std::string::npos);
  EXPECT_EQ(a.back(), '\n');
}

TEST(Markdown, PipeCharactersAreEscapedInCells) {
  const std::string md = renderMarkdown(goldenReport(), nullptr);
  EXPECT_NE(md.find("`operator\\|(int)`"), std::string::npos);
}

}  // namespace
}  // namespace tachyon::report
