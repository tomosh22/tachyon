// Perf budgets (WP-54): the cost of the instrumentation additions to a timeline frame, measured like
// timeline_perf_test.cpp (CPU time of one complete frame on the main thread: every LOD query the frame needs,
// the lanes and the ImGui draw-list building, without a GPU), at 64 rows:
//   * scope lanes on a synthetic trace with 1 M scopes (4 depths, most of them sub-pixel at the full view),
//   on
//     the `scopes` and `fibers` fixtures replicated to 64 rows (fibers: grouped lanes);
//   * the frame-time strip with 100 k frames;
//   * a plot lane with 10 M samples and a markers lane with 100 k markers.
// Asserted metric as in the timeline test: the MEDIAN of the counted reps at each zoom, then the worst zoom;
// the assert is at 2x the budget. Off-thread builds (lane tracks, the plot index) have their own budgets. All
// numbers are printed.
#include "analysis/fibers.h"
#include "analysis/scope_pairing.h"
#include "analysis/thread_states.h"
#include "viewer/document.h"
#include "viewer/timeline/frame_graph.h"
#include "viewer/timeline/plot_lanes.h"
#include "viewer/timeline/scope_lanes.h"
#include "viewer/timeline/timeline_widget.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <time.h>
#endif

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;

namespace {

// Budgets in milliseconds: where the metric sits today with some headroom for slower CI machines; the assert
// is at twice the budget (Part 2.4).
// Today on the development machine (4 to 9 ms depending on load): scopes 6.0 / 6.3 ms (1 M scopes, collapsed
// / 4 rows expanded), 3.9 ms (fixtures), strip 5.1 ms, plot + markers 7.4 to 8.4 ms (of which ~4.3 ms is the
// 64-row timeline itself, as in timeline_perf_test).
constexpr double kProductFrameMs = 8.0;        // a steady frame (no input) must stay under the product budget
constexpr double kScopeFrameBudgetMs = 6.0;    // 64 rows with scope lanes, median frame at the worst zoom
constexpr double kStripFrameBudgetMs = 5.5;    // + a 100 k frame strip
constexpr double kPlotFrameBudgetMs = 6.5;     // + a 10 M sample plot lane and 100 k markers
constexpr double kLaneBuildBudgetMs = 1500.0;  // lane tracks of 1 M scopes (off the UI thread)
constexpr double kPlotIndexBudgetMs = 600.0;   // index of 10 M samples (off the UI thread)
constexpr float kDisplayW = 1920.0f;
constexpr float kDisplayH = 6000.0f;

double threadCpuMs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) * 1e-6;
#endif
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double wallMs(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

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
  double span, pos;
  std::string label;
};

std::vector<Zoom> makeZooms(const model::TraceModel& m) {
  const double total = static_cast<double>(m.header.end_ts - m.header.start_ts);
  const double hz = static_cast<double>(m.header.clock_frequency_hz);
  std::vector<Zoom> out;
  for (double span = total; span >= hz * 2e-4; span /= 2.5) {
    for (const double pos : {0.0, 0.4, 0.73}) {
      char label[64];
      std::snprintf(label, sizeof label, "%.3f ms @%.2f", span * 1e3 / hz, pos);
      out.push_back({span, pos, label});
    }
  }
  return out;
}

struct Sweep {
  double medianWorstMs = 0, worstMs = 0, steadyMs = 0;
  std::string medianWorstLabel;
  uint32_t minRowsDrawn = 1u << 30;
  uint32_t maxScopeItems = 0, maxFrameBars = 0, maxPlotColumns = 0, maxMarkerTicks = 0;
  size_t maxVertices = 0;
};

