// WP-67 item 2: the `*.symbols.json` scan of a symbol directory runs off the UI thread, is bounded, and an
// unreadable directory is reported instead of accepted silently.
#include "viewer/paths.h"
#include "viewer/status/symbol_paths.h"
#include "viewer/status/symbols_panel.h"

#include "status_test_util.h"

#include <algorithm>
#include <fstream>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

namespace {

// <root>/trace/tiny.tachyon without a sidecar and <root>/syms with `count` copies of tiny.symbols.json (every
// copy describes the same modules), named so that the real match sorts last.
struct BigLayout {
  BigLayout(const std::string& name, size_t count) : root(name) {
    std::error_code ec;
    fs::create_directories(root.path() / "trace", ec);
    fs::create_directories(symsDir(), ec);
    fs::copy_file(fixture("tiny.tachyon"), trace(), ec);
    EXPECT_FALSE(ec);
    for (size_t i = 0; i < count; ++i) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "m%05zu.symbols.json", i);
      fs::copy_file(fixture("tiny.symbols.json"), symsDir() / buf, ec);
      EXPECT_FALSE(ec);
    }
  }
  fs::path trace() const { return root.path() / "trace" / "tiny.tachyon"; }
  fs::path symsDir() const { return root.path() / "syms"; }
  TempDir root;
};

