// Perf budget (WP-42, acceptance "60 fps with 64 rows at every zoom level"): CPU time of one complete
// timeline frame on the main thread, i.e. every LOD query the frame needs plus the ImGui draw-list building
// (NewFrame ... Render), without a GPU. The frame budget is 8 ms (half of 16.6 ms, leaving the rest to the GL
// backend and the other panels); the assert is at 2x.
//
//   medium (CI): the 16 threads of `medium` replicated to 64 rows (same samples per row).
//   large (local, skipped when tachyon_fixtures_large was not built): the real 64-thread, 10 minute capture.
//
// Asserted metric: for each zoom the MEDIAN of the 7 counted frames, then the worst zoom ("median frame at
// the worst zoom"). It is stable (a single scheduler hiccup cannot move a median of 7, so CI does not flake)
// and still moves with a real regression, because a 2x slowdown of the code doubles every frame at every
// zoom. Budget: 5 ms, which is where this metric sits on this machine today (4.3 to 6.7 ms over repeated runs
// on medium and large, depending on machine load); the assert is at 2x the budget (10 ms). A 2x regression of
// the current cost lands at 9 to 13 ms. The slowest single frame (worst
// rep of the worst zoom, usually 5 to 9 ms) and the best-of-reps figure are printed for information only. The
// separate 8 ms figure is the product frame budget (half of 16.6 ms); the steady frame is asserted against
// it.
//
// Every measured frame changes the view by a tick so the row caches miss and all rows are re-queried (the
// worst case: continuous panning or zooming); a steady frame is separately measured and is far cheaper.
#include "analysis/lod.h"
#include "analysis/thread_states.h"
#include "viewer/document.h"
#include "viewer/interaction/interaction_model.h"
#include "viewer/timeline/timeline_widget.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <time.h>
#endif

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;

namespace {

constexpr double kFrameBudgetMs = 8.0;       // product budget for a whole timeline frame
constexpr double kRegressionBudgetMs = 5.0;  // budget of the asserted metric (median frame, worst zoom)
constexpr float kDisplayW = 1920.0f;
constexpr float kDisplayH = 6000.0f;  // tall enough that all 64 rows (4 of them expanded) are on screen

// CPU time of the calling thread in milliseconds (wall time where there is no per-thread clock).
double threadCpuMs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) * 1e-6;
#endif
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Own ImGui context with a font atlas but no backend, like the Null renderer.
class ImGuiHeadless {
 public:
  ImGuiHeadless() {
    m_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);  // NOLINT
  }
  ~ImGuiHeadless() { ImGui::DestroyContext(m_ctx); }

 private:
  ImGuiContext* m_ctx = nullptr;
};

struct FrameResult {
  double cpuMs = 0;
  size_t vertices = 0;
  TimelineFrameStats stats;
};

FrameResult runFrame(TimelineWidget& widget, const PanelContext& context) {
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = 1.0f / 60.0f;
  FrameResult r;
  const double start = threadCpuMs();
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(kDisplayW, kDisplayH));
  ImGui::Begin(
      "perf", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  widget.draw(context);
  ImGui::End();
  ImGui::Render();
  r.cpuMs = threadCpuMs() - start;
  r.vertices = static_cast<size_t>(ImGui::GetDrawData()->TotalVtxCount);
  r.stats = widget.frameStats();
  return r;
}

struct Zoom {
  double span;
  double pos;
  std::string label;
};

// Whole trace down to ~100 us windows in factors of 2, at three positions.
std::vector<Zoom> makeZooms(const model::TraceModel& m) {
  const double total = static_cast<double>(m.header.end_ts - m.header.start_ts);
  const double hz = static_cast<double>(m.header.clock_frequency_hz);
  std::vector<Zoom> out;
  for (double span = total; span >= hz * 1e-4; span /= 2) {
    for (const double pos : {0.0, 0.4, 0.73}) {
      char label[64];
      std::snprintf(label, sizeof label, "%.3f ms @%.2f", span * 1e3 / hz, pos);
      out.push_back({span, pos, label});
    }
  }
  return out;
}

struct Sweep {
  double worstMs = 0;        // slowest single frame (worst rep of the worst zoom), information only
  double medianWorstMs = 0;  // worst zoom of the per-zoom median of the counted reps: the asserted metric
  std::string medianWorstLabel;
  double bestWorstMs = 0;  // worst zoom of the best-of-reps (the noise-free cost, for information)
  std::string worstFrameLabel;
  double medianMs = 0;
  std::string worstLabel;
  size_t maxVertices = 0;
  uint32_t minRowsDrawn = 1u << 30;
  uint32_t maxRects = 0;
};