Sweep sweep(TimelineWidget& widget, const PanelContext& context, const std::vector<Zoom>& zooms, int reps) {
  const TimeLimits limits = widget.limits();
  Sweep s;
  for (const Zoom& z : zooms) {
    std::vector<double> counted;
    for (int i = 0; i < reps; ++i) {
      TimeView v;
      v.span = z.span;
      v.start = (limits.total() - z.span) * z.pos + i;
      widget.setView(v);
      const FrameResult r = runFrame(widget, context);
      if (i == 0) continue;  // warm-up of the new zoom
      counted.push_back(r.cpuMs);
      s.worstMs = std::max(s.worstMs, r.cpuMs);
      s.maxVertices = std::max(s.maxVertices, r.vertices);
      s.minRowsDrawn = std::min(s.minRowsDrawn, r.stats.rowsDrawn);
      s.maxScopeItems = std::max(s.maxScopeItems, r.stats.scopeItems);
      s.maxFrameBars = std::max(s.maxFrameBars, r.stats.frameBars);
      s.maxPlotColumns = std::max(s.maxPlotColumns, r.stats.plotColumns);
      s.maxMarkerTicks = std::max(s.maxMarkerTicks, r.stats.markerTicks);
    }
    std::sort(counted.begin(), counted.end());
    const double median = counted[counted.size() / 2];
    if (median > s.medianWorstMs) {
      s.medianWorstMs = median;
      s.medianWorstLabel = z.label;
    }
  }
  widget.fit();
  runFrame(widget, context);
  s.steadyMs = 1e30;
  for (int i = 0; i < 10; ++i) s.steadyMs = std::min(s.steadyMs, runFrame(widget, context).cpuMs);
  return s;
}

void replicateThreads(model::TraceModel& m, size_t rows) {
  const size_t n = m.threads.size();
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

// A fixture on 64 rows with thread states: the sampled part of every frame.
std::shared_ptr<model::TraceModel> loadBase(const char* fixture) {
  auto loaded = model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / fixture);
  EXPECT_TRUE(loaded.ok());
  auto m = std::make_shared<model::TraceModel>(std::move(*loaded));
  EXPECT_TRUE(analysis::buildThreadStates(*m).ok());
  replicateThreads(*m, 64);
  return m;
}

// Scopes on every thread: roots with nested scopes (four depths, five scopes per root).
std::shared_ptr<analysis::PairedScopes> syntheticScopes(model::TraceModel& m, size_t perThread) {
  auto paired = std::make_shared<analysis::PairedScopes>();
  paired->byThread.resize(m.threads.size());
  StringIdx ids[6];
  const char* names[] = {"Frame", "Update", "Physics", "Solve", "Render", "Job"};
  for (int i = 0; i < 6; ++i) ids[i] = m.strings.intern(names[i]);
  const StringIdx data = m.strings.intern("level 3");
  const uint64_t t0 = m.header.start_ts, total = m.header.end_ts - m.header.start_ts;
  const size_t roots = perThread / 5;
  const uint64_t period = total / roots;
  std::mt19937_64 rng(77);
  for (size_t t = 0; t < m.threads.size(); ++t) {
    auto& out = paired->byThread[t];
    out.reserve(perThread);
    const uint64_t phase = rng() % (period / 4 + 1);
    for (size_t r = 0; r < roots; ++r) {
      const uint64_t b = t0 + phase + r * period;
      const uint64_t len = period * 3 / 4;
      const auto add = [&](int id, uint64_t begin, uint64_t end, uint32_t depth, uint32_t color) {
        analysis::ScopeInstance s;
        s.id = ids[id];
        s.begin = begin;
        s.end = end;
        s.depth = depth;
        s.color = color;
        if (id == 1) s.data = data;
        out.push_back(s);
      };
      add(0, b, b + len, 0, 0xFFFFFFFFu);
      add(1, b + len / 20, b + len / 2, 1, 0xFFFFFFFFu);
      add(2, b + len / 10, b + len / 3, 2, 0x40A0FFFFu);
      add(3, b + len / 8, b + len / 4, 3, 0xFFFFFFFFu);
      add(4, b + len / 2 + 1, b + len * 9 / 10, 1, r % 97 == 0 ? analysis::kLockColorSentinel : 0xFFFFFFFFu);
    }
  }
  return paired;
}