bool anyContains(const std::vector<std::string>& lines, const std::string& needle) {
  return std::any_of(lines.begin(), lines.end(),
                     [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

bool toastContains(App& app, const std::string& text) {
  for (const Toast& t : app.toasts().toasts())
    if (t.text.find(text) != std::string::npos) return true;
  return false;
}

bool isRoot() {
#if defined(_WIN32)
  return false;
#else
  return ::geteuid() == 0;
#endif
}

}  // namespace

TEST(PolishSymbolScan, TheDirectoryScanIsCappedAndSaysSo) {
  EXPECT_EQ(kMaxSymbolDirectoryEntries, 10000u);
  BigLayout layout("polish_scan_cap", 300);
  symbols::SymbolService service({}, layout.root.path() / "cache");
  SymbolPathOptions options;
  options.maxDirectoryEntries = 100;
  const SymbolPathReport report = applySymbolSearchPaths(service, {pathToUtf8(layout.symsDir())}, {},
                                                         layout.root.path() / "cache", options);
  EXPECT_EQ(report.scanStops, 1u);
  EXPECT_LE(report.overrides, 100u);
  EXPECT_GT(report.overrides, 0u);
  ASSERT_EQ(report.problems.size(), 1u);
  EXPECT_NE(report.problems[0].find("scan stopped at 100 files"), std::string::npos) << report.problems[0];
  EXPECT_EQ(report.searchPaths, 1u) << "the locator still probes the whole directory by name";
  EXPECT_FALSE(report.cancelled);

  // Under the cap nothing is reported.
  const SymbolPathReport all =
      applySymbolSearchPaths(service, {pathToUtf8(layout.symsDir())}, {}, layout.root.path() / "cache");
  EXPECT_EQ(all.scanStops, 0u);
  EXPECT_EQ(all.overrides, 300u);
  EXPECT_TRUE(all.problems.empty());
}

TEST(PolishSymbolScan, ACancelFlagStopsTheScan) {
  BigLayout layout("polish_scan_cancel", 50);
  symbols::SymbolService service({}, layout.root.path() / "cache");
  std::atomic<bool> cancel{true};
  SymbolPathOptions options;
  options.cancel = &cancel;
  const SymbolPathReport report = applySymbolSearchPaths(service, {pathToUtf8(layout.symsDir())}, {},
                                                         layout.root.path() / "cache", options);
  EXPECT_TRUE(report.cancelled);
  EXPECT_EQ(report.overrides, 0u);
}

TEST(PolishSymbolScan, ManyBrokenFilesGiveABoundedProblemList) {
  BigLayout layout("polish_scan_problems", 0);
  for (int i = 0; i < 200; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "bad%03d.symbols.json", i);
    std::ofstream(layout.symsDir() / buf) << "{ not json";
  }
  symbols::SymbolService service({}, layout.root.path() / "cache");
  const SymbolPathReport report =
      applySymbolSearchPaths(service, {pathToUtf8(layout.symsDir())}, {}, layout.root.path() / "cache");
  EXPECT_EQ(report.overrides, 0u);
  EXPECT_EQ(report.problems.size(), kMaxSymbolPathProblems + 1);
  EXPECT_NE(report.problems.back().find("150 more"), std::string::npos) << report.problems.back();
}

TEST(PolishSymbolScan, AnUnreadableDirectoryIsReported) {
  if (isRoot()) GTEST_SKIP() << "root can read any directory";
#if !defined(_WIN32)
  BigLayout layout("polish_scan_unreadable", 0);
  const fs::path locked = layout.root.path() / "locked";
  fs::create_directories(locked);
  fs::permissions(locked, fs::perms::none);
  symbols::SymbolService service({}, layout.root.path() / "cache");
  const SymbolPathReport report =
      applySymbolSearchPaths(service, {pathToUtf8(locked)}, {}, layout.root.path() / "cache");
  EXPECT_TRUE(anyContains(report.problems, "cannot read the directory")) << "reported, not accepted silently";
  EXPECT_FALSE(validateSymbolPath(locked).ok());
  Harness h;
  ASSERT_TRUE(h.open(layout.trace()));
  std::string error;
  EXPECT_FALSE(h.app.addSymbolPath(locked, error));
  EXPECT_NE(error.find("cannot read the directory"), std::string::npos);
  EXPECT_TRUE(h.app.settings().symbolSearchPaths.empty());
  fs::permissions(locked, fs::perms::owner_all);
#endif
}

TEST(PolishSymbolScan, AMissingConfiguredDirectoryIsReportedByTheRun) {
  BigLayout layout("polish_scan_missing", 0);
  Harness h;
  h.app.settings().symbolSearchPaths = {pathToUtf8(layout.root.path() / "gone")};
  ASSERT_TRUE(h.open(layout.trace()));
  ASSERT_TRUE(h.until([&] { return !h.app.symbolPathScanRunning() && h.symbolsDone(); }));
  ASSERT_FALSE(h.app.symbolPathProblems().empty());
  EXPECT_TRUE(anyContains(h.app.symbolPathProblems(), "does not exist"));
  EXPECT_TRUE(toastContains(h.app, "does not exist"));
}

TEST(PolishSymbolScan, ThreeThousandFilesDoNotStallAFrame) {
  BigLayout layout("polish_scan_3000", 3000);
  Harness h;
  ASSERT_TRUE(h.open(layout.trace()));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  h.frames(2);
  EXPECT_EQ(h.app.statusBar().summary().failed, 2u);

  std::string error;
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  ASSERT_TRUE(h.app.addSymbolPath(layout.symsDir(), error)) << error;
  const double addMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  EXPECT_TRUE(h.app.symbolPathScanRunning() || h.app.document()->symbols()->progress().total > 0);

  // Frames keep coming while the worker reads the 3000 files; none of them takes long.
  double worstMs = 0;
  bool sawScanning = h.app.symbolPathScanRunning();
  const auto deadline = Clock::now() + std::chrono::seconds(120);
  while ((h.app.symbolPathScanRunning() || !h.symbolsDone()) && Clock::now() < deadline) {
    const auto t0 = Clock::now();
    h.app.frame(0);
    worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    sawScanning |= h.app.symbolPathScanRunning();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(h.symbolsDone());
  EXPECT_TRUE(sawScanning) << "the scan is a background run, not part of addSymbolPath";
  // The scan itself takes seconds under ASan (7.7 s measured before); a frame must not carry it.
  EXPECT_LT(addMs, 1000.0) << "addSymbolPath returns without scanning";
  EXPECT_LT(worstMs, 1000.0) << "no frame waits for the scan";
  h.frames(3);
  EXPECT_EQ(h.app.statusBar().summary().resolved, 2u) << "the overrides were registered before the resolve";
  EXPECT_TRUE(h.app.symbolPathProblems().empty());
}

TEST(PolishSymbolScan, RerunningWhileTheScanIsStillGoingIsClean) {
  BigLayout layout("polish_scan_rerun", 1500);
  Harness h;
  h.app.settings().symbolSearchPaths = {pathToUtf8(layout.symsDir())};
  ASSERT_TRUE(h.open(layout.trace()));
  for (int i = 0; i < 3; ++i) h.app.rerunSymbolResolution();  // joins the earlier workers
  ASSERT_TRUE(h.until([&] { return !h.app.symbolPathScanRunning() && h.symbolsDone(); }, 120));
  h.frames(2);
  EXPECT_EQ(h.app.statusBar().summary().resolved, 2u);
  // Closing the document or the App during a scan is clean too.
  h.app.rerunSymbolResolution();
}

TEST(PolishSymbolScan, TheSymbolsPanelListsWhatTheScanFound) {
  BigLayout layout("polish_scan_panel", 30);
  Harness h;
  h.app.settings().symbolSearchPaths = {pathToUtf8(layout.root.path() / "gone")};
  ASSERT_TRUE(h.open(layout.trace()));
  ASSERT_TRUE(h.until([&] { return !h.app.symbolPathScanRunning() && h.symbolsDone(); }));
  PanelEntry* e = h.app.panels().find(SymbolsPanel::kName);
  ASSERT_NE(e, nullptr);
  e->open = true;
  h.frames(3);
  EXPECT_FALSE(h.app.symbolPathProblems().empty());
}
