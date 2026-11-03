// WP-67 item 4 (WP-64 follow-ups): (f) a preview published before the load was cancelled is not shown;
// (g) when the complete document replaces a preview that numbered the threads differently, the selection and
// the timeline's row flags follow the threads by pid/tid instead of keeping stale indices.
#include "trace/trace_writer.h"
#include "viewer/app.h"
#include "viewer/selection_bus.h"
#include "viewer/timeline/timeline_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;
namespace fs = std::filesystem;

namespace {

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
  explicit Harness(std::shared_ptr<Gate> g, std::vector<double> stages = {0.5})
      : gate(std::move(g)), app(makeNullRenderer(), options(gate, std::move(stages))) {
    std::string error;
    EXPECT_TRUE(app.init(error)) << error;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    timeline =
        dynamic_cast<ThreadsTimelinePanel*>(app.panels().find(ThreadsTimelinePanel::kName)->panel.get());
    for (PanelEntry& e : app.panels().entries()) e.open = e.panel.get() == timeline;
  }
  ~Harness() { gate->open(); }
  static AppOptions options(const std::shared_ptr<Gate>& gate, std::vector<double> stages) {
    AppOptions o;
    o.persist = false;
    o.pacing = false;
    o.autoConnectAgent = false;
    o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_polish_stream_test_no_such_settings.json";
    o.streamMinFileBytes = 0;
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

class TempFile {
 public:
  explicit TempFile(const std::string& name) {
#if defined(_WIN32)
    const long pid = static_cast<long>(_getpid());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    m_dir = fs::path(TACHYON_BINARY_DIR) / "viewer_polish_tmp" / (name + "_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    fs::create_directories(m_dir, ec);
  }
  ~TempFile() {
    std::error_code ec;
    fs::remove_all(m_dir, ec);
  }
  fs::path path() const { return m_dir / "t.tachyon"; }

 private:
  fs::path m_dir;
};

constexpr uint32_t kPid = 77;
constexpr uint32_t kMainTid = 1234;  // has a ThreadRec
constexpr uint32_t kEarlyTid = 10;   // no ThreadRec; in the first sample chunks
constexpr uint32_t kLateTid = 5;     // no ThreadRec; only in the last sample chunks (sorts before kEarlyTid)

// Four sample chunks. Threads without a ThreadRec are numbered in ascending tid order after the recorded
// ones: a preview of the first two chunks knows {1234, 10}; the complete trace {1234, 5, 10}, so tid 10 moves
// from index 1 to index 2.
void writeRenumberingTrace(const fs::path& path) {
  using namespace trace;
  FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = 1000;
  h.end_ts = 1000 + 4 * 1000;
  h.platform = kPlatformLinux;
  auto w = TraceWriter::open(path, h);
  ASSERT_TRUE(w.ok());
  ProcessRec proc{};
  proc.pid = kPid;
  proc.start_ts = h.start_ts;
  proc.end_ts = h.end_ts;
  proc.name = w->internString("app");
  w->append(std::span<const ProcessRec>(&proc, 1));
  ThreadRec th{};
  th.tid = kMainTid;
  th.pid = kPid;
  th.start_ts = h.start_ts;
  th.end_ts = h.end_ts;
  th.name = w->internString("main");
  th.flags = kThreadFlagTargetProcess;
  w->append(std::span<const ThreadRec>(&th, 1));
  const FrameIdx f = w->internFrame(ModuleIdx(), 0x1000);
  const FrameIdx stack[1] = {f};
  const CallstackIdx leaf = w->internCallstack(std::span<const FrameIdx>(stack, 1));
  for (int chunk = 0; chunk < 4; ++chunk) {
    SampleBatch batch;
    const uint32_t other = chunk < 2 ? kEarlyTid : kLateTid;
    for (uint64_t i = 0; i < 20; ++i) {
      const uint64_t ts = h.start_ts + static_cast<uint64_t>(chunk) * 1000 + i * 10;
      batch.push(ts, i % 2 == 0 ? kMainTid : other, leaf, 0);
    }
    w->append(batch);
  }
  ASSERT_TRUE(w->finish().ok());
}

}  // namespace

TEST(PolishStream, APreviewPublishedBeforeACancelIsNotShown) {
  auto gate = std::make_shared<Gate>();
  Harness h(gate);
  h.app.openFile(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  // Wait (without pumping frames) until the loading thread has published the preview and sits at the gate.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (gate->reached() == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  ASSERT_GE(gate->reached(), 1);
  ASSERT_EQ(h.app.document(), nullptr) << "no frame ran: the preview is still in the mailbox";
  const auto jobs = h.app.jobs().active();
  ASSERT_EQ(jobs.size(), 1u);
  h.app.jobs().cancel(jobs[0].id);  // the user cancels the load (status bar) before the preview is taken
  h.frames(5);
  EXPECT_EQ(h.app.document(), nullptr) << "the abandoned load must not show its preview";
  gate->open();
  ASSERT_TRUE(h.until([&] { return !h.app.loading(); }));
  h.frames(5);
  EXPECT_EQ(h.app.document(), nullptr);
}

TEST(PolishStream, ThePreviewIsStillShownWithoutACancel) {
  auto gate = std::make_shared<Gate>();
  Harness h(gate);
  h.app.openFile(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr; }));
  EXPECT_TRUE(h.app.document()->isPreview());
}

TEST(PolishStream, TheSelectionFollowsItsThreadsWhenTheCompleteModelNumbersThemDifferently) {
  TempFile file("renumber");
  writeRenumberingTrace(file.path());
  auto gate = std::make_shared<Gate>();
  Harness h(gate, {0.5});
  h.app.openFile(file.path());
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr; }));
  ASSERT_TRUE(h.app.loading());
  const std::shared_ptr<Document> preview = h.app.document();
  ASSERT_TRUE(preview->isPreview());
  ASSERT_EQ(preview->model().threads.size(), 2u);
  const ThreadIdx previewEarly = preview->model().tidToThread.at(kEarlyTid);
  const ThreadIdx previewMain = preview->model().tidToThread.at(kMainTid);
  ASSERT_EQ(previewMain, ThreadIdx(0));
  ASSERT_EQ(previewEarly, ThreadIdx(1));

  // The user's place in the preview: a selected thread, a thread filter, a hovered thread, an expanded row.
  ViewFilter filter;
  filter.t0 = preview->model().header.start_ts;
  filter.t1 = preview->model().header.end_ts;
  filter.threads = std::vector<ThreadIdx>{previewEarly, previewMain};
  h.app.selection().setTimeFilter(filter);
  h.app.selection().setSelectedThread(previewEarly);
  HoveredItem hover;
  hover.kind = HoveredItem::Kind::Thread;
  hover.thread = previewEarly;
  h.app.selection().setHovered(hover);
  h.app.selection().setSelectedWait(WaitSelection{previewEarly, 1234});
  ASSERT_TRUE(h.until([&] { return h.timeline->widget().lodFinished(); }));
  h.timeline->widget().setExpanded(previewEarly, true);
  h.frames(3);
  ASSERT_TRUE(h.timeline->widget().flagsOf(previewEarly).expanded);

  gate->open();
  ASSERT_TRUE(h.until([&] { return h.app.document() && !h.app.document()->isPreview(); }));
  const std::shared_ptr<Document> full = h.app.document();
  ASSERT_EQ(full->model().threads.size(), 3u);
  const ThreadIdx fullEarly = full->model().tidToThread.at(kEarlyTid);
  const ThreadIdx fullMain = full->model().tidToThread.at(kMainTid);
  const ThreadIdx fullLate = full->model().tidToThread.at(kLateTid);
  ASSERT_EQ(fullMain, ThreadIdx(0));
  ASSERT_NE(fullEarly, previewEarly) << "the scenario: the same thread has another index now";
  ASSERT_EQ(fullLate, previewEarly) << "...and its old index belongs to another thread";

  const SelectionState& s = h.app.selection().state();
  EXPECT_EQ(s.selectedThread, fullEarly);
  ASSERT_TRUE(s.timeFilter && s.timeFilter->threads);
  EXPECT_EQ(*s.timeFilter->threads, (std::vector<ThreadIdx>{fullEarly, fullMain}));
  EXPECT_EQ(s.hovered.thread, fullEarly);
  ASSERT_TRUE(s.selectedWait);
  EXPECT_EQ(s.selectedWait->thread, fullEarly);

  // The timeline's row flags follow the thread too.
  h.frames(3);
  ASSERT_TRUE(h.until([&] { return h.timeline->widget().lodFinished(); }));
  EXPECT_TRUE(h.timeline->widget().flagsOf(fullEarly).expanded);
  EXPECT_FALSE(h.timeline->widget().flagsOf(fullLate).expanded);
}

