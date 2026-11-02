// WP-48: the status bar. Pure formatting helpers, then a headless App on `medium` (4 modules) whose symbol
// provider factory is slow (SymbolService::setProviderFactory): the "Symbols: resolved/total" counts only
// ever grow, an intermediate state is observed, the hover table lists every module, and the bar shows the
// capture duration, sample count and the active filter.
#include "viewer/status/status_bar.h"

#include "status_test_util.h"

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

TEST(StatusBarFormat, Durations) {
  EXPECT_EQ(formatDuration(850'000'000, 1'000'000'000), "850 ms");
  EXPECT_EQ(formatDuration(1'250'000'000, 1'000'000'000), "1.25 s");
  EXPECT_EQ(formatDuration(125'000'000'000ull, 1'000'000'000), "2 min 5 s");
  EXPECT_EQ(formatDuration(5, 0), "");
}

TEST(StatusBarFormat, FilterSummary) {
  model::TraceModel m;
  m.header.clock_frequency_hz = 1'000'000'000;
  m.header.start_ts = 1'000'000'000;
  SelectionState s;
  EXPECT_EQ(describeFilter(s, m), "none");
  ViewFilter f;
  f.t0 = 1'250'000'000;
  f.t1 = 2'500'000'000;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(0), ThreadIdx(1)};
  s.timeFilter = f;
  EXPECT_EQ(describeFilter(s, m), "time 0.250-1.500 s, 2 threads");
  model::Thread t;
  t.tid = 77;
  t.name = "render";
  m.threads.push_back(t);
  s.selectedThread = ThreadIdx(0);
  EXPECT_EQ(describeFilter(s, m), "time 0.250-1.500 s, 2 threads, thread render");
}

TEST(StatusBar, SymbolCountsGrowMonotonicallyWhileResolving) {
  AppOptions options = headlessOptions("statusbar");
  options.configureSymbols = slowProviders(60);
  Harness h(std::move(options));
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr; }));

  uint32_t lastResolved = 0, lastDone = 0;
  bool sawRunning = false, sawFinished = false;
  std::string runningText;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    h.frames(1);
    const SymbolSummary& s = h.app.statusBar().summary();
    EXPECT_GE(s.resolved, lastResolved);
    EXPECT_GE(s.done, lastDone);
    EXPECT_LE(s.resolved + s.failed, s.total);
    lastResolved = s.resolved;
    lastDone = s.done;
    if (s.total > 0 && s.running()) {
      sawRunning = true;
      if (runningText.empty()) runningText = h.app.statusBar().symbolsText();
    }
    if (s.total > 0 && !s.running()) {
      sawFinished = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(sawRunning) << "never observed the resolution in flight";
  ASSERT_TRUE(sawFinished);
  const SymbolSummary& s = h.app.statusBar().summary();
  EXPECT_EQ(s.total, 4u);
  EXPECT_EQ(s.resolved, 4u);
  EXPECT_EQ(s.failed, 0u);
  EXPECT_EQ(runningText.rfind("Symbols: ", 0), 0u);
  EXPECT_EQ(h.app.statusBar().symbolsText(), "Symbols: 4/4");
}

TEST(StatusBar, HoverListsEveryModuleWithItsState) {
  TempDir dir("statusbar_hover");
  // medium without its sidecar: nothing resolves, every module is NotFound with a reason.
  std::error_code ec;
  fs::copy_file(fixture("medium.tachyon"), dir.path() / "medium.tachyon", ec);
  ASSERT_FALSE(ec);

  Harness h;
  ASSERT_TRUE(h.open(dir.path() / "medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  h.frames(3);
  const SymbolSummary& s = h.app.statusBar().summary();
  EXPECT_EQ(s.total, 4u);
  EXPECT_EQ(s.resolved, 0u);
  EXPECT_EQ(s.failed, 4u);
  EXPECT_EQ(h.app.statusBar().symbolsText(), "Symbols: 0/4 (4 failed)");

  const StatusBar::Rect r = h.app.statusBar().symbolsRect();
  ASSERT_GT(r.x1, r.x0);
  ImGui::GetIO().AddMousePosEvent((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f);
  h.frames(3);
  EXPECT_GT(h.app.statusBar().tooltipDraws(), 0u);
  const auto& rows = h.app.statusBar().tooltipRows();
  ASSERT_EQ(rows.size(), 4u);
  std::vector<std::string> names;
  for (const ModuleSymbolRow& row : rows) {
    names.push_back(row.name);
    EXPECT_EQ(row.state, symbols::SymbolState::NotFound);
    EXPECT_FALSE(row.detail.empty()) << row.name;
  }
  EXPECT_EQ(names, (std::vector<std::string>{"engine.exe", "render.dll", "physics.dll", "ntdll.dll"}));
}

TEST(StatusBar, ShowsDurationSamplesAndFilter) {
  Harness h;
  ASSERT_TRUE(h.open(fixture("tiny.tachyon")));
  h.frames(2);
  const std::string text = describeDocument(*h.app.document());
  EXPECT_EQ(text.rfind("tiny.tachyon", 0), 0u);
  EXPECT_NE(text.find(std::to_string(h.app.document()->model().threads.size()) + " threads"),
            std::string::npos);
  EXPECT_NE(text.find("samples"), std::string::npos);
  const model::TraceModel& m = h.app.document()->model();
  EXPECT_NE(text.find(formatDuration(m.header.end_ts - m.header.start_ts, m.header.clock_frequency_hz)),
            std::string::npos);
  ViewFilter f;
  f.t0 = m.header.start_ts;
  f.t1 = m.header.start_ts + m.header.clock_frequency_hz / 100;
  h.app.selection().setTimeFilter(f);
  h.frames(2);
  EXPECT_NE(describeFilter(h.app.selection().state(), m), "none");
}

TEST(StatusBar, ClickOpensSymbolsPanel) {
  TempDir dir("statusbar_click");
  std::error_code ec;
  fs::copy_file(fixture("tiny.tachyon"), dir.path() / "tiny.tachyon", ec);
  Harness h;
  ASSERT_TRUE(h.open(dir.path() / "tiny.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  h.frames(3);
  PanelEntry* symbols = h.app.panels().find("Symbols");
  ASSERT_NE(symbols, nullptr);
  EXPECT_FALSE(symbols->open);
  const StatusBar::Rect r = h.app.statusBar().symbolsRect();
  h.click((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f);
  EXPECT_TRUE(symbols->open);
}
