// WP-41: the Session Explorer panel inside a headless App: rows from the recent files and the captures
// directory, Open / Reveal / Delete / Forget, the confirmation flow, and survival of broken rows.
#include "capture_test_util.h"

#include <fstream>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

namespace {

AppOptions explorerOptions(std::shared_ptr<IProcessLauncher> launcher) {
  AppOptions o = headlessOptions("explorer");
  o.processLauncher = std::move(launcher);
  o.autoConnectAgent = false;
  return o;
}

struct ExplorerRig {
  explicit ExplorerRig(const std::string& tag)
      : dir(tag), launcher(std::make_shared<TestLauncher>()), h(explorerOptions(launcher)) {
    captures = dir.path() / "captures";
    fs::create_directories(captures);
    h.app.settings().capture.capturesDir = pathToUtf8(captures);
    PanelEntry* entry = h.app.panels().find(kSessionExplorerPanelName);
    EXPECT_NE(entry, nullptr);
    entry->open = true;
    panel = dynamic_cast<SessionExplorerPanel*>(entry->panel.get());
    EXPECT_NE(panel, nullptr);
  }
  // Frames until the catalog went idle and its rows reached the panel.
  void settle(size_t expectedRows) {
    ASSERT_TRUE(h.until([&] {
      panel->poll();
      return !panel->catalog().busy() && panel->rows().size() == expectedRows;
    })) << "rows: "
        << panel->rows().size();
    h.frames(2);
  }
  const SessionInfo* row(const fs::path& path) {
    for (const SessionInfo& r : panel->rows())
      if (r.path == path) return &r;
    return nullptr;
  }

  TempDir dir;
  fs::path captures;
  std::shared_ptr<TestLauncher> launcher;
  Harness h;
  SessionExplorerPanel* panel = nullptr;
};

}  // namespace

TEST(SessionExplorerPanel, IsRegisteredAndClosedByDefault) {
  Harness h(explorerOptions(nullptr));
  PanelEntry* entry = h.app.panels().find(kSessionExplorerPanelName);
  ASSERT_NE(entry, nullptr);
  EXPECT_FALSE(entry->open);
  EXPECT_NE(h.app.panels().find(kCapturePanelName), nullptr);
  h.frames(3);  // closed panels draw nothing and start no scan
  EXPECT_TRUE(h.app.commands().run("capture.explorer"));
  EXPECT_TRUE(entry->open);
}

TEST(SessionExplorerPanel, ListsTheCapturesDirectoryAndRecentFiles) {
  ExplorerRig rig("explorer_list");
  fs::copy_file(fixture("tiny.tachyon"), rig.captures / "one.tachyon");
  fs::copy_file(fixture("medium.tachyon"), rig.captures / "two.tachyon");
  const fs::path outside = rig.dir.path() / "outside.tachyon";
  fs::copy_file(fixture("tiny.tachyon"), outside);
  rig.h.app.settings().recentFiles = {pathToUtf8(outside), pathToUtf8(rig.dir.path() / "stale.tachyon")};
  rig.h.frames(2);
  rig.settle(4);

  const SessionInfo* one = rig.row(rig.captures / "one.tachyon");
  ASSERT_NE(one, nullptr);
  EXPECT_EQ(one->state, SessionState::Ok);
  EXPECT_EQ(one->agentVersion.rfind("tracegen-", 0), 0u);
  EXPECT_GT(one->samplingHz, 0u);
  EXPECT_GT(one->sizeBytes, 0u);
  EXPECT_TRUE(rig.row(outside)->recent);
  ASSERT_NE(rig.row(rig.dir.path() / "stale.tachyon"), nullptr);
  EXPECT_EQ(rig.row(rig.dir.path() / "stale.tachyon")->state, SessionState::Missing);
  // The medium fixture is bigger: size sort puts it first.
  rig.panel->setSort(SessionSort::Size, false);
  EXPECT_EQ(rig.panel->visibleRows().front().path, rig.captures / "two.tachyon");
  rig.panel->setSort(SessionSort::Name, true);
  EXPECT_EQ(rig.panel->visibleRows().front().path.filename(), fs::path("one.tachyon"));
  rig.panel->setFilter("two");
  ASSERT_EQ(rig.panel->visibleRows().size(), 1u);
  EXPECT_EQ(rig.panel->visibleRows()[0].path.filename(), fs::path("two.tachyon"));
  rig.panel->setFilter("");
  EXPECT_EQ(rig.panel->visibleRows().size(), 4u);
  rig.h.frames(3);  // draws the table with every row state without crashing
}

TEST(SessionExplorerPanel, RescansWhenTheRecentFilesChange) {
  ExplorerRig rig("explorer_rescan");
  fs::copy_file(fixture("tiny.tachyon"), rig.captures / "one.tachyon");
  rig.h.frames(2);
  rig.settle(1);
  const fs::path outside = rig.dir.path() / "outside.tachyon";
  fs::copy_file(fixture("tiny.tachyon"), outside);
  rig.h.app.settings().addRecentFile(pathToUtf8(outside));
  rig.h.frames(2);
  rig.settle(2);
  ASSERT_NE(rig.row(outside), nullptr);
}