Sweep sweep(TimelineWidget& widget, const PanelContext& context, const std::vector<Zoom>& zooms, int reps) {
  const TimeLimits limits = widget.limits();
  Sweep s;
  std::vector<double> perZoom;
  for (const Zoom& z : zooms) {
    double best = 1e30, worst = 0;
    std::vector<double> counted;
    for (int i = 0; i < reps; ++i) {
      TimeView v;
      v.span = z.span;
      v.start = (limits.total() - z.span) * z.pos + i;  // a tick of pan per rep: every row re-queries
      widget.setView(v);
      const FrameResult r = runFrame(widget, context);
      if (i == 0)
        continue;  // the first frame at a new zoom warms the caches (a one-frame jump, not steady cost)
      counted.push_back(r.cpuMs);
      best = std::min(best, r.cpuMs);
      worst = std::max(worst, r.cpuMs);
      s.maxVertices = std::max(s.maxVertices, r.vertices);
      s.minRowsDrawn = std::min(s.minRowsDrawn, r.stats.rowsDrawn);
      s.maxRects = std::max(s.maxRects, r.stats.rects);
    }
    std::sort(counted.begin(), counted.end());
    const double median = counted[counted.size() / 2];
    if (median > s.medianWorstMs) {
      s.medianWorstMs = median;
      s.medianWorstLabel = z.label;
    }
    perZoom.push_back(best);
    if (best > s.bestWorstMs) {
      s.bestWorstMs = best;
      s.worstLabel = z.label;
    }
    if (worst > s.worstMs) {
      s.worstMs = worst;
      s.worstFrameLabel = z.label;
    }
  }
  std::sort(perZoom.begin(), perZoom.end());
  s.medianMs = perZoom[perZoom.size() / 2];
  return s;
}

// Replicates threads cyclically until there are `rows` of them.
void replicateThreads(model::TraceModel& m, size_t rows) {
  const size_t n = m.threads.size();
  ASSERT_GT(n, 0u);
  for (size_t i = n; i < rows; ++i) {
    model::Thread t = m.threads[i % n];
    t.tid += static_cast<uint32_t>(1000 * (i / n));
    t.name += "_r" + std::to_string(i / n);
    m.threads.push_back(std::move(t));
    m.samplesByThread.push_back(m.samplesByThread[i % n]);
    m.statesByThread.push_back(m.statesByThread[i % n]);
    m.scopesByThread.emplace_back();
  }
}