std::shared_ptr<analysis::PairedScopes> replicatePaired(const analysis::PairedScopes& src, size_t rows) {
  auto out = std::make_shared<analysis::PairedScopes>();
  const size_t n = src.byThread.size();
  out->byThread.resize(rows);
  for (size_t i = 0; i < rows; ++i) out->byThread[i] = src.byThread[i % n];
  return out;
}

void printSweep(const char* name, const char* what, const Sweep& s, double budget) {
  std::printf(
      "[inst perf] %s (%s): median frame at the worst zoom %.3f ms (%s), slowest frame %.3f ms, steady %.3f "
      "ms; "
      "budget %.1f ms, assert %.1f ms; max %u scope items, %u frame bars, %u plot columns, %u marker ticks, "
      "%zu vertices\n",
      name, what, s.medianWorstMs, s.medianWorstLabel.c_str(), s.worstMs, s.steadyMs, budget, budget * 2,
      s.maxScopeItems, s.maxFrameBars, s.maxPlotColumns, s.maxMarkerTicks, s.maxVertices);
}

struct Scenario {
  const char* name;
  std::shared_ptr<model::TraceModel> model;
  std::function<void(TimelineWidget&)> inject;  // after the first frame (the document exists then)
  double budgetMs;
  uint32_t minScopeItems = 0, minFrameBars = 0, minPlotColumns = 0;
};

void run(const Scenario& sc) {
  ImGuiHeadless imgui;
  std::shared_ptr<const model::TraceModel> model = sc.model;
  Document doc("perf.tachyon", model);
  SelectionBus bus;
  PanelContext context;
  context.document = &doc;
  context.selection = &bus;
  context.documentGeneration = 1;
  TimelineWidget widget;
  widget.options().buildInline = true;
  const double buildStart = threadCpuMs();
  runFrame(widget, context);  // starts the inline LOD build (and the plot index)
  const double firstMs = threadCpuMs() - buildStart;
  ASSERT_TRUE(widget.lodFinished());
  if (sc.inject) sc.inject(widget);
  runFrame(widget, context);
  const std::vector<Zoom> zooms = makeZooms(*model);
  const int reps = 6;

  const Sweep collapsed = sweep(widget, context, zooms, reps);
  for (size_t i = 0; i < 4; ++i) widget.setExpanded(widget.rowOrder()[i], true);
  const Sweep expanded = sweep(widget, context, zooms, reps);
  std::printf("[inst perf] %s: first frame incl. LOD build%s %.0f ms\n", sc.name,
              widget.plotLanes().hasContent() ? " and plot index" : "", firstMs);
  printSweep(sc.name, "64 rows collapsed", collapsed, sc.budgetMs);
  printSweep(sc.name, "4 rows expanded", expanded, sc.budgetMs);
  EXPECT_EQ(collapsed.minRowsDrawn, 64u);
  EXPECT_LT(collapsed.medianWorstMs, sc.budgetMs * 2);
  EXPECT_LT(expanded.medianWorstMs, sc.budgetMs * 2);
  EXPECT_LT(collapsed.steadyMs, kProductFrameMs);
  EXPECT_GE(collapsed.maxScopeItems, sc.minScopeItems);
  EXPECT_GE(collapsed.maxFrameBars, sc.minFrameBars);
  EXPECT_GE(collapsed.maxPlotColumns, sc.minPlotColumns);
}

}  // namespace

TEST(InstPerf, ScopeLanesOnAMillionScopesAt64Rows) {
  auto m = loadBase("medium.tachyon");
  auto paired = syntheticScopes(*m, 15625);
  uint64_t total = 0;
  for (const auto& v : paired->byThread) total += v.size();
  ASSERT_GE(total, 1000000u);
  m->scopeEvents.emplace_back();  // the trace "has instrumentation": the rows follow the Both mode
  const auto start = std::chrono::steady_clock::now();
  auto set = buildScopeLanes(paired, m->threads.size());
  const double buildMs = wallMs(start);
  std::printf("[inst perf] lane tracks of %llu scopes: %.1f ms (budget %.0f ms, assert %.0f ms)\n",
              static_cast<unsigned long long>(total), buildMs, kLaneBuildBudgetMs, kLaneBuildBudgetMs * 2);
  EXPECT_LT(buildMs, kLaneBuildBudgetMs * 2);
  Scenario sc{"1M scopes x64", m, [set](TimelineWidget& w) { w.scopeLanes().inject(set, false); },
              kScopeFrameBudgetMs};
  sc.minScopeItems = 64;
  run(sc);
}

