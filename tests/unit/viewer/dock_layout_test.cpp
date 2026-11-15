// WP-67c / WP-67e: the default docked layout (Superluminal arrangement: Function Statistics strip, Threads
// Timeline, bottom row with the analysis tab group and Source). Runs the real App with the null renderer
// (ImGui computes dock nodes without a GPU), so docking, saved layouts and "Reset layout" are exercised for
// real.
#include "viewer/dock_layout.h"
#include "analysis/query.h"
#include "viewer/app.h"
#include "viewer/stats/function_stats_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

const char* const kDefaultOpen[] = {"Function Statistics", "Trace Info", "Threads Timeline",   "Call Graph",
                                    "Function List",       "Butterfly",  "Thread Interaction", "Source"};
const char* const kDefaultClosed[] = {"Symbols", "Capture", "Flight Recorder", "Session Explorer", "Live"};

AppOptions headless() {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.autoConnectAgent = false;
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "dock_layout_test_no_such_settings.json";
  return o;
}

// A fresh config directory (no settings.json, no imgui.ini) that is removed on scope exit.
struct TempDir {
  explicit TempDir(const char* name) : path(fs::path(TACHYON_BINARY_DIR) / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path path;
};

AppOptions persisted(const fs::path& dir) {
  AppOptions o = headless();
  o.persist = true;
  o.configPath = dir / "settings.json";
  return o;
}

void runFrames(App& app, int n = 6) {
  for (int i = 0; i < n; ++i) ASSERT_TRUE(app.frame(0));
}

ImGuiWindow* win(const char* name) {
  return ImGui::FindWindowByName(name);
}

const ImGuiDockNode* nodeOf(const char* name) {
  const ImGuiWindow* w = win(name);
  return w ? w->DockNode : nullptr;
}

bool docked(const char* name) {
  const ImGuiWindow* w = win(name);
  return w != nullptr && w->DockId != 0 && w->DockNode != nullptr && w->DockNode->IsLeafNode();
}

bool overlap(const ImRect& a, const ImRect& b) {
  return a.Min.x < b.Max.x - 1.0f && b.Min.x < a.Max.x - 1.0f && a.Min.y < b.Max.y - 1.0f &&
         b.Min.y < a.Max.y - 1.0f;
}

void openAll(App& app) {
  for (const char* name : kDefaultClosed) {
    PanelEntry* p = app.panels().find(name);
    ASSERT_NE(p, nullptr) << name;
    p->open = true;
  }
}

}  // namespace

TEST(DockLayout, SlotsOfBuiltinPanels) {
  EXPECT_EQ(dockSlotFor("Threads Timeline"), DockSlot::Timeline);
  EXPECT_EQ(dockSlotFor("Function Statistics"), DockSlot::Stats);
  EXPECT_EQ(dockSlotFor("Trace Info"), DockSlot::Stats);
  for (const char* n : {"Call Graph", "Function List", "Butterfly", "Thread Interaction", "Capture",
                        "Flight Recorder", "Live", "Session Explorer", "Symbols"})
    EXPECT_EQ(dockSlotFor(n), DockSlot::Analysis) << n;
  EXPECT_EQ(dockSlotFor("Source"), DockSlot::Source);
  EXPECT_EQ(dockSlotFor("Recorder"), DockSlot::None);
  for (const char* n : kDefaultOpen) EXPECT_NE(dockSlotFor(n), DockSlot::None) << n;
  for (const char* n : kDefaultClosed) EXPECT_NE(dockSlotFor(n), DockSlot::None) << n;
}