TEST(PolishStream, ASelectionOnAThreadThatIsGoneIsDroppedNotKeptStale) {
  // The map has no entry for the preview's thread 1: its selection entries go, the others are kept.
  SelectionBus bus;
  ViewFilter f;
  f.t0 = 0;
  f.t1 = 100;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(1), ThreadIdx(0)};
  bus.setTimeFilter(f);
  bus.setSelectedThread(ThreadIdx(1));
  WaitSelection wait{ThreadIdx(1), 5};
  bus.setSelectedWait(wait);
  int published = 0;
  bus.subscribe([&](const SelectionState&, SelectionField) { ++published; });
  bus.remapThreads([](ThreadIdx t) { return t == ThreadIdx(0) ? ThreadIdx(0) : ThreadIdx(); });
  const SelectionState& s = bus.state();
  EXPECT_FALSE(s.selectedThread.valid());
  ASSERT_TRUE(s.timeFilter && s.timeFilter->threads);
  EXPECT_EQ(*s.timeFilter->threads, (std::vector<ThreadIdx>{ThreadIdx(0)}));
  EXPECT_FALSE(s.selectedWait);
  EXPECT_EQ(published, 1);

  // Nothing left on the list: the thread restriction goes instead of selecting no thread at all.
  bus.remapThreads([](ThreadIdx) { return ThreadIdx(); });
  ASSERT_TRUE(bus.state().timeFilter);
  EXPECT_FALSE(bus.state().timeFilter->threads);
}