TEST(InstPerf, ScopesAndFibersFixturesAt64Rows) {
  for (const char* fixture : {"scopes.tachyon", "fibers.tachyon"}) {
    auto loaded = model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / fixture);
    ASSERT_TRUE(loaded.ok());
    auto m = std::make_shared<model::TraceModel>(std::move(*loaded));
    ASSERT_TRUE(analysis::buildThreadStates(*m).ok());
    const size_t base = m->threads.size();
    const bool fibers = std::string(fixture) == "fibers.tachyon";
    std::shared_ptr<ScopeLaneSet> set;
    if (fibers) {
      auto paired = analysis::pairScopesByFiber(*m);
      ASSERT_TRUE(paired.ok());
      set = buildScopeLanes(std::make_shared<const analysis::PairedFibers>(std::move(*paired)), base);
      // Replicate the lanes of the first threads onto the other rows.
      std::vector<ScopeLane> copies;
      for (size_t t = base; t < 64; ++t) {
        for (const uint32_t li : set->lanesOfThread[t % base]) {
          ScopeLane lane = set->lanes[li];
          lane.thread = ThreadIdx(static_cast<uint32_t>(t));
          copies.push_back(std::move(lane));
        }
      }
      for (ScopeLane& l : copies) set->lanes.push_back(std::move(l));
      std::sort(set->lanes.begin(), set->lanes.end(), [](const ScopeLane& a, const ScopeLane& b) {
        return a.thread.get() != b.thread.get() ? a.thread.get() < b.thread.get() : a.fiberId < b.fiberId;
      });
      set->lanesOfThread.assign(64, {});
      for (size_t i = 0; i < set->lanes.size(); ++i) {
        set->lanesOfThread[set->lanes[i].thread.get()].push_back(static_cast<uint32_t>(i));
      }
    } else {
      auto paired = analysis::pairScopes(*m);
      ASSERT_TRUE(paired.ok());
      set = buildScopeLanes(replicatePaired(*paired, 64), 64);
    }
    replicateThreads(*m, 64);
    Scenario sc{fibers ? "fibers x64 (grouped)" : "scopes x64", m,
                [set, fibers](TimelineWidget& w) { w.scopeLanes().inject(set, fibers); },
                kScopeFrameBudgetMs};
    sc.minScopeItems = 64;
    run(sc);
  }
}

TEST(InstPerf, FrameStripWith100kFrames) {
  auto m = loadBase("medium.tachyon");
  m->frameMarks.push_back(model::FrameMark{m->header.start_ts, ThreadIdx(0), m->strings.intern("Frame"), 0});
  auto data = std::make_shared<FrameGraphData>();
  const uint64_t total = m->header.end_ts - m->header.start_ts;
  std::vector<analysis::FrameSpan> spans;
  std::mt19937_64 rng(5);
  const uint64_t period = total / 100000;
  uint64_t t = m->header.start_ts;
  for (size_t i = 0; i < 100000; ++i) {
    const uint64_t len = period * 3 / 4 + rng() % (period / 2 + 1);
    spans.push_back({i, i, t, t + len});
    t += len;
  }
  data->series.push_back({ThreadIdx(0), m->frameMarks[0].name, 100001});
  data->bars = barsFrom(spans, data->series[0]);
  // Pure layout cost first: the full view merges 100 k frames into one bar per pixel.
  {
    PlotArea area;
    area.x0 = 150.0f;
    area.width = 1700.0f;
    area.limits.traceStart = m->header.start_ts;
    area.limits.traceEnd = std::max(data->bars.end.back(), m->header.end_ts);
    area.view.span = area.limits.total();
    area.clockHz = static_cast<double>(m->header.clock_frequency_hz);
    std::vector<FrameBarItem> items;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) layoutFrameBars(data->bars, area, msToTicks(16.67, area.clockHz), items);
    const double ms = wallMs(start) / 100.0;
    std::printf("[inst perf] layoutFrameBars, 100k frames, full view: %.3f ms (%zu bars)\n", ms,
                items.size());
    EXPECT_LT(items.size(), 1710u);
    EXPECT_LT(ms, 2.0);
  }
  Scenario sc{"100k frames x64", m, [data](TimelineWidget& w) { w.frameGraph().inject(data); },
              kStripFrameBudgetMs};
  sc.minFrameBars = 100;
  run(sc);
}

