// Perf budgets (WP-47), asserted at 2x:
//  * fuzzy search over 200 000 names, one keystroke (every prefix of several queries): < 15 ms;
//  * one timeline frame with the instance-highlight overlay on 64 rows of `medium` (the same shape as the
//    timeline perf test) within the existing timeline budget: median frame of the worst zoom < 5 ms, steady
//    frame < 8 ms, and the overlay adds little on top of the same frames without it.
// The numbers are printed.
#include "analysis/instances.h"
#include "analysis/thread_states.h"
#include "viewer/document.h"
#include "viewer/selection_bus.h"
#include "viewer/stats/stats_model.h"
#include "viewer/timeline/timeline_widget.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;
namespace st = tachyon::viewer::stats;

namespace {

constexpr double kSearchBudgetMs = 15.0;
constexpr double kFrameBudgetMs = 8.0;
constexpr double kRegressionBudgetMs = 5.0;
constexpr float kDisplayW = 1920.0f;
constexpr float kDisplayH = 6000.0f;

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double threadCpuMs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) * 1e-6;
#endif
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// ---- fuzzy search
// -------------------------------------------------------------------------------------------

TEST(StatsPerf, FuzzySearchOverTwoHundredThousandNames) {
  constexpr size_t kNames = 200'000;
  st::FuzzyIndex index;
  uint64_t x = 0x9E3779B97F4A7C15ull;
  const auto next = [&] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  };
  static const char* const kWords[] = {"Engine", "Render", "Physics", "Update", "Scene", "Job",  "Fiber",
                                       "Mesh",   "Audio",  "Input",   "Stream", "Cache", "Pool", "Worker"};
  for (size_t i = 0; i < kNames; ++i) {
    std::string name = "module" + std::to_string(next() % 40) + ".dll!";
    name += kWords[next() % 14];
    name += "::";
    name += kWords[next() % 14];
    name += kWords[next() % 14];
    name += "::method_" + std::to_string(next() % 100000);
    if (i % 3 == 0) name += "<lambda_" + std::to_string(i) + ">";
    index.add(name);
  }
  ASSERT_EQ(index.size(), kNames);

  struct Typed {
    const char* text;
  };
  // Every prefix of each query is one keystroke.
  const char* const queries[] = {"physicsupdate", "rndr", "engine::job", "mesh",
                                 "zzzz",          "a",    "w0rk",        "method_9"};
  double worst = 0;
  std::string worstQuery;
  size_t results = 0;
  for (const char* q : queries) {
    const std::string full = q;
    for (size_t len = 1; len <= full.size(); ++len) {
      const std::string text = full.substr(0, len);
      double best = 1e30;
      for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = index.search(text, st::kMaxSearchResults);
        best = std::min(best, msSince(t0));
        results = r.size();
      }
      if (best > worst) {
        worst = best;
        worstQuery = text;
      }
      EXPECT_LT(best, kSearchBudgetMs * 2.0) << "query \"" << text << "\"";
      (void)results;
    }
  }
  std::printf(
      "[stats perf] fuzzy search over %zu names: worst keystroke \"%s\" %.2f ms (budget %.0f ms, assert %.0f "
      "ms)\n",
      kNames, worstQuery.c_str(), worst, kSearchBudgetMs, kSearchBudgetMs * 2.0);
}

// ---- timeline frame with the overlay ----------------------------------------------------------------------

namespace {

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
  r.stats = widget.frameStats();
  return r;
}

struct SweepResult {
  double medianWorstMs = 0;
  std::string label;
  uint32_t maxHighlightRects = 0;
  uint32_t minRowsDrawn = 1u << 30;
};

SweepResult sweep(TimelineWidget& widget, const PanelContext& context, const model::TraceModel& m) {
  const TimeLimits limits = widget.limits();
  const double total = static_cast<double>(m.header.end_ts - m.header.start_ts);
  const double hz = static_cast<double>(m.header.clock_frequency_hz);
  SweepResult s;
  for (double span = total; span >= hz * 1e-4; span /= 2) {
    for (const double pos : {0.0, 0.4, 0.73}) {
      std::vector<double> counted;
      for (int i = 0; i < 8; ++i) {
        TimeView v;
        v.span = span;
        v.start = (limits.total() - span) * pos + i;
        widget.setView(v);
        const FrameResult r = runFrame(widget, context);
        if (i == 0) continue;
        counted.push_back(r.cpuMs);
        s.maxHighlightRects = std::max(s.maxHighlightRects, r.stats.highlightRects);
        s.minRowsDrawn = std::min(s.minRowsDrawn, r.stats.rowsDrawn);
      }
      std::sort(counted.begin(), counted.end());
      const double median = counted[counted.size() / 2];
      if (median > s.medianWorstMs) {
        s.medianWorstMs = median;
        char label[64];
        std::snprintf(label, sizeof label, "%.3f ms @%.2f", span * 1e3 / hz, pos);
        s.label = label;
      }
    }
  }
  return s;
}

}  // namespace