namespace {

// The proportions of the default layout at one window size.
void expectSuperluminalArrangement(float w, float h) {
  App app(makeNullRenderer(w, h), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);

  for (const char* n : kDefaultOpen) EXPECT_TRUE(docked(n)) << n << " is not docked at " << w << "x" << h;
  const ImGuiDockNode* stats = nodeOf("Function Statistics");
  const ImGuiDockNode* timeline = nodeOf("Threads Timeline");
  const ImGuiDockNode* analysis = nodeOf("Call Graph");
  const ImGuiDockNode* source = nodeOf("Source");
  ASSERT_TRUE(stats && timeline && analysis && source);

  // Tab groups: Function Statistics + Trace Info on top; Call Graph, Function List, Butterfly and Thread
  // Interaction together at the bottom left; the timeline and Source alone in their nodes.
  EXPECT_EQ(nodeOf("Trace Info"), stats);
  for (const char* n : {"Function List", "Butterfly", "Thread Interaction"})
    EXPECT_EQ(nodeOf(n), analysis) << n;
  const ImGuiDockNode* distinct[] = {stats, timeline, analysis, source};
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      EXPECT_NE(distinct[i], distinct[j]);
      EXPECT_FALSE(overlap(distinct[i]->Rect(), distinct[j]->Rect())) << i << " overlaps " << j;
    }
  }

  // The dockspace is the viewport's work area (between the menu bar / session tabs and the status bar).
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  EXPECT_GT(vp->WorkPos.y, 0.0f);
  EXPECT_LT(vp->WorkPos.y + vp->WorkSize.y, vp->Size.y);
  // Top to bottom: stats strip (~20%), timeline (~45%), bottom row (~35%), each across the full width except
  // the bottom row, which splits ~65 / 35.
  EXPECT_NEAR(stats->Size.x, vp->WorkSize.x, 2.0f);
  EXPECT_NEAR(stats->Size.y / vp->WorkSize.y, 0.20f, 0.03f);
  EXPECT_NEAR(timeline->Size.x, vp->WorkSize.x, 2.0f);
  EXPECT_NEAR(timeline->Size.y / vp->WorkSize.y, 0.45f, 0.03f);
  EXPECT_GE(timeline->Pos.y, stats->Pos.y + stats->Size.y - 1.0f);
  for (const ImGuiDockNode* bottom : {analysis, source}) {
    EXPECT_GE(bottom->Pos.y, timeline->Pos.y + timeline->Size.y - 1.0f);
    EXPECT_NEAR(bottom->Size.y / vp->WorkSize.y, 0.35f, 0.03f);
    EXPECT_LE(bottom->Pos.y + bottom->Size.y, vp->WorkPos.y + vp->WorkSize.y + 1.0f);
  }
  EXPECT_LT(analysis->Pos.x, source->Pos.x);
  EXPECT_NEAR(analysis->Size.x / vp->WorkSize.x, 0.65f, 0.03f);
  EXPECT_NEAR(source->Size.x / vp->WorkSize.x, 0.35f, 0.03f);

  // The tab groups open on their main panel.
  EXPECT_EQ(stats->SelectedTabId, win("Function Statistics")->TabId);
  EXPECT_EQ(analysis->SelectedTabId, win("Call Graph")->TabId);
}

}  // namespace

TEST(DockLayout, FreshConfigDocksEveryDefaultPanelLikeSuperluminalAt1600x900) {
  expectSuperluminalArrangement(1600.0f, 900.0f);
}
TEST(DockLayout, FreshConfigDocksEveryDefaultPanelLikeSuperluminalAt1920x1080) {
  expectSuperluminalArrangement(1920.0f, 1080.0f);
}

TEST(DockLayout, SourceAndFunctionStatisticsAreOpenByDefault) {
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  for (const char* n : kDefaultOpen) {
    PanelEntry* p = app.panels().find(n);
    ASSERT_NE(p, nullptr) << n;
    EXPECT_TRUE(p->open) << n;
  }
  for (const char* n : kDefaultClosed) {
    PanelEntry* p = app.panels().find(n);
    ASSERT_NE(p, nullptr) << n;
    EXPECT_FALSE(p->open) << n;
  }
}

TEST(DockLayout, ClosedPanelsOpenDockedIntoTheAnalysisGroup) {
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);
  openAll(app);
  runFrames(app);

  const ImGuiDockNode* analysis = nodeOf("Call Graph");
  for (const char* n : kDefaultClosed) {
    EXPECT_TRUE(docked(n)) << n;
    EXPECT_EQ(nodeOf(n), analysis) << n;  // not floating over the timeline
  }
}

