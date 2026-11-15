// WP-67c: the default docked layout. Runs the real App with the null renderer (ImGui computes dock nodes
// without a GPU), so docking, saved layouts and "Reset layout" are exercised for real.
#include "viewer/dock_layout.h"
#include "viewer/app.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

const char* const kDefaultOpen[] = {"Trace Info", "Threads Timeline", "Thread Interaction",
                                    "Call Graph", "Function List",    "Butterfly"};
const char* const kDefaultClosed[] = {"Function Statistics", "Source",           "Symbols", "Capture",
                                      "Flight Recorder",     "Session Explorer", "Live"};

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
  EXPECT_EQ(dockSlotFor("Call Graph"), DockSlot::CallGraph);
  EXPECT_EQ(dockSlotFor("Function List"), DockSlot::FunctionList);
  EXPECT_EQ(dockSlotFor("Butterfly"), DockSlot::Butterfly);
  EXPECT_EQ(dockSlotFor("Source"), DockSlot::Butterfly);
  EXPECT_EQ(dockSlotFor("Live"), DockSlot::FunctionList);
  EXPECT_EQ(dockSlotFor("Recorder"), DockSlot::None);
  for (const char* n : kDefaultOpen) EXPECT_NE(dockSlotFor(n), DockSlot::None) << n;
  for (const char* n : kDefaultClosed) EXPECT_NE(dockSlotFor(n), DockSlot::None) << n;
}

TEST(DockLayout, FreshConfigDocksEveryDefaultPanelWithoutOverlap) {
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);

  for (const char* n : kDefaultOpen) EXPECT_TRUE(docked(n)) << n << " is not docked";
  const ImGuiDockNode* timeline = nodeOf("Threads Timeline");
  const ImGuiDockNode* callGraph = nodeOf("Call Graph");
  const ImGuiDockNode* functionList = nodeOf("Function List");
  const ImGuiDockNode* butterfly = nodeOf("Butterfly");
  ASSERT_TRUE(timeline && callGraph && functionList && butterfly);

  // Thread Interaction and Trace Info are tabs of the Butterfly node; the timeline has a node of its own.
  EXPECT_EQ(nodeOf("Thread Interaction"), butterfly);
  EXPECT_EQ(nodeOf("Trace Info"), butterfly);
  const ImGuiDockNode* distinct[] = {timeline, callGraph, functionList, butterfly};
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      EXPECT_NE(distinct[i], distinct[j]);
      EXPECT_FALSE(overlap(distinct[i]->Rect(), distinct[j]->Rect())) << i << " overlaps " << j;
    }
  }

  // Timeline on top, full width, about 55% of the dockspace; the other three side by side below it.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  EXPECT_NEAR(timeline->Size.x, vp->WorkSize.x, 2.0f);
  EXPECT_NEAR(timeline->Size.y / vp->WorkSize.y, 0.55f, 0.03f);
  for (const ImGuiDockNode* below : {callGraph, functionList, butterfly}) {
    EXPECT_GE(below->Pos.y, timeline->Pos.y + timeline->Size.y - 1.0f);
  }
  EXPECT_LT(callGraph->Pos.x, functionList->Pos.x);
  EXPECT_LT(functionList->Pos.x, butterfly->Pos.x);

  // The dockspace sits between the menu bar and the status bar: it is exactly the viewport's work area.
  EXPECT_GT(vp->WorkPos.y, 0.0f);
  EXPECT_LT(vp->WorkPos.y + vp->WorkSize.y, vp->Size.y);
  EXPECT_GE(timeline->Pos.y, vp->WorkPos.y - 1.0f);
  for (const ImGuiDockNode* below : {callGraph, functionList, butterfly}) {
    EXPECT_LE(below->Pos.y + below->Size.y, vp->WorkPos.y + vp->WorkSize.y + 1.0f);
  }
}

TEST(DockLayout, ClosedPanelsOpenDockedIntoTheirSlot) {
  App app(makeNullRenderer(1600.0f, 900.0f), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  runFrames(app);
  openAll(app);
  runFrames(app);

  const ImGuiDockNode* butterfly = nodeOf("Butterfly");
  const ImGuiDockNode* functionList = nodeOf("Function List");
  EXPECT_TRUE(docked("Source"));
  EXPECT_EQ(nodeOf("Source"), butterfly);
  for (const char* n :
       {"Function Statistics", "Symbols", "Capture", "Flight Recorder", "Session Explorer", "Live"}) {
    EXPECT_TRUE(docked(n)) << n;
    EXPECT_EQ(nodeOf(n), functionList) << n;
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
  for (const char* n : {"Threads Timeline", "Function List", "Butterfly"}) EXPECT_TRUE(docked(n)) << n;
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
  EXPECT_EQ(nodeOf("Thread Interaction"), nodeOf("Butterfly"));

  // Closed panels dock too after a reset (also a panel that was floated while open).
  PanelEntry* source = app.panels().find("Source");
  ASSERT_NE(source, nullptr);
  source->open = true;
  runFrames(app);
  EXPECT_EQ(nodeOf("Source"), nodeOf("Butterfly"));
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