TEST(SessionExplorerPanel, OpenLoadsTheTrace) {
  ExplorerRig rig("explorer_open");
  fs::copy_file(fixture("tiny.tachyon"), rig.captures / "one.tachyon");
  rig.h.frames(2);
  rig.settle(1);
  rig.panel->open(rig.captures / "one.tachyon");
  ASSERT_TRUE(rig.h.until([&] { return !rig.h.app.loading() && rig.h.app.document() != nullptr; }));
  EXPECT_EQ(rig.h.app.document()->path(), rig.captures / "one.tachyon");
}

TEST(SessionExplorerPanel, RevealGoesThroughTheInjectedLauncher) {
  ExplorerRig rig("explorer_reveal");
  rig.launcher->spawn = false;
  fs::copy_file(fixture("tiny.tachyon"), rig.captures / "one.tachyon");
  rig.panel->reveal(rig.captures / "one.tachyon");
  ASSERT_EQ(rig.launcher->launches(), 1u);
  const LaunchSpec spec = rig.launcher->last();
  EXPECT_EQ(spec.exe, revealSpec(rig.captures / "one.tachyon").exe);
  EXPECT_FALSE(spec.elevate);
  rig.launcher->failWith = "no file manager";
  rig.panel->reveal(rig.captures / "one.tachyon");
  rig.h.frames(2);
  bool toasted = false;
  for (const Toast& t : rig.h.app.toasts().toasts())
    toasted |= t.text.find("no file manager") != std::string::npos;
  EXPECT_TRUE(toasted);
}

TEST(SessionExplorerPanel, DeleteAsksFirstAndRemovesTheTraceWithItsSiblings) {
  ExplorerRig rig("explorer_delete");
  const fs::path trace = rig.captures / "run.tachyon";
  fs::copy_file(fixture("tiny.tachyon"), trace);
  std::ofstream(rig.captures / "run.tachyon.etl") << "etl";
  fs::create_directories(rig.captures / "run.tachyon.perfspool");
  std::ofstream(rig.captures / "run.tachyon.perfspool" / "session.json") << "{}";
  fs::copy_file(fixture("tiny.tachyon"), rig.captures / "keep.tachyon");
  rig.h.app.settings().addRecentFile(pathToUtf8(trace));
  rig.h.frames(2);
  rig.settle(2);

  rig.panel->requestDelete(trace);
  EXPECT_TRUE(rig.panel->deleteArmed());
  EXPECT_EQ(rig.panel->pendingDelete().size(), 3u);
  rig.h.frames(3);  // the modal is drawn
  EXPECT_TRUE(fs::exists(trace)) << "nothing is deleted before the confirmation";

  rig.panel->cancelDelete();
  EXPECT_FALSE(rig.panel->deleteArmed());
  EXPECT_FALSE(rig.panel->confirmDelete()) << "nothing armed";
  EXPECT_TRUE(fs::exists(trace));

  rig.panel->requestDelete(trace);
  EXPECT_TRUE(rig.panel->confirmDelete());
  EXPECT_FALSE(fs::exists(trace));
  EXPECT_FALSE(fs::exists(rig.captures / "run.tachyon.etl"));
  EXPECT_FALSE(fs::exists(rig.captures / "run.tachyon.perfspool"));
  EXPECT_TRUE(fs::exists(rig.captures / "keep.tachyon"));
  for (const std::string& r : rig.h.app.settings().recentFiles) EXPECT_NE(r, pathToUtf8(trace));
  rig.settle(1);
  EXPECT_EQ(rig.panel->rows()[0].path.filename(), fs::path("keep.tachyon"));
}

TEST(SessionExplorerPanel, BrokenRowsNeverCrashAndCanBeForgotten) {
  ExplorerRig rig("explorer_broken");
  std::ofstream(rig.captures / "junk.tachyon") << "not a trace";
  fs::create_directories(rig.captures / "dir.tachyon");
  const fs::path stale = rig.dir.path() / "gone.tachyon";
  rig.h.app.settings().recentFiles = {pathToUtf8(stale)};
  rig.h.frames(2);
  rig.settle(2);  // a directory is not a capture: only the junk file and the stale entry are rows
  EXPECT_EQ(rig.row(rig.captures / "junk.tachyon")->state, SessionState::Unreadable);
  EXPECT_EQ(rig.row(stale)->state, SessionState::Missing);
  rig.h.frames(5);

  rig.panel->forget(stale);
  EXPECT_TRUE(rig.h.app.settings().recentFiles.empty());
  rig.h.frames(2);
  rig.settle(1);
  EXPECT_EQ(rig.row(stale), nullptr);
  EXPECT_TRUE(fs::exists(rig.captures / "junk.tachyon")) << "forgetting never touches the disk";
}

TEST(SessionExplorerPanel, ADirectoryThatDoesNotExistIsAnEmptyList) {
  ExplorerRig rig("explorer_nodir");
  rig.h.app.settings().capture.capturesDir = pathToUtf8(rig.dir.path() / "nowhere");
  rig.h.frames(3);
  rig.settle(0);
  rig.h.frames(2);
}