TEST(DockLayout, SavedLayoutIsKeptAcrossRestarts) {
  TempDir dir("dock_layout_test_saved");
  const ImVec2 floatPos(137.0f, 203.0f);
  {
    App app(makeNullRenderer(1600.0f, 900.0f), persisted(dir.path));
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    runFrames(app);
    ASSERT_TRUE(docked("Call Graph"));
    // The user tears the Call Graph out of the layout and drags it somewhere else.
    ImGuiWindow* w = win("Call Graph");
    ImGui::SetWindowDock(w, 0, ImGuiCond_Always);
    ImGui::SetWindowPos(w, floatPos, ImGuiCond_Always);
    ImGui::SetWindowSize(w, ImVec2(333.0f, 222.0f), ImGuiCond_Always);
    runFrames(app);
    ASSERT_FALSE(docked("Call Graph"));
  }  // the layout is written to imgui.ini next to settings.json when the context is destroyed
  ASSERT_TRUE(fs::exists(dir.path / "imgui.ini"));

  App app(makeNullRenderer(1600.0f, 900.0f), persisted(dir.path));
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);
  EXPECT_FALSE(docked("Call Graph")) << "the default layout replaced a saved one";
  EXPECT_EQ(win("Call Graph")->DockId, 0u);
  EXPECT_NEAR(win("Call Graph")->Pos.x, floatPos.x, 1.0f);
  EXPECT_NEAR(win("Call Graph")->Pos.y, floatPos.y, 1.0f);
  EXPECT_NEAR(win("Call Graph")->Size.x, 333.0f, 1.0f);
  for (const char* n : {"Threads Timeline", "Function List", "Butterfly", "Source"})
    EXPECT_TRUE(docked(n)) << n;
}

TEST(DockLayout, HandWrittenFloatingLayoutIsKept) {
  TempDir dir("dock_layout_test_floating");
  {
    std::ofstream ini(dir.path / "imgui.ini");
    ini << "[Window][Threads Timeline]\nPos=60,60\nSize=500,300\n\n"
           "[Window][Butterfly]\nPos=700,400\nSize=400,300\n\n";
  }
  App app(makeNullRenderer(1600.0f, 900.0f), persisted(dir.path));
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);
  EXPECT_FALSE(docked("Threads Timeline"));
  EXPECT_NEAR(win("Threads Timeline")->Pos.x, 60.0f, 1.0f);
  EXPECT_NEAR(win("Butterfly")->Pos.y, 400.0f, 1.0f);
  EXPECT_FALSE(docked("Butterfly"));
}

TEST(DockLayout, ResetLayoutRedocksFloatedPanels) {
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);
  const ImGuiID callGraphNode = win("Call Graph")->DockId;
  ASSERT_NE(callGraphNode, 0u);

  ImGui::SetWindowDock(win("Call Graph"), 0, ImGuiCond_Always);
  ImGui::SetWindowDock(win("Threads Timeline"), 0, ImGuiCond_Always);
  runFrames(app);
  ASSERT_FALSE(docked("Call Graph"));
  ASSERT_FALSE(docked("Threads Timeline"));

  ASSERT_TRUE(app.commands().run("view.layout.reset"));
  runFrames(app);
  for (const char* n : kDefaultOpen) EXPECT_TRUE(docked(n)) << n;
  EXPECT_NE(nodeOf("Call Graph"), nodeOf("Threads Timeline"));
  EXPECT_FALSE(overlap(nodeOf("Call Graph")->Rect(), nodeOf("Threads Timeline")->Rect()));
  EXPECT_EQ(nodeOf("Thread Interaction"), nodeOf("Call Graph"));

  // Closed panels dock too after a reset (also a panel that was floated while open).
  PanelEntry* symbols = app.panels().find("Symbols");
  ASSERT_NE(symbols, nullptr);
  symbols->open = true;
  runFrames(app);
  EXPECT_EQ(nodeOf("Symbols"), nodeOf("Call Graph"));
}

