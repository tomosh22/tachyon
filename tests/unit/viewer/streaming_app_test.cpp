// WP-64: the viewer opens big traces in stages. A preview document is shown while the rest loads, the
// complete document replaces it with the user's place kept, closing the preview abandons the load, and a
// newer open supersedes a load in flight. The loading thread is held at a gate while the test looks at the
// preview.
#include "viewer/app.h"
#include "viewer/timeline/timeline_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

// Blocks the loading thread after each preview until the test lets it go (or the test is over).
class Gate {
 public:
  void block() {
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_reached;
    m_cv.wait_for(lock, std::chrono::seconds(60), [&] { return m_open; });
  }
  void open() {
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      m_open = true;
    }
    m_cv.notify_all();
  }
  int reached() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_reached;
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_open = false;
  int m_reached = 0;
};

class Harness {
 public:
  explicit Harness(std::shared_ptr<Gate> g, std::vector<double> stages = {0.3})
      : gate(std::move(g)), app(makeNullRenderer(), options(gate, std::move(stages))) {
    std::string error;
    EXPECT_TRUE(app.init(error)) << error;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    timeline =
        dynamic_cast<ThreadsTimelinePanel*>(app.panels().find(ThreadsTimelinePanel::kName)->panel.get());
    for (PanelEntry& e : app.panels().entries()) e.open = e.panel.get() == timeline;
  }
  ~Harness() {
    gate->open();  // never leave the loading thread blocked
  }
  static AppOptions options(const std::shared_ptr<Gate>& gate, std::vector<double> stages) {
    AppOptions o;
    o.persist = false;
    o.pacing = false;
    o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_streaming_test_no_such_settings.json";
    o.streamMinFileBytes = 0;  // stream even the small fixtures
    o.streamStages = std::move(stages);
    o.streamGate = [gate] { gate->block(); };
    return o;
  }
  bool until(const std::function<bool()>& pred, int seconds = 60) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  void frames(int n) {
    for (int i = 0; i < n; ++i) app.frame(0);
  }

  std::shared_ptr<Gate> gate;
  App app;
  ThreadsTimelinePanel* timeline = nullptr;
};

TEST(StreamingApp, ThePreviewIsShownFirstAndTheCompleteDocumentReplacesItKeepingThePlace) {
  auto gate = std::make_shared<Gate>();
  Harness h(gate);
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr; }));
  ASSERT_TRUE(h.app.loading()) << "the preview appears while the load goes on";
  const std::shared_ptr<Document> preview = h.app.document();
  EXPECT_TRUE(preview->isPreview());
  EXPECT_NE(preview->streamId(), 0u);
  EXPECT_GT(preview->model().loadStatus.fullEndTs, preview->model().header.end_ts);
  const uint64_t previewGeneration = h.app.documentGeneration();

  // The preview is fully usable: the timeline builds its index and the user navigates.
  ASSERT_TRUE(h.until([&] { return h.timeline->widget().lodFinished(); }));
  h.frames(3);
  const TimeLimits previewLimits = h.timeline->widget().limits();
  EXPECT_EQ(previewLimits.traceEnd, preview->model().header.end_ts);
  TimeView view = h.timeline->widget().view();
  view.span = view.span / 4.0;
  view.start = view.span;  // somewhere inside the preview
  h.timeline->widget().setView(view);
  const TimeView set = h.timeline->widget().view();
  h.timeline->widget().setExpanded(ThreadIdx(1), true);
  h.app.selection().setSelectedThread(ThreadIdx(2));
  h.frames(2);

  gate->open();
  ASSERT_TRUE(h.until([&] { return !h.app.loading(); }));
  ASSERT_TRUE(h.until([&] { return h.app.document() && !h.app.document()->isPreview(); }));
  const std::shared_ptr<Document> full = h.app.document();
  EXPECT_NE(full.get(), preview.get());
  EXPECT_EQ(full->streamId(), preview->streamId());
  EXPECT_GT(h.app.documentGeneration(), previewGeneration);
  EXPECT_TRUE(full->model().loadStatus.complete);
  EXPECT_GT(full->totalSamples(), preview->totalSamples());

  // The user's place carried over: same view, same expanded row, same selection.
  h.frames(3);
  ASSERT_TRUE(h.until([&] { return h.timeline->widget().lodFinished(); }));
  EXPECT_EQ(h.timeline->widget().limits().traceEnd, full->model().header.end_ts);
  EXPECT_DOUBLE_EQ(h.timeline->widget().view().start, set.start);
  EXPECT_DOUBLE_EQ(h.timeline->widget().view().span, set.span);
  EXPECT_TRUE(h.timeline->widget().flagsOf(ThreadIdx(1)).expanded);
  EXPECT_EQ(h.app.selection().state().selectedThread, ThreadIdx(2));
}