TEST(PolishStream, RemappingWithTheIdentityPublishesNothing) {
  SelectionBus bus;
  bus.setSelectedThread(ThreadIdx(3));
  int published = 0;
  bus.subscribe([&](const SelectionState&, SelectionField) { ++published; });
  bus.remapThreads([](ThreadIdx t) { return t; });
  EXPECT_EQ(published, 0);
  EXPECT_EQ(bus.state().selectedThread, ThreadIdx(3));
}

TEST(PolishStream, ThreadMapBetweenModelsIsByPidAndTid) {
  auto thread = [](uint32_t pid, uint32_t tid) {
    model::Thread t;
    t.pid = pid;
    t.tid = tid;
    return t;
  };
  model::TraceModel a, b;
  a.threads = {thread(1, 100), thread(1, 10)};
  b.threads = {thread(1, 100), thread(1, 5), thread(1, 10)};
  const auto map = threadMapBetween(a, b);
  ASSERT_TRUE(map);
  EXPECT_EQ(map(ThreadIdx(0)), ThreadIdx(0));
  EXPECT_EQ(map(ThreadIdx(1)), ThreadIdx(2));
  EXPECT_FALSE(map(ThreadIdx(7)).valid());
  // Identical tables: nothing to remap. A thread the new model lacks maps to invalid.
  EXPECT_FALSE(threadMapBetween(b, b));
  const auto gone = threadMapBetween(b, a);
  ASSERT_TRUE(gone);
  EXPECT_FALSE(gone(ThreadIdx(1)).valid());
  EXPECT_EQ(gone(ThreadIdx(2)), ThreadIdx(1));
  // The same tid in another process is another thread.
  model::TraceModel c;
  c.threads = {thread(2, 100)};
  const auto other = threadMapBetween(a, c);
  ASSERT_TRUE(other);
  EXPECT_FALSE(other(ThreadIdx(0)).valid());
}