TEST(DockLayout, ResetLayoutIsInTheCommandRegistry) {
  App app(makeNullRenderer(), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  bool found = false;
  for (const Command& c : app.commands().commands()) found |= c.id == "view.layout.reset";
  EXPECT_TRUE(found);
}

TEST(DockLayout, TinyOrZeroSizedViewportDoesNotCrashAndBuildsLater) {
  for (const float extent : {0.0f, 1.0f, 8.0f}) {
    App app(makeNullRenderer(extent, extent), headless());
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    runFrames(app, 4);
    ASSERT_TRUE(app.commands().run("view.layout.reset"));
    runFrames(app, 4);
  }
  // The builder itself refuses a degenerate size and leaves the dockspace alone.
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  EXPECT_FALSE(buildDefaultDockLayout(1234u, ImVec2(0.0f, 0.0f), {"Call Graph"}));
  EXPECT_FALSE(buildDefaultDockLayout(1234u, ImVec2(100.0f, 3.0f), {"Call Graph"}));
}

// WP-67d: in the default docked layout the function name column of the analysis tables stays readable.
namespace {

const ImGuiTable* tableOf(const char* panel, int columns) {
  const ImGuiWindow* outer = win(panel);
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (int i = 0; i < g.Tables.GetMapSize(); ++i) {
    const ImGuiTable* t = g.Tables.TryGetMapData(i);
    // (a panel may put its table in a child window: the Call Graph does when the pie sits beside it)
    if (t != nullptr && (t->OuterWindow == outer || t->OuterWindow->ParentWindow == outer) &&
        t->ColumnsCount == columns && t->LastFrameActive >= g.FrameCount - 1)
      return t;
  }
  return nullptr;
}

void expectReadableNameColumns(float w, float h) {
  App app(makeNullRenderer(w, h), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  app.openFile(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.tachyon");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (app.document() == nullptr || app.loading()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  app.selection().setSelectedFunction(analysis::kRootKey);  // gives the Butterfly something to show
  const float em = ImGui::GetFontSize();
  for (const char* panel : {"Function List", "Call Graph", "Butterfly"}) {
    const ImGuiTable* t = nullptr;
    // The analysis panels are tabs of one node (WP-67e): bring this one to the front before looking.
    ImGui::SetWindowFocus(panel);
    for (int i = 0; i < 5000 && (t = tableOf(panel, 7)) == nullptr; ++i) {
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(t, nullptr) << panel << " has no table";
    for (int i = 0; i < 60; ++i) app.frame(0);
    t = tableOf(panel, 7);
    ASSERT_NE(t, nullptr) << panel;
    EXPECT_GE(t->Columns[0].WidthGiven, 12.0f * em) << panel << " at " << w << "x" << h;
  }
}

}  // namespace

TEST(DockLayout, NameColumnsStayReadableAt1600x900) {
  expectReadableNameColumns(1600.0f, 900.0f);
}
TEST(DockLayout, NameColumnsStayReadableAt1280x720) {
  expectReadableNameColumns(1280.0f, 720.0f);
}

// ---- WP-67e: the layout version
// -----------------------------------------------------------------------------

namespace {

void writeText(const fs::path& p, const std::string& text) {
  std::ofstream(p, std::ios::binary) << text;
}

// An install from before the versioned layout: settings.json without "layoutVersion" and an imgui.ini whose
// Call Graph floats at a known place.
void writeOldInstall(const fs::path& dir, const std::string& settings) {
  writeText(dir / "settings.json", settings);
  writeText(dir / "imgui.ini", "[Window][Call Graph]\nPos=60,60\nSize=500,300\n\n");
}

}  // namespace

TEST(DockLayout, OldSavedLayoutIsReplacedByTheNewDefaultOnceAndNeverAgain) {
  TempDir dir("dock_layout_test_version");
  writeOldInstall(dir.path, R"({"theme":"light"})");  // no layoutVersion: saved before WP-67e
  ASSERT_EQ(Settings::load(dir.path / "settings.json").layoutVersion, 0u);
  {
    App app(makeNullRenderer(1600.0f, 900.0f), persisted(dir.path));
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    runFrames(app);
    for (const char* n : kDefaultOpen) EXPECT_TRUE(docked(n)) << n << ": the new default was not applied";
    EXPECT_EQ(nodeOf("Function List"), nodeOf("Call Graph"));
    // The bump is on disk at once (not only at exit).
    EXPECT_EQ(Settings::load(dir.path / "settings.json").layoutVersion, Settings::kLayoutVersion);
    // The user rearranges: tears Call Graph out and puts it somewhere else.
    ImGuiWindow* w = win("Call Graph");
    ImGui::SetWindowDock(w, 0, ImGuiCond_Always);
    ImGui::SetWindowPos(w, ImVec2(211.0f, 157.0f), ImGuiCond_Always);
    runFrames(app);
    ASSERT_FALSE(docked("Call Graph"));
  }
  for (int run = 0; run < 2; ++run) {  // the layout saved after the bump is never overwritten again
    App app(makeNullRenderer(1600.0f, 900.0f), persisted(dir.path));
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    runFrames(app);
    EXPECT_FALSE(docked("Call Graph")) << "run " << run << ": the default replaced the user's layout";
    EXPECT_NEAR(win("Call Graph")->Pos.x, 211.0f, 1.0f);
    EXPECT_NEAR(win("Call Graph")->Pos.y, 157.0f, 1.0f);
    EXPECT_TRUE(docked("Threads Timeline"));
    EXPECT_EQ(app.settings().layoutVersion, Settings::kLayoutVersion);
  }
}

TEST(DockLayout, ALayoutOfTheCurrentVersionIsKeptEvenIfTheDefaultWouldDiffer) {
  TempDir dir("dock_layout_test_current");
  writeOldInstall(dir.path, "{\"layoutVersion\":" + std::to_string(Settings::kLayoutVersion) + "}");
  App app(makeNullRenderer(1600.0f, 900.0f), persisted(dir.path));
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);
  EXPECT_FALSE(docked("Call Graph"));
  EXPECT_NEAR(win("Call Graph")->Pos.x, 60.0f, 1.0f);
}

TEST(DockLayout, AnOlderLayoutVersionIsReplacedToo) {
  TempDir dir("dock_layout_test_v1");
  writeOldInstall(dir.path, R"({"layoutVersion":1})");
  App app(makeNullRenderer(1600.0f, 900.0f), persisted(dir.path));
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);
  EXPECT_TRUE(docked("Call Graph"));
}

// ---- WP-67e: the session tab strip
// --------------------------------------------------------------------------

TEST(DockLayout, SessionTabStripSitsBetweenTheMenuBarAndTheDockspace) {
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  app.openFile(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.tachyon");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (app.document() == nullptr || app.loading()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  runFrames(app);
  ImGuiWindow* strip = win("##SessionTabs");
  ASSERT_NE(strip, nullptr);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  EXPECT_GT(strip->Pos.y, 0.0f);  // below the menu bar
  EXPECT_LT(strip->Size.y, 40.0f);
  EXPECT_NEAR(strip->Size.x, vp->Size.x, 2.0f);
  EXPECT_GE(vp->WorkPos.y, strip->Pos.y + strip->Size.y - 1.0f);  // the dockspace starts below it

  // The tab's close button closes the document: find it by hovering along the strip, then click.
  ImGuiIO& io = ImGui::GetIO();
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiID closeId = strip->GetID("##closesession");
  const float y = strip->Pos.y + strip->Size.y * 0.5f;
  bool found = false;
  for (float x = strip->Pos.x; x < strip->Pos.x + 400.0f && !found; x += 3.0f) {
    io.AddMousePosEvent(x, y);
    app.frame(0);
    found = g.HoveredId == closeId;
  }
  ASSERT_TRUE(found) << "the close button of the session tab was not found";
  io.AddMouseButtonEvent(0, true);
  app.frame(0);
  io.AddMouseButtonEvent(0, false);
  runFrames(app, 3);
  EXPECT_EQ(app.document(), nullptr);
  io.AddMousePosEvent(-1000.0f, -1000.0f);
  runFrames(app, 2);
}

// ---- WP-67e: the wide Function Statistics strip and the Call Graph pie beside the table
// -------------------------

namespace {

bool hasChildWindow(const char* parent, const char* childPart) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (ImGuiWindow* w : g.Windows) {
    const std::string name = w->Name;
    if (w->Active && w->ParentWindow != nullptr && name.find(parent) == 0 &&
        name.find(childPart) != std::string::npos)
      return true;
  }
  return false;
}

void openTiny(App& app) {
  app.openFile(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.tachyon");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (app.document() == nullptr || app.loading()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

TEST(DockLayout, TheStatisticsStripLaysItsPartsOutSideBySide) {
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  openTiny(app);
  auto* panel = dynamic_cast<FunctionStatsPanel*>(app.panels().find(FunctionStatsPanel::kName)->panel.get());
  ASSERT_NE(panel, nullptr);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  const auto until = [&](const std::function<bool()>& pred) {
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  };
  ASSERT_TRUE(
      until([&] { return panel->index() != nullptr && !panel->indexPending() && !panel->hits().empty(); }));
  PanelContext context;
  context.document = app.document().get();
  context.selection = &app.selection();
  context.settings = &app.settings();
  context.jobs = &app.jobs();
  context.queryExecutor = &app.queryExecutor();
  context.documentGeneration = app.documentGeneration();
  context.symbols = app.document()->symbols().get();
  context.symbolGeneration = app.symbolGeneration();
  // The first entry that has instances (the order of the hits differs between machines).
  bool found = false;
  for (size_t row = 0; row < std::min<size_t>(panel->hits().size(), 12) && !found; ++row) {
    panel->chooseResult(context, row);
    ASSERT_TRUE(until([&] {
      return panel->selected() && panel->result() != nullptr &&
             panel->result()->key == panel->selected()->key && !panel->statsPending();
    }));
    found = panel->result()->instances != nullptr && !panel->result()->instances->empty() &&
            panel->result()->stats != nullptr && panel->result()->stats->count > 0;
  }
  ASSERT_TRUE(found) << "no entry of the tiny trace has instances";
  runFrames(app, 3);

  ASSERT_TRUE(docked("Function Statistics"));
  // The four parts are child windows of the strip; the stacked arrangement's extras are not drawn.
  for (const char* part : {"##sw_list", "##sw_bars", "##sw_hist", "##sw_stats"})
    EXPECT_TRUE(hasChildWindow("Function Statistics", part)) << part;
  EXPECT_FALSE(panel->lastHistogram().bars.empty());  // the histogram box
  EXPECT_FALSE(panel->lastBoxPlot().valid);           // the box plot belongs to the stacked arrangement
  ASSERT_TRUE(panel->result()->instances != nullptr);
  EXPECT_FALSE(panel->result()->instances->empty());
  ASSERT_TRUE(panel->goToInstance(context, 0));
  EXPECT_EQ(panel->currentInstance(), std::optional<uint32_t>(0));
}

TEST(DockLayout, TheCallGraphPieSitsBesideTheTableOnlyWhereThereIsRoom) {
  struct Case {
    float w, h;
    bool aside;
  };
  for (const Case c : {Case{1600.0f, 900.0f, true}, Case{1280.0f, 720.0f, false}}) {
    App app(makeNullRenderer(c.w, c.h), headless());
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    openTiny(app);
    runFrames(app, 30);
    EXPECT_EQ(hasChildWindow("Call Graph", "##cgpiecol"), c.aside) << c.w << "x" << c.h;
    EXPECT_TRUE(hasChildWindow("Call Graph", "##cgpie"))
        << c.w << "x" << c.h;  // the pie itself is always there
  }
}