// WP-68: the complete document's symbol run adopts what the preview's run finished (same module table):
// nothing is located or loaded again, and both services index one string table.
TEST(StreamingApp, TheCompleteDocumentAdoptsThePreviewsFinishedSymbols) {
  auto gate = std::make_shared<Gate>();
  Harness h(gate);
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr && h.app.document()->symbols() != nullptr; }));
  const std::shared_ptr<Document> preview = h.app.document();
  ASSERT_TRUE(preview->isPreview());
  const std::shared_ptr<symbols::SymbolService> previewSymbols = preview->symbols();
  ASSERT_TRUE(h.until([&] {
    const symbols::SymbolProgress p = previewSymbols->progress();
    return p.total > 0 && p.done == p.total;
  }));
  const uint32_t modules = previewSymbols->progress().total;
  EXPECT_GT(previewSymbols->modulesLoaded(), 0u);
  EXPECT_EQ(previewSymbols->modulesAdopted(), 0u);

  gate->open();
  ASSERT_TRUE(h.until([&] { return h.app.document() && !h.app.document()->isPreview(); }));
  const std::shared_ptr<Document> full = h.app.document();
  ASSERT_TRUE(h.until([&] { return full->symbols() != nullptr; }));
  const std::shared_ptr<symbols::SymbolService> fullSymbols = full->symbols();
  EXPECT_NE(fullSymbols.get(), previewSymbols.get());
  ASSERT_TRUE(h.until([&] {
    const symbols::SymbolProgress p = fullSymbols->progress();
    return p.total > 0 && p.done == p.total;
  }));
  EXPECT_EQ(fullSymbols->progress().total, modules);
  EXPECT_EQ(fullSymbols->modulesLoaded(), 0u) << "no module is located or loaded again";
  EXPECT_EQ(fullSymbols->modulesAdopted(), modules);
  EXPECT_EQ(&fullSymbols->symbolStrings(), &previewSymbols->symbolStrings());
  // The frames of the complete model resolve through the adopted providers.
  size_t resolved = 0;
  for (uint32_t f = 0; f < full->model().frames.size(); ++f)
    resolved += fullSymbols->lookup(FrameIdx(f)).resolved;
  EXPECT_GT(resolved, 0u);
}

TEST(StreamingApp, AViewThatShowedTheWholePreviewShowsTheWholeTraceOnceItIsComplete) {
  auto gate = std::make_shared<Gate>();
  Harness h(gate);
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr && h.timeline->widget().hasDocument(); }));
  h.frames(2);
  EXPECT_EQ(h.timeline->widget().view().span, h.timeline->widget().limits().total());
  gate->open();
  ASSERT_TRUE(h.until([&] { return h.app.document() && !h.app.document()->isPreview(); }));
  h.frames(3);
  EXPECT_DOUBLE_EQ(h.timeline->widget().view().start, 0.0);
  EXPECT_DOUBLE_EQ(h.timeline->widget().view().span, h.timeline->widget().limits().total());
  EXPECT_EQ(h.timeline->widget().limits().traceEnd, h.app.document()->model().header.end_ts);
}

TEST(StreamingApp, ClosingThePreviewAbandonsTheLoad) {
  auto gate = std::make_shared<Gate>();
  Harness h(gate);
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr; }));
  ASSERT_TRUE(h.app.loading());
  h.app.commands().run("file.close");
  h.frames(2);
  EXPECT_EQ(h.app.document(), nullptr);
  gate->open();
  ASSERT_TRUE(h.until([&] { return !h.app.loading(); }));
  h.frames(5);
  EXPECT_EQ(h.app.document(), nullptr) << "the cancelled load must not open the complete document";
  EXPECT_TRUE(h.app.lastError().empty());
}

TEST(StreamingApp, ANewOpenSupersedesALoadInFlight) {
  auto gate = std::make_shared<Gate>();
  Harness h(gate);
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr; }));
  const uint64_t firstStream = h.app.document()->streamId();
  h.app.openFile(fixture("tiny.tachyon"));  // a single Samples chunk: loaded in one go (no preview)
  gate->open();
  ASSERT_TRUE(h.until([&] { return !h.app.loading(); }));
  ASSERT_NE(h.app.document(), nullptr);
  EXPECT_EQ(h.app.document()->path().filename(), "tiny.tachyon");
  EXPECT_FALSE(h.app.document()->isPreview());
  EXPECT_NE(h.app.document()->streamId(), firstStream);
  h.frames(5);
  EXPECT_EQ(h.app.document()->path().filename(), "tiny.tachyon") << "the cancelled stream must not come back";
}

TEST(StreamingApp, DestroyingTheAppWhileAPreviewIsHeldAtTheGateIsSafe) {
  auto gate = std::make_shared<Gate>();
  {
    Harness h(gate);
    h.app.openFile(fixture("medium.tachyon"));
    ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr; }));
    ASSERT_TRUE(h.app.loading());
    // Leaves the scope with the loading job still blocked: the Harness opens the gate, the job sees its
    // cancellation and the JobQueue joins it.
  }
  SUCCEED();
}

TEST(StreamingApp, SmallFilesAndDisabledStreamingLoadInOneGo) {
  {
    auto gate = std::make_shared<Gate>();
    AppOptions o = Harness::options(gate, {0.3});
    o.streamMinFileBytes = 1ull << 40;  // nothing is big enough
    App app(makeNullRenderer(), o);
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    app.openFile(fixture("medium.tachyon"));
    int previews = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (app.loading() || !app.document()) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      app.frame(0);
      if (app.document() && app.document()->isPreview()) ++previews;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(previews, 0);
    EXPECT_EQ(gate->reached(), 0);
    EXPECT_FALSE(app.document()->isPreview());
  }
}

}  // namespace