TEST(InstPerf, PlotLaneWith10MSamplesAndAMarkersLane) {
  auto m = loadBase("medium.tachyon");
  const uint64_t total = m->header.end_ts - m->header.start_ts;
  model::PlotSeries p;
  p.name = m->strings.intern("frame_ms");
  const size_t n = 10000000;
  p.ts.resize(n);
  p.value.resize(n);
  std::mt19937_64 rng(9);
  for (size_t i = 0; i < n; ++i) {
    p.ts[i] = m->header.start_ts + static_cast<uint64_t>(static_cast<double>(total) * static_cast<double>(i) /
                                                         static_cast<double>(n));
    p.value[i] = 16.0 + static_cast<double>(rng() % 1000) * 0.002 + (i % 999983 == 0 ? 40.0 : 0.0);
  }
  m->plots.push_back(std::move(p));
  for (size_t i = 0; i < 100000; ++i) {
    model::Marker mk;
    mk.ts = m->header.start_ts + total / 100000 * i;
    mk.name = m->strings.intern("marker " + std::to_string(i % 50));
    m->markers.push_back(mk);
  }
  // The index build runs off the UI thread in the app: its own budget.
  {
    const auto start = std::chrono::steady_clock::now();
    auto idx = buildPlotIndex(m, nullptr);
    const double ms = wallMs(start);
    std::printf(
        "[inst perf] plot index of %zu samples and %zu markers: %.1f ms (budget %.0f ms, assert %.0f ms)\n",
        n, m->markers.size(), ms, kPlotIndexBudgetMs, kPlotIndexBudgetMs * 2);
    EXPECT_LT(ms, kPlotIndexBudgetMs * 2);
  }
  // The pure part of a frame: the columns of the plot and the ticks of the markers at a few zooms.
  {
    auto idx = buildPlotIndex(m, nullptr);
    double worstColumns = 0, worstMarkers = 0;
    for (const double frac : {1.0, 0.25, 0.01, 0.0001}) {
      PlotArea area;
      area.x0 = 150.0f;
      area.width = 1700.0f;
      area.limits.traceStart = m->header.start_ts;
      area.limits.traceEnd = m->header.end_ts;
      area.view.span = area.limits.total() * frac;
      area.view.start = area.limits.total() * 0.3;
      area.clockHz = static_cast<double>(m->header.clock_frequency_hz);
      PlotWindow w;
      std::vector<MarkerItem> items;
      auto t = std::chrono::steady_clock::now();
      for (int i = 0; i < 50; ++i) queryPlotColumns(idx->series[0], area, 1.0f, w);
      worstColumns = std::max(worstColumns, wallMs(t) / 50.0);
      t = std::chrono::steady_clock::now();
      const MarkerView view{&m->markers, &idx->markerOrder};
      for (int i = 0; i < 50; ++i) layoutMarkers(view, area, 4000, items);
      worstMarkers = std::max(worstMarkers, wallMs(t) / 50.0);
    }
    std::printf(
        "[inst perf] queryPlotColumns (10M samples) worst zoom %.3f ms, layoutMarkers (100k) %.3f ms\n",
        worstColumns, worstMarkers);
    EXPECT_LT(worstColumns, 2.0);
    EXPECT_LT(worstMarkers, 1.0);
  }
  Scenario sc{"10M plot samples x64", m, nullptr, kPlotFrameBudgetMs};
  sc.minPlotColumns = 100;
  run(sc);
}
