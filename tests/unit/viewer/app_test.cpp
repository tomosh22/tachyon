#include "viewer/app.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

AppOptions headlessOptions() {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  // Never read the developer's real settings.
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_app_test_no_such_settings.json";
  return o;
}

class RecordingPanel final : public IPanel {
 public:
  const char* name() const override { return "Recorder"; }
  void draw(const PanelContext& context, bool* open) override {
    ++draws;
    sawDocument = context.document != nullptr;
    if (ImGui::Begin(name(), open)) ImGui::TextUnformatted("hello");
    ImGui::End();
  }
  void onSelectionChanged(const PanelContext&, const SelectionState& state, SelectionField field) override {
    lastField = field;
    lastThread = state.selectedThread;
    ++selectionEvents;
  }
  void onDocumentChanged(const Document* document) override {
    ++documentEvents;
    documentOpen = document != nullptr;
  }
  int draws = 0, selectionEvents = 0, documentEvents = 0;
  bool sawDocument = false, documentOpen = false;
  SelectionField lastField = SelectionField::TimeFilter;
  ThreadIdx lastThread;
};

bool frameUntil(App& app, const std::function<bool()>& pred) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

TEST(ViewerApp, RunsFramesWithNullRenderer) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  for (int i = 0; i < 20; ++i) ASSERT_TRUE(app.frame(0));
  EXPECT_EQ(app.frameCount(), 20u);
  EXPECT_FALSE(app.document());
}

TEST(ViewerApp, ThemeAndScaleApplyAtTheNextFrame) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  app.frame(0);
  const ImVec4 darkBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  app.setTheme(ThemeKind::Light);
  app.setUserScale(2.0f);
  app.frame(0);
  EXPECT_GT(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x, darkBg.x);
  EXPECT_FLOAT_EQ(ImGui::GetIO().FontGlobalScale, 2.0f);
}

TEST(ViewerApp, OpensTraceAsynchronouslyAndNotifiesPanels) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  auto recorder = std::make_unique<RecordingPanel>();
  RecordingPanel* panel = recorder.get();
  ASSERT_TRUE(app.panels().add(std::move(recorder)));
  app.selection().setSelectedThread(ThreadIdx(2));  // must be cleared by the load

  app.openFile(fixture("tiny.tachyon"));
  EXPECT_TRUE(app.loading());
  ASSERT_TRUE(frameUntil(app, [&] { return app.document() != nullptr; }));
  EXPECT_FALSE(app.loading());
  EXPECT_GT(app.document()->model().threads.size(), 0u);
  EXPECT_GT(app.document()->totalSamples(), 0u);
  EXPECT_EQ(panel->documentEvents, 1);
  EXPECT_TRUE(panel->documentOpen);
  EXPECT_FALSE(app.selection().state().selectedThread.valid());
  EXPECT_EQ(panel->lastField, SelectionField::All);
  app.frame(0);
  EXPECT_TRUE(panel->sawDocument);
  EXPECT_GT(panel->draws, 0);
  ASSERT_FALSE(app.settings().recentFiles.empty());

  // Service slots exist but are empty until WP-20/33/36 install implementations.
  EXPECT_EQ(app.document()->symbols(), nullptr);
  EXPECT_EQ(app.document()->queries(), nullptr);
  EXPECT_EQ(app.document()->lod(), nullptr);

  panel->selectionEvents = 0;
  app.selection().setSelectedThread(ThreadIdx(1));
  EXPECT_EQ(panel->selectionEvents, 1);
  EXPECT_EQ(panel->lastField, SelectionField::SelectedThread);

  app.commands().run("file.close");
  EXPECT_FALSE(app.document());
  EXPECT_FALSE(panel->documentOpen);
}

TEST(ViewerApp, MissingFileReportsErrorWithoutDocument) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  app.openFile(fixture("does-not-exist.tachyon"));
  ASSERT_TRUE(frameUntil(app, [&] { return !app.loading(); }));
  EXPECT_FALSE(app.document());
  EXPECT_FALSE(app.lastError().empty());
  app.frame(0);  // the status bar draws the error
}

TEST(ViewerApp, NewOpenSupersedesLoadInFlight) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  app.openFile(fixture("medium.tachyon"));
  app.openFile(fixture("tiny.tachyon"));
  ASSERT_TRUE(frameUntil(app, [&] { return !app.loading() && app.jobs().activeCount() == 0; }));
  ASSERT_TRUE(app.document());
  EXPECT_EQ(app.document()->path().filename(), "tiny.tachyon");
}

TEST(ViewerApp, SelfTestPrintsSummary) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  std::ostringstream out;
  EXPECT_EQ(app.runSelfTest(fixture("tiny.tachyon"), 100, out), 0);
  EXPECT_NE(out.str().find("selftest: OK"), std::string::npos) << out.str();
  EXPECT_GE(app.frameCount(), 100u);
}

TEST(ViewerApp, SelfTestFailsCleanlyOnMissingFile) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  std::ostringstream out;
  EXPECT_NE(app.runSelfTest(fixture("does-not-exist.tachyon"), 100, out), 0);
  EXPECT_NE(out.str().find("selftest: FAILED"), std::string::npos) << out.str();
}

TEST(ViewerApp, QuitCommandStopsTheLoop) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  EXPECT_TRUE(app.commands().run("app.quit"));
  EXPECT_FALSE(app.frame(0));
  EXPECT_EQ(app.run(), 0);
}

TEST(ViewerApp, PanelNamesAreUnique) {
  App app(makeNullRenderer(), headlessOptions());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  EXPECT_TRUE(app.panels().add(std::make_unique<RecordingPanel>()));
  EXPECT_FALSE(app.panels().add(std::make_unique<RecordingPanel>()));
  EXPECT_NE(app.panels().find("Trace Info"), nullptr);
}