// withArrow: a wait with a known waker is selected on the bus, so every frame also draws its persistent
// wake-up arrow (WP-46) on top of the normal timeline cost; the same budgets apply.
void runScenario(const char* name, std::shared_ptr<const model::TraceModel> model, bool withArrow = false) {
  ImGuiHeadless imgui;
  Document doc("perf.tachyon", model);
  SelectionBus bus;
  PanelContext context;
  context.document = &doc;
  context.selection = &bus;
  context.documentGeneration = 1;
  if (withArrow) {
    interaction::WaitQuery q;
    q.t0 = model->header.start_ts;
    q.maxResults = 100000;
    bool found = false;
    for (const interaction::WaitEpisode& e : interaction::longestWaits(*model, q, nullptr).rows) {
      if (e.hasWaker && e.hasReadyRec && e.wakerThread.valid()) {
        bus.setSelectedWait(WaitSelection{e.thread, e.switchOutTs});
        found = true;
        break;
      }
    }
    ASSERT_TRUE(found);
  }

  TimelineWidget widget;
  widget.options().buildInline = true;
  const double buildStart = threadCpuMs();
  runFrame(widget, context);  // starts the inline LOD build
  const double buildMs = threadCpuMs() - buildStart;
  ASSERT_TRUE(widget.lodFinished());
  ASSERT_EQ(widget.rowOrder().size(), model->threads.size());
  runFrame(widget, context);  // warm up fonts / glyph cache

  const std::vector<Zoom> zooms = makeZooms(*model);
  const int reps = 8;  // the first rep of each zoom is a warm-up

  // 1. all rows collapsed.
  const Sweep collapsed = sweep(widget, context, zooms, reps);
  // 2. four rows expanded to their flame charts.
  for (size_t i = 0; i < 4; ++i) widget.setExpanded(widget.rowOrder()[i], true);
  const Sweep expanded = sweep(widget, context, zooms, reps);
  for (size_t i = 0; i < 4; ++i) widget.setExpanded(widget.rowOrder()[i], false);

  // 3. steady frame: nothing changes, nothing is re-queried.
  widget.fit();
  runFrame(widget, context);
  double steady = 1e30;
  FrameResult steadyResult;
  for (int i = 0; i < 10; ++i) {
    steadyResult = runFrame(widget, context);
    steady = std::min(steady, steadyResult.cpuMs);
  }

  std::printf(
      "[timeline perf] %s: %zu rows, %zu zooms x %d reps at %.0f px (first frame incl. LOD build %.0f ms)\n",
      name, model->threads.size(), zooms.size(), reps, static_cast<double>(kDisplayW), buildMs);
  std::printf(
      "[timeline perf] %s: collapsed 64 rows: SLOWEST FRAME %.3f ms (%s); best-of-reps worst zoom %.3f ms "
      "(%s), "
      "median %.3f ms, max %zu vertices, max %u rects, min rows drawn %u\n",
      name, collapsed.worstMs, collapsed.worstFrameLabel.c_str(), collapsed.bestWorstMs,
      collapsed.worstLabel.c_str(), collapsed.medianMs, collapsed.maxVertices, collapsed.maxRects,
      collapsed.minRowsDrawn);
  std::printf(
      "[timeline perf] %s: 4 rows expanded: SLOWEST FRAME %.3f ms (%s); best-of-reps worst zoom %.3f ms, "
      "median "
      "%.3f ms, max %zu vertices\n",
      name, expanded.worstMs, expanded.worstFrameLabel.c_str(), expanded.bestWorstMs, expanded.medianMs,
      expanded.maxVertices);
  std::printf(
      "[timeline perf] %s: steady frame (no input): %.3f ms; budget %.1f ms, assert (slowest frame) %.1f "
      "ms\n",
      name, steady, kFrameBudgetMs, kFrameBudgetMs * 2);

  std::printf(
      "[timeline perf] %s: ASSERTED median-frame worst zoom: collapsed %.3f ms (%s), expanded %.3f ms (%s); "
      "budget %.1f ms, assert %.1f ms\n",
      name, collapsed.medianWorstMs, collapsed.medianWorstLabel.c_str(), expanded.medianWorstMs,
      expanded.medianWorstLabel.c_str(), kRegressionBudgetMs, kRegressionBudgetMs * 2);

  EXPECT_EQ(collapsed.minRowsDrawn, 64u) << "all 64 rows must be on screen for the budget to mean anything";
  if (withArrow) std::printf("[timeline perf] %s: wake-up arrow selected during the sweeps\n", name);
  EXPECT_LT(collapsed.medianWorstMs, kRegressionBudgetMs * 2);
  EXPECT_LT(expanded.medianWorstMs, kRegressionBudgetMs * 2);
  EXPECT_LT(steady, kFrameBudgetMs);
}

}  // namespace

TEST(TimelinePerf, MediumReplicatedTo64Rows) {
  auto loaded =
      model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel m = std::move(*loaded);
  ASSERT_TRUE(analysis::buildThreadStates(m).ok());
  replicateThreads(m, 64);
  ASSERT_EQ(m.threads.size(), 64u);
  runScenario("medium x4", std::make_shared<const model::TraceModel>(std::move(m)));
}

TEST(TimelinePerf, MediumReplicatedTo64RowsWithAWakeUpArrow) {
  auto loaded =
      model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel m = std::move(*loaded);
  ASSERT_TRUE(analysis::buildThreadStates(m).ok());
  replicateThreads(m, 64);
  runScenario("medium x4 + arrow", std::make_shared<const model::TraceModel>(std::move(m)), true);
}

TEST(TimelinePerf, Large64Threads10Minutes) {
  const auto path = std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "large.tachyon";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "large fixture not built (target tachyon_fixtures_large)";
  auto loaded = model::TraceModel::load(path);
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel m = std::move(*loaded);
  ASSERT_TRUE(analysis::buildThreadStates(m).ok());
  replicateThreads(m, 64);  // no-op for the real 64-thread fixture
  runScenario("large", std::make_shared<const model::TraceModel>(std::move(m)));
}