TEST(StatsPerf, TimelineFrameWithTheInstanceOverlayOnMedium) {
  auto loaded =
      model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel m = std::move(*loaded);
  ASSERT_TRUE(analysis::buildThreadStates(m).ok());

  // The instances of a busy function, computed on the original threads.
  std::shared_ptr<const std::vector<analysis::Instance>> base;
  {
    symbols::SymbolService svc(std::vector<std::string>{}, std::filesystem::path{});
    ASSERT_TRUE(svc.addSymbolFileOverride(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) /
                                          "medium.symbols.json")
                    .ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
    analysis::QueryContext ctx(m, svc);
    auto rows = analysis::bottomUpFunctions(ctx, {});
    ASSERT_TRUE(rows.ok());
    for (size_t i = 0; i < rows->size() && i < 12; ++i) {
      auto inst = analysis::functionInstances(ctx, {}, (*rows)[i].key);
      ASSERT_TRUE(inst.ok());
      if (!base || (*inst)->size() > base->size()) base = *inst;
    }
  }
  ASSERT_TRUE(base);
  ASSERT_GT(base->size(), 1000u) << "the overlay needs a busy function";

  // 64 rows like the timeline perf test: threads replicated cyclically, instances replicated with them.
  const size_t baseThreads = m.threads.size();
  for (size_t i = baseThreads; i < 64; ++i) {
    model::Thread t = m.threads[i % baseThreads];
    t.tid += static_cast<uint32_t>(1000 * (i / baseThreads));
    t.name += "_r" + std::to_string(i / baseThreads);
    m.threads.push_back(std::move(t));
    m.samplesByThread.push_back(m.samplesByThread[i % baseThreads]);
    m.statesByThread.push_back(m.statesByThread[i % baseThreads]);
    m.scopesByThread.emplace_back();
  }
  auto all = std::make_shared<std::vector<analysis::Instance>>();
  for (const analysis::Instance& in : *base) {
    for (size_t k = in.thread.get(); k < 64; k += baseThreads)
      all->push_back({ThreadIdx(static_cast<uint32_t>(k)), in.begin, in.end});
  }
  std::sort(all->begin(), all->end(), [](const analysis::Instance& a, const analysis::Instance& b) {
    return a.begin != b.begin ? a.begin < b.begin : a.thread < b.thread;
  });
  const size_t instanceCount = all->size();

  auto model = std::make_shared<const model::TraceModel>(std::move(m));
  ImGuiHeadless imgui;
  Document doc("perf.tachyon", model);
  SelectionBus bus;
  PanelContext context;
  context.document = &doc;
  context.selection = &bus;
  context.documentGeneration = 1;

  TimelineWidget widget;
  widget.options().buildInline = true;
  runFrame(widget, context);
  ASSERT_TRUE(widget.lodFinished());
  runFrame(widget, context);

  const SweepResult without = sweep(widget, context, *model);
  bus.setInstanceHighlight(
      base->empty() ? std::nullopt : std::optional(symbols::FunctionKey{ModuleIdx(0), 1}), all);
  runFrame(widget, context);  // builds the per-thread index
  const SweepResult with = sweep(widget, context, *model);

  widget.fit();
  runFrame(widget, context);
  double steady = 1e30;
  uint32_t steadyRects = 0;
  for (int i = 0; i < 10; ++i) {
    const FrameResult r = runFrame(widget, context);
    steady = std::min(steady, r.cpuMs);
    steadyRects = r.stats.highlightRects;
  }
  std::printf(
      "[stats perf] medium x64 rows, %zu highlighted instances: median frame of the worst zoom %.3f ms (%s) "
      "with the overlay vs %.3f ms (%s) without; max %u overlay rects/frame; steady frame %.3f ms "
      "(%u rects); budget %.1f ms, assert %.1f ms (steady %.1f ms)\n",
      instanceCount, with.medianWorstMs, with.label.c_str(), without.medianWorstMs, without.label.c_str(),
      with.maxHighlightRects, steady, steadyRects, kRegressionBudgetMs, kRegressionBudgetMs * 2,
      kFrameBudgetMs);

  EXPECT_EQ(with.minRowsDrawn, 64u);
  EXPECT_GT(with.maxHighlightRects, 100u) << "the overlay must actually be drawn";
  EXPECT_LT(with.medianWorstMs, kRegressionBudgetMs * 2);
  EXPECT_LT(steady, kFrameBudgetMs);
  // The overlay itself must stay a small part of the frame.
  EXPECT_LT(with.medianWorstMs - without.medianWorstMs, kRegressionBudgetMs);
}
