// WP-54: the instrumentation lanes, the frame graph and the plot / marker lanes driven headless (Null
// renderer, real ImGui frames, injected input) on the `scopes`, `fibers` and `frames` fixtures; the shared
// view options (blend, group by fiber) as the Call Graph, Function List, Butterfly and Function Statistics
// see them; and lifetime checks for closing a document with the pairing query in flight.
#include "analysis/query.h"
#include "analysis/scope_pairing.h"
#include "viewer/app.h"
#include "viewer/callgraph/callgraph_panel.h"
#include "viewer/common/scope_view_options.h"
#include "viewer/functions/butterfly_panel.h"
#include "viewer/functions/function_list_model.h"
#include "viewer/functions/function_list_panel.h"
#include "viewer/stats/function_stats_panel.h"
#include "viewer/timeline/timeline_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <set>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;
namespace fs = std::filesystem;
using symbols::FunctionKey;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

AppOptions headlessOptions() {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_inst_test_no_such_settings.json";
  return o;
}

class Harness {
 public:
  Harness() : app(makeNullRenderer(), headlessOptions()) {
    std::string error;
    EXPECT_TRUE(app.init(error)) << error;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigInputTrickleEventQueue = false;
    timeline = find<ThreadsTimelinePanel>(ThreadsTimelinePanel::kName);
    callGraph = find<CallGraphPanel>(CallGraphPanel::kName);
    list = find<FunctionListPanel>(FunctionListPanel::kName);
    butterfly = find<ButterflyPanel>(ButterflyPanel::kName);
    stats = find<FunctionStatsPanel>(FunctionStatsPanel::kName);
    for (PanelEntry& e : app.panels().entries()) e.open = e.panel.get() == timeline;
    timeline->widget().options().recordDrawnItems = true;
  }

  template <typename T>
  T* find(const char* name) {
    PanelEntry* entry = app.panels().find(name);
    EXPECT_NE(entry, nullptr) << name;
    T* p = entry ? dynamic_cast<T*>(entry->panel.get()) : nullptr;
    EXPECT_NE(p, nullptr) << name;
    return p;
  }
  void openPanel(const char* name) { app.panels().find(name)->open = true; }

  SelectionBus& bus() { return app.selection(); }
  TimelineWidget& widget() { return timeline->widget(); }
  const model::TraceModel& model() { return app.document()->model(); }

  bool until(const std::function<bool()>& pred, int seconds = 60) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  void frames(int n = 1) {
    for (int i = 0; i < n; ++i) app.frame(0);
  }
  bool symbolsDone() {
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    return p.total > 0 && p.done == p.total;
  }

  // Opens a fixture; waits for the document, its symbols and the timeline index; the symbol generation then
  // stays put for a moment so that the panels' queries settle on one generation.
  void open(const char* name) {
    app.openFile(fixture(name));
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    ASSERT_TRUE(until([&] { return symbolsDone(); }));
    for (;;) {
      const uint64_t generation = app.symbolGeneration();
      const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
      while (std::chrono::steady_clock::now() < stop) {
        frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (generation == app.symbolGeneration()) break;
    }
    ASSERT_TRUE(until([&] { return widget().lodFinished(); }));
    layout();
  }
  void waitLanes() {
    ASSERT_TRUE(until([&] { return widget().scopeLanes().ready(); }));
    frames(3);
  }
  // The timeline fills the top of the 1280x720 display, the analysis panels sit below.
  void layout() {
    frames(2);
    ImGui::SetWindowPos(ThreadsTimelinePanel::kName, ImVec2(0, 20));
    ImGui::SetWindowSize(ThreadsTimelinePanel::kName, ImVec2(1260, 600));
    frames(3);
  }
  // Opens panels below a shorter timeline, side by side, so that none covers the timeline.
  void openBelow(std::initializer_list<const char*> names) {
    for (const char* n : names) openPanel(n);
    frames(3);
    ImGui::SetWindowPos(ThreadsTimelinePanel::kName, ImVec2(0, 20));
    ImGui::SetWindowSize(ThreadsTimelinePanel::kName, ImVec2(1260, 380));
    float x = 0;
    for (const char* n : names) {
      ImGui::SetWindowPos(n, ImVec2(x, 410));
      ImGui::SetWindowSize(n, ImVec2(420, 300));
      x += 425;
    }
    frames(3);
  }

  PanelContext context() {
    PanelContext c;
    c.document = app.document().get();
    c.selection = &app.selection();
    c.symbols = app.document() ? app.document()->symbols().get() : nullptr;
    c.queryExecutor = &app.queryExecutor();
    c.documentGeneration = app.documentGeneration();
    c.symbolGeneration = app.symbolGeneration();
    return c;
  }

  void mouse(float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    frames(1);
  }
  void click(float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frames(1);
    io.AddMouseButtonEvent(0, true);
    frames(1);
    io.AddMouseButtonEvent(0, false);
    frames(1);
  }

  ThreadIdx threadOfTid(uint32_t tid) { return model().tidToThread.at(tid); }

  // The first drawn scope item of the last frame matching `pred`.
  const DrawnScopeItem* findItem(const std::function<bool(const DrawnScopeItem&)>& pred) {
    for (const DrawnScopeItem& it : widget().scopeLanes().drawnItems()) {
      if (pred(it)) return &it;
    }
    return nullptr;
  }

  App app;
  ThreadsTimelinePanel* timeline = nullptr;
  CallGraphPanel* callGraph = nullptr;
  FunctionListPanel* list = nullptr;
  ButterflyPanel* butterfly = nullptr;
  FunctionStatsPanel* stats = nullptr;
};

bool hasLine(const std::vector<std::string>& lines, const std::string& needle) {
  return std::any_of(lines.begin(), lines.end(),
                     [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

// Depth-first search of a NodeAgg for a key, iterative (the trees can be deep).
bool treeContains(const analysis::NodeAgg& root, const FunctionKey& key) {
  std::vector<const analysis::NodeAgg*> stack{&root};
  while (!stack.empty()) {
    const analysis::NodeAgg* n = stack.back();
    stack.pop_back();
    if (n->key == key) return true;
    for (const analysis::NodeAgg& c : n->children) stack.push_back(&c);
  }
  return false;
}

}  // namespace

// ---- shared options (pure)
// -----------------------------------------------------------------------------------

TEST(InstOptions, ViewOptionsPublishOnceAndSurviveClear) {
  SelectionBus bus;
  int calls = 0;
  SelectionField last = SelectionField::All;
  bus.subscribe([&](const SelectionState&, SelectionField f) {
    ++calls;
    last = f;
  });
  EXPECT_TRUE(bus.state().viewOptions.blendScopes);  // on by default
  EXPECT_FALSE(bus.state().viewOptions.groupByFiber);
  bus.setBlendScopes(true);
  EXPECT_EQ(calls, 0);  // unchanged: no event
  bus.setBlendScopes(false);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(last, SelectionField::ViewOptions);
  bus.setGroupByFiber(true);
  EXPECT_EQ(calls, 2);
  bus.clear();
  EXPECT_EQ(calls, 3);                                // the clear itself
  EXPECT_FALSE(bus.state().viewOptions.blendScopes);  // preferences survive a new document
  EXPECT_TRUE(bus.state().viewOptions.groupByFiber);
}

TEST(InstOptions, SettingsPersistTheBlendChoiceAndTheFrameBudget) {
  Settings s;
  EXPECT_TRUE(s.blendScopes);
  EXPECT_DOUBLE_EQ(s.frameBudgetMs, 16.67);
  s.blendScopes = false;
  s.frameBudgetMs = 33.3;
  const fs::path file = fs::path(TACHYON_BINARY_DIR) / "viewer_inst_settings" / "settings.json";
  ASSERT_TRUE(s.save(file).ok());
  const Settings back = Settings::load(file);
  EXPECT_FALSE(back.blendScopes);
  EXPECT_DOUBLE_EQ(back.frameBudgetMs, 33.3);
  EXPECT_EQ(back, s);
  fs::remove_all(file.parent_path());
}

TEST(InstOptions, BlendingAndFiberGroupingAreOnlyEffectiveWhereTheTraceHasThem) {
  model::TraceModel plain;
  ViewOptions wish;
  wish.blendScopes = true;
  wish.groupByFiber = true;
  const common::EffectiveScopeOptions none = common::effectiveScopeOptions(plain, wish);
  EXPECT_FALSE(none.blendScopes);
  EXPECT_FALSE(none.groupByFiber);

  model::TraceModel scoped;
  scoped.scopeEvents.emplace_back();
  const common::EffectiveScopeOptions blend = common::effectiveScopeOptions(scoped, wish);
  EXPECT_TRUE(blend.blendScopes);
  EXPECT_FALSE(blend.groupByFiber);  // no fiber events
  wish.blendScopes = false;
  EXPECT_FALSE(common::effectiveScopeOptions(scoped, wish).blendScopes);

  model::TraceModel fibered;
  fibered.fiberSwitches.emplace_back();
  EXPECT_TRUE(common::effectiveScopeOptions(fibered, wish).groupByFiber);

  analysis::AggregationOptions options;
  common::applyScopeOptions(options, {true, true});
  EXPECT_TRUE(options.blendScopes);
  EXPECT_TRUE(options.groupByFiber);
}

TEST(InstOptions, ASelectedScopeForcesBlendingBecauseItOnlyExistsInBlendedStacks) {
  const FunctionKey scope = FunctionKey::scope(StringIdx(3));
  EXPECT_TRUE(
      functions::effectiveOptions(false, analysis::KernelFrames::Show, scope, false, false).blendScopes);
  EXPECT_FALSE(functions::effectiveOptions(false, analysis::KernelFrames::Show, std::nullopt, false, false)
                   .blendScopes);
  const FunctionKey sampled{ModuleIdx(0), 0x1000};
  EXPECT_FALSE(
      functions::effectiveOptions(false, analysis::KernelFrames::Show, sampled, false, true).blendScopes);
  EXPECT_TRUE(
      functions::effectiveOptions(false, analysis::KernelFrames::Show, sampled, false, true).groupByFiber);
}

// ---- scope lanes
// ---------------------------------------------------------------------------------------------

TEST(InstWidget, ScopeLanesAppearUnderTheThreadRowsOfAnInstrumentedTrace) {
  Harness h;
  h.open("scopes.tachyon");
  EXPECT_EQ(h.widget().effectiveRowMode(h.threadOfTid(301)), RowMode::Both);
  h.waitLanes();
  const TimelineFrameStats& s = h.widget().frameStats();
  EXPECT_GT(s.scopeItems, 0u);
  EXPECT_GT(s.scopeLines, 0u);
  EXPECT_EQ(s.scopeRowsPending, 0u);
  EXPECT_FALSE(h.widget().scopeLanes().pending());

  // Every depth-0 scope of Main is drawn in exactly one item.
  auto paired = analysis::pairScopes(h.model());
  ASSERT_TRUE(paired.ok());
  const ThreadIdx main_ = h.threadOfTid(301);
  size_t depth0 = 0;
  for (const auto& sc : paired->byThread[main_.get()]) depth0 += sc.depth == 0 ? 1 : 0;
  ASSERT_GT(depth0, 50u);
  size_t drawn = 0;
  for (const DrawnScopeItem& it : h.widget().scopeLanes().drawnItems()) {
    if (it.thread == main_ && it.depth == 0) drawn += it.count;
  }
  EXPECT_EQ(drawn, depth0);
  // A collapsed row shows the outermost line only; the Worker has one too.
  EXPECT_EQ(h.widget().rowLayoutOf(main_)->scopeLines, 1u);
  EXPECT_EQ(h.widget().rowLayoutOf(h.threadOfTid(302))->scopeLines, 1u);
}

TEST(InstWidget, ExpandingARowShowsTheNestedDepthsAndZoomingInLabelsTheScopes) {
  Harness h;
  h.open("scopes.tachyon");
  h.waitLanes();
  const ThreadIdx main_ = h.threadOfTid(301);
  h.widget().setExpanded(main_, true);
  h.frames(3);
  const RowLayout* row = h.widget().rowLayoutOf(main_);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->scopeLines, 4u);  // Frame > Update > Physics > Solve
  std::set<uint32_t> depths;
  for (const DrawnScopeItem& it : h.widget().scopeLanes().drawnItems())
    if (it.thread == main_) depths.insert(it.depth);
  EXPECT_EQ(depths, (std::set<uint32_t>{0, 1, 2, 3}));
  // Items are stacked: the depth-1 line is below the depth-0 line.
  const DrawnScopeItem* d0 =
      h.findItem([&](const DrawnScopeItem& i) { return i.thread == main_ && i.depth == 0; });
  const DrawnScopeItem* d1 =
      h.findItem([&](const DrawnScopeItem& i) { return i.thread == main_ && i.depth == 1; });
  ASSERT_TRUE(d0 && d1);
  EXPECT_GT(d1->y0, d0->y1 - 0.01f);

  // Zoom to ~100 ms: scopes are wide enough to carry their names.
  const auto& lim = h.widget().limits();
  h.widget().setView({0.0, static_cast<double>(lim.traceEnd - lim.traceStart) * 0.05});
  h.frames(3);
  EXPECT_GT(h.widget().frameStats().scopeLabels, 0u);
  EXPECT_NE(h.findItem([](const DrawnScopeItem& i) { return i.labelled; }), nullptr);
}

TEST(InstWidget, RowModesToggleGloballyAndPerRow) {
  Harness h;
  h.open("scopes.tachyon");
  h.waitLanes();
  const ThreadIdx main_ = h.threadOfTid(301), worker = h.threadOfTid(302);
  const float both = h.widget().rowLayoutOf(main_)->height;
  EXPECT_GT(h.widget().frameStats().scopeItems, 0u);

  h.widget().setGlobalRowMode(RowMode::Sampled);
  h.frames(3);
  EXPECT_EQ(h.widget().frameStats().scopeItems, 0u);
  EXPECT_EQ(h.widget().rowLayoutOf(main_)->scopeLines, 0u);
  const float sampled = h.widget().rowLayoutOf(main_)->height;
  EXPECT_LT(sampled, both);

  h.widget().setGlobalRowMode(RowMode::Scopes);
  h.frames(3);
  EXPECT_GT(h.widget().frameStats().scopeItems, 0u);
  const RowLayout* scopesOnly = h.widget().rowLayoutOf(main_);
  EXPECT_EQ(scopesOnly->mode, RowMode::Scopes);
  EXPECT_LT(scopesOnly->height, both);  // the sampled strip is gone
  // The strip is not hit in a scopes-only row: just under the band comes the scope line.
  const RowHit hit =
      hitRowSection(*scopesOnly, RowGeometry{}, stripTop(RowGeometry{}) + RowGeometry{}.scopeGap + 3.0f);
  EXPECT_EQ(hit.section, RowSection::Scope);

  // One row differently from the toolbar.
  h.widget().setRowMode(worker, RowMode::Sampled);
  h.frames(3);
  EXPECT_EQ(h.widget().effectiveRowMode(worker), RowMode::Sampled);
  EXPECT_EQ(h.widget().rowLayoutOf(worker)->scopeLines, 0u);
  EXPECT_EQ(h.widget().rowLayoutOf(main_)->mode, RowMode::Scopes);
  h.widget().setGlobalRowMode(RowMode::Both);  // the toolbar resets the per-row choices
  h.frames(3);
  EXPECT_EQ(h.widget().effectiveRowMode(worker), RowMode::Both);
  EXPECT_FLOAT_EQ(h.widget().rowLayoutOf(main_)->height, both);
}

TEST(InstWidget, TracesWithoutInstrumentationStayExactlyAsBefore) {
  Harness h;
  h.open("tiny.tachyon");
  h.frames(3);
  EXPECT_EQ(h.widget().effectiveRowMode(ThreadIdx(0)), RowMode::Sampled);
  EXPECT_EQ(h.widget().frameStats().scopeItems, 0u);
  EXPECT_EQ(h.widget().frameStats().scopeRows, 0u);
  EXPECT_EQ(h.widget().scopeLanes().issuedQueries(), 0u);  // nothing was paired
  const RowLayout* r = h.widget().rowLayoutOf(h.widget().rowOrder().front());
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->scopeLines, 0u);
  EXPECT_EQ(r->mode, RowMode::Sampled);
  const common::EffectiveScopeOptions eff = common::effectiveScopeOptions(h.context());
  EXPECT_FALSE(eff.blendScopes);
  EXPECT_FALSE(eff.groupByFiber);
}

TEST(InstWidget, HoveringAScopeShowsItsTooltipAndPublishesItOnTheBus) {
  Harness h;
  h.open("scopes.tachyon");
  h.waitLanes();
  const ThreadIdx main_ = h.threadOfTid(301);
  const StringIdx frameId = h.model().strings.find("Frame");
  ASSERT_TRUE(frameId.valid());
  const DrawnScopeItem* item = h.findItem([&](const DrawnScopeItem& i) {
    return i.thread == main_ && i.depth == 0 && i.count == 1 && i.id == frameId && i.x1 - i.x0 > 3.0f;
  });
  ASSERT_NE(item, nullptr);
  const float x = (item->x0 + item->x1) * 0.5f, y = (item->y0 + item->y1) * 0.5f;
  h.mouse(x, y);
  h.frames(1);
  const auto& tip = h.widget().lastTooltip();
  EXPECT_TRUE(hasLine(tip, "Scope: Frame"));
  EXPECT_TRUE(hasLine(tip, "duration: "));
  EXPECT_TRUE(hasLine(tip, "depth: 0"));
  EXPECT_TRUE(hasLine(tip, "thread: Main (tid 301)"));
  ASSERT_TRUE(h.widget().lastHoveredScope().has_value());
  EXPECT_EQ(*h.widget().lastHoveredScope(), FunctionKey::scope(frameId));
  EXPECT_EQ(h.bus().state().hovered.kind, HoveredItem::Kind::Scope);
  EXPECT_EQ(h.bus().state().hovered.function, FunctionKey::scope(frameId));
}

TEST(InstWidget, ClickingAScopeSelectsItAndTheAnalysisPanelsFollow) {
  Harness h;
  h.open("scopes.tachyon");
  h.openBelow({CallGraphPanel::kName, FunctionListPanel::kName, ButterflyPanel::kName});
  h.waitLanes();
  const ThreadIdx main_ = h.threadOfTid(301);
  const StringIdx updateId = h.model().strings.find("Update");
  ASSERT_TRUE(updateId.valid());
  h.widget().setExpanded(main_, true);
  h.widget().setView(
      {0.0, static_cast<double>(h.widget().limits().traceEnd - h.widget().limits().traceStart) * 0.1});
  h.frames(3);
  const DrawnScopeItem* item = h.findItem([&](const DrawnScopeItem& i) {
    return i.thread == main_ && i.depth == 1 && i.count == 1 && i.id == updateId && i.x1 - i.x0 > 4.0f;
  });
  ASSERT_NE(item, nullptr);
  h.click((item->x0 + item->x1) * 0.5f, (item->y0 + item->y1) * 0.5f);

  const FunctionKey key = FunctionKey::scope(updateId);
  ASSERT_TRUE(h.bus().state().selectedFunction.has_value());
  EXPECT_EQ(*h.bus().state().selectedFunction, key);
  EXPECT_EQ(h.bus().state().selectedThread, main_);

  // The Call Graph is blended by default (the trace has scopes) and shows the scope row, selected.
  ASSERT_TRUE(h.until([&] {
    return h.callGraph->selectedId().has_value() && h.callGraph->selectedKey() == key &&
           !h.callGraph->pending();
  }));
  EXPECT_TRUE(h.callGraph->requestedSpec().blendScopes);
  ASSERT_NE(h.callGraph->tree(), nullptr);
  EXPECT_TRUE(treeContains(*h.callGraph->tree(), key));
  bool rowShown = false;
  for (const auto& row : h.callGraph->rows()) rowShown = rowShown || row.id == *h.callGraph->selectedId();
  EXPECT_TRUE(rowShown);
  EXPECT_TRUE(h.callGraph->nameOf(key).scope);
  EXPECT_EQ(h.callGraph->nameOf(key).function, "Update");

  // The Function List has the scope as an entry (flagged for the scope icon); the Butterfly shows its
  // callers.
  ASSERT_TRUE(h.until([&] { return h.list->table() != nullptr && h.list->shownSpec().blendScopes; }));
  bool listed = false;
  for (const functions::Entry& e : h.list->table()->entries) {
    if (e.key == key) {
      listed = true;
      EXPECT_TRUE(e.scope);
      EXPECT_EQ(e.function, "Update");
      EXPECT_TRUE(e.module.empty());
    }
  }
  EXPECT_TRUE(listed);
  ASSERT_TRUE(h.until([&] {
    return h.butterfly->data() != nullptr && h.butterfly->shownSpec() == h.butterfly->requestedSpec();
  }));
  EXPECT_TRUE(h.butterfly->shownSpec().base.blendScopes);
}

TEST(InstWidget, FunctionStatisticsWorkOnTheSelectedScope) {
  Harness h;
  h.open("scopes.tachyon");
  h.openBelow({FunctionStatsPanel::kName});
  h.waitLanes();
  const ThreadIdx main_ = h.threadOfTid(301);
  const StringIdx frameId = h.model().strings.find("Frame");
  const DrawnScopeItem* item = h.findItem([&](const DrawnScopeItem& i) {
    return i.thread == main_ && i.depth == 0 && i.count == 1 && i.id == frameId && i.x1 - i.x0 > 3.0f;
  });
  ASSERT_NE(item, nullptr);
  h.click((item->x0 + item->x1) * 0.5f, (item->y0 + item->y1) * 0.5f);
  ASSERT_EQ(h.bus().state().selectedFunction, std::optional<FunctionKey>(FunctionKey::scope(frameId)));
  ASSERT_TRUE(h.stats->populateFromSelection(h.context()));
  ASSERT_TRUE(h.until([&] {
    return h.stats->result() != nullptr && h.stats->result()->key == FunctionKey::scope(frameId) &&
           !h.stats->statsPending() && h.stats->shownStatsSpec() == h.stats->requestedStatsSpec();
  }));
  analysis::QueryContext qc(h.model(), *h.app.document()->symbols());
  auto expected = analysis::scopeStats(qc, analysis::Filter{}, frameId);
  ASSERT_TRUE(expected.ok());
  EXPECT_EQ(h.stats->result()->stats->count, expected->count);
  EXPECT_EQ(h.stats->result()->stats->median, expected->median);
  EXPECT_GT(h.stats->result()->stats->count, 50u);
  EXPECT_EQ(h.stats->result()->instances->size(), expected->count);
}

TEST(InstWidget, DisablingTheBlendRemovesTheScopesFromTheCallGraph) {
  Harness h;
  h.open("scopes.tachyon");
  h.openBelow({CallGraphPanel::kName});
  h.waitLanes();
  const StringIdx frameId = h.model().strings.find("Frame");
  const FunctionKey key = FunctionKey::scope(frameId);
  ASSERT_TRUE(h.until([&] { return h.callGraph->tree() != nullptr && !h.callGraph->pending(); }));
  EXPECT_TRUE(h.callGraph->shownSpec().blendScopes);
  EXPECT_TRUE(treeContains(*h.callGraph->tree(), key));

  h.bus().setBlendScopes(false);
  ASSERT_TRUE(h.until([&] { return !h.callGraph->shownSpec().blendScopes && !h.callGraph->pending(); }));
  EXPECT_FALSE(treeContains(*h.callGraph->tree(), key));
  h.bus().setBlendScopes(true);
  ASSERT_TRUE(h.until([&] { return h.callGraph->shownSpec().blendScopes && !h.callGraph->pending(); }));
  EXPECT_TRUE(treeContains(*h.callGraph->tree(), key));
}

TEST(InstWidget, LockWaitsAreHatchedAndUnterminatedScopesGetAnOpenEnd) {
  Harness h;
  h.open("scopes.tachyon");
  h.waitLanes();
  const ThreadIdx main_ = h.threadOfTid(301);
  const model::TraceModel& m = h.model();
  // A hand-made pairing: a lock wait, an unterminated scope and a plain one on Main.
  auto paired = std::make_shared<analysis::PairedScopes>();
  paired->byThread.resize(m.threads.size());
  const uint64_t t0 = m.header.start_ts;
  const uint64_t span = m.header.end_ts - m.header.start_ts;
  const StringIdx lockId = m.strings.find("Frame");  // any valid id: only the colour marks a lock wait
  analysis::ScopeInstance lock;
  lock.id = lockId;
  lock.begin = t0 + span / 10;
  lock.end = t0 + span / 4;
  lock.color = analysis::kLockColorSentinel;
  analysis::ScopeInstance open;
  open.id = lockId;
  open.begin = t0 + span / 2;
  open.end = t0 + span * 3 / 4;
  open.unterminated = true;
  open.depth = 0;
  analysis::ScopeInstance plain;
  plain.id = lockId;
  plain.begin = t0 + span * 4 / 5;
  plain.end = t0 + span * 9 / 10;
  paired->byThread[main_.get()] = {lock, open, plain};
  auto set = buildScopeLanes(paired, m.threads.size());
  h.widget().scopeLanes().inject(set, false);
  h.frames(3);
  const DrawnScopeItem* l = h.findItem([&](const DrawnScopeItem& i) { return i.thread == main_ && i.lock; });
  ASSERT_NE(l, nullptr);
  EXPECT_GE(h.widget().scopeLanes().frameStats().lockHatches, 1u);
  EXPECT_GE(h.widget().scopeLanes().frameStats().openEnds, 1u);
  EXPECT_NE(h.findItem([&](const DrawnScopeItem& i) { return i.thread == main_ && i.unterminated; }),
            nullptr);
  // The tooltip of the lock wait says so (no summary here: only the kind and the unknown address).
  h.mouse((l->x0 + l->x1) * 0.5f, (l->y0 + l->y1) * 0.5f);
  h.frames(1);
  EXPECT_TRUE(hasLine(h.widget().lastTooltip(), "Lock wait: "));
  EXPECT_TRUE(hasLine(h.widget().lastTooltip(), "lock address: unknown"));
}

// ---- fibers
// --------------------------------------------------------------------------------------------------

TEST(InstWidget, GroupByFiberSplitsTheLanesPerFiberAndEveryPanelFollows) {
  Harness h;
  h.open("fibers.tachyon");
  h.openBelow({CallGraphPanel::kName});
  h.waitLanes();
  EXPECT_FALSE(h.widget().scopeLanes().grouped());
  const ThreadIdx t401 = h.threadOfTid(401);
  const auto fibersOn = [&](ThreadIdx t) {
    std::set<uint64_t> ids;
    for (const uint32_t li : h.widget().scopeLanes().set()->lanesFor(t))
      ids.insert(h.widget().scopeLanes().set()->lanes[li].fiberId);
    return ids;
  };
  EXPECT_EQ(fibersOn(t401), (std::set<uint64_t>{0}));  // per thread: one lane for everything
  EXPECT_EQ(h.widget().rowLayoutOf(t401)->scopeLines, 1u);

  h.bus().setGroupByFiber(true);
  ASSERT_TRUE(h.until([&] { return h.widget().scopeLanes().grouped() && h.widget().scopeLanes().ready(); }));
  h.frames(3);
  const std::set<uint64_t> fibers401 = fibersOn(t401);
  EXPECT_TRUE(fibers401.count(1) && fibers401.count(2) && fibers401.count(3));
  EXPECT_EQ(fibersOn(h.threadOfTid(402)), (std::set<uint64_t>{10, 11}));
  EXPECT_GE(h.widget().rowLayoutOf(t401)->scopeLines, 3u);  // a line per fiber when collapsed
  // The items carry their lane; fiber 1's outermost scope is drawn under thread 401.
  std::set<uint64_t> drawnFibers;
  for (const DrawnScopeItem& it : h.widget().scopeLanes().drawnItems()) {
    if (it.thread == t401) drawnFibers.insert(h.widget().scopeLanes().set()->lanes[it.lane].fiberId);
  }
  EXPECT_TRUE(drawnFibers.count(1) && drawnFibers.count(2));

  // The fiber lane's tooltip names the fiber.
  const DrawnScopeItem* f2 = h.findItem([&](const DrawnScopeItem& i) {
    return i.thread == t401 && h.widget().scopeLanes().set()->lanes[i.lane].fiberId == 2 && i.count == 1 &&
           i.x1 - i.x0 > 3.0f;
  });
  ASSERT_NE(f2, nullptr);
  h.mouse((f2->x0 + f2->x1) * 0.5f, (f2->y0 + f2->y1) * 0.5f);
  h.frames(1);
  EXPECT_TRUE(hasLine(h.widget().lastTooltip(), "fiber: 0x2"));

  // The Call Graph asks for fiber-grouped blending too.
  ASSERT_TRUE(h.until([&] { return h.callGraph->shownSpec().groupByFiber && !h.callGraph->pending(); }));
  EXPECT_TRUE(h.callGraph->requestedSpec().groupByFiber);

  h.bus().setGroupByFiber(false);
  ASSERT_TRUE(h.until([&] { return !h.widget().scopeLanes().grouped() && h.widget().scopeLanes().ready(); }));
  EXPECT_EQ(fibersOn(t401), (std::set<uint64_t>{0}));
}

TEST(InstWidget, GroupByFiberDoesNothingWithoutFiberEvents) {
  Harness h;
  h.open("scopes.tachyon");
  h.openBelow({CallGraphPanel::kName});
  h.waitLanes();
  h.bus().setGroupByFiber(true);
  h.frames(5);
  EXPECT_FALSE(h.widget().scopeLanes().grouped());
  ASSERT_TRUE(h.until([&] { return h.callGraph->tree() != nullptr && !h.callGraph->pending(); }));
  EXPECT_FALSE(h.callGraph->requestedSpec().groupByFiber);
}

// ---- frames and plots
// ----------------------------------------------------------------------------------------

TEST(InstWidget, TheFrameGraphDrawsOneBarPerFrameAndHighlightsTheOnesOverBudget) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_TRUE(h.until([&] { return h.widget().frameGraph().ready(); }));
  h.frames(3);
  FrameGraphView& g = h.widget().frameGraph();
  ASSERT_EQ(g.data()->series.size(), 2u);
  EXPECT_EQ(g.data()->selected, 0u);  // the series with the most marks
  EXPECT_EQ(g.data()->bars.size(), 116u);
  EXPECT_EQ(g.lastBars().size(), 116u);  // wide enough for a bar per frame
  EXPECT_GT(h.widget().screenGeometry().frameGraphY1, h.widget().screenGeometry().frameGraphY0);
  const double hz = static_cast<double>(h.model().header.clock_frequency_hz);
  const uint64_t budget = msToTicks(16.67, hz);
  uint32_t expected = 0;
  for (size_t i = 0; i < g.data()->bars.size(); ++i) expected += g.data()->bars.duration(i) > budget ? 1 : 0;
  ASSERT_GE(expected, 1u);  // the hitch
  EXPECT_EQ(g.overBudgetBars(), expected);
  // The budget line lies above the normal bars and below the hitch.
  uint32_t normalTall = 0;
  float hitchTop = 1e9f;
  for (const DrawnFrameBar& b : g.lastBars()) {
    if (b.overBudget)
      hitchTop = std::min(hitchTop, b.y0);
    else
      normalTall += b.y0 >= g.lastBudgetY() - 0.5f ? 1 : 0;
  }
  EXPECT_EQ(normalTall, g.lastBars().size() - expected);
  EXPECT_LT(hitchTop, g.lastBudgetY());

  // A per-series budget override moves the line and the highlighting.
  g.budgets().set(g.data()->series[0], 60.0);
  h.frames(2);
  EXPECT_EQ(g.overBudgetBars(), 0u);
  g.budgets().set(g.data()->series[0], 1.0);
  h.frames(2);
  EXPECT_EQ(g.overBudgetBars(), g.lastBars().size());
}

TEST(InstWidget, ClickingAFrameBarZoomsTheTimelineToThatFrame) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_TRUE(h.until([&] { return h.widget().frameGraph().ready(); }));
  h.frames(3);
  FrameGraphView& g = h.widget().frameGraph();
  const FrameBars& bars = g.data()->bars;
  size_t hitch = 0;  // the slowest frame
  for (size_t i = 0; i < bars.size(); ++i) hitch = bars.duration(i) > bars.duration(hitch) ? i : hitch;
  ASSERT_GT(bars.duration(hitch), 2 * bars.duration(0));
  const DrawnFrameBar* bar = nullptr;
  for (const DrawnFrameBar& b : g.lastBars())
    if (b.tallest == hitch) bar = &b;
  ASSERT_NE(bar, nullptr);
  const uint64_t before = h.bus().state().navigation.serial;
  const TimeView fitted = h.widget().view();
  h.click((bar->x0 + bar->x1) * 0.5f, (bar->y0 + bar->y1) * 0.5f);
  h.frames(2);
  const NavigationRequest nav = h.bus().state().navigation;
  EXPECT_GT(nav.serial, before);
  EXPECT_EQ(nav.t0, bars.start[hitch]);
  EXPECT_EQ(nav.t1, bars.end[hitch]);
  EXPECT_EQ(nav.thread, bars.series.thread);
  // The timeline zoomed to the frame (with its 10 % margins).
  const TimeView v = h.widget().view();
  const double frame = static_cast<double>(bars.duration(hitch));
  EXPECT_LT(v.span, fitted.span * 0.1);
  EXPECT_GE(v.span, frame);
  EXPECT_LE(v.span, frame * 1.5);
  const double rel = static_cast<double>(bars.start[hitch] - h.widget().limits().traceStart);
  EXPECT_LE(v.start, rel);
  EXPECT_GE(v.end(), rel + frame);
}

TEST(InstWidget, JumpToTheSlowestFrameInTheFilter) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_TRUE(h.until([&] { return h.widget().frameGraph().ready(); }));
  h.frames(3);
  FrameGraphView& g = h.widget().frameGraph();
  const FrameBars& bars = g.data()->bars;
  ASSERT_TRUE(g.slowestButton().drawn);
  const float bx = (g.slowestButton().x0 + g.slowestButton().x1) * 0.5f;
  const float by = (g.slowestButton().y0 + g.slowestButton().y1) * 0.5f;

  // Without a filter: the hitch frame.
  size_t hitch = 0;
  for (size_t i = 0; i < bars.size(); ++i) hitch = bars.duration(i) > bars.duration(hitch) ? i : hitch;
  uint64_t serial = h.bus().state().navigation.serial;
  h.click(bx, by);
  ASSERT_TRUE(h.until([&] { return h.bus().state().navigation.serial != serial; }));
  EXPECT_EQ(h.bus().state().navigation.t0, bars.start[hitch]);
  EXPECT_EQ(h.bus().state().navigation.t1, bars.end[hitch]);

  // A filter that leaves the hitch out: the slowest of the rest (all equal: the earliest wins).
  ViewFilter f;
  ASSERT_LT(hitch, 99u);
  f.t0 = bars.start[100];
  f.t1 = h.widget().limits().traceEnd;
  h.bus().setTimeFilter(f);
  h.frames(2);
  serial = h.bus().state().navigation.serial;
  h.click(bx, by);
  ASSERT_TRUE(h.until([&] { return h.bus().state().navigation.serial != serial; }));
  uint64_t longest = 0;
  size_t expected = 0;
  for (size_t i = 0; i < bars.size(); ++i) {
    if (bars.end[i] <= f.t0 || bars.start[i] >= f.t1) continue;
    if (bars.duration(i) > longest) {
      longest = bars.duration(i);
      expected = i;
    }
  }
  EXPECT_EQ(h.bus().state().navigation.t0, bars.start[expected]);
  EXPECT_EQ(h.bus().state().navigation.t1, bars.end[expected]);
  EXPECT_NE(expected, hitch);
}

TEST(InstWidget, TheFrameSeriesCanBeSwitched) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_TRUE(h.until([&] { return h.widget().frameGraph().ready(); }));
  FrameGraphView& g = h.widget().frameGraph();
  const uint64_t issued = g.issuedQueries();
  g.selectSeries(1);
  ASSERT_TRUE(h.until([&] { return g.data()->selected == 1 && !g.pending(); }));
  EXPECT_GT(g.issuedQueries(), issued);
  EXPECT_EQ(g.data()->bars.size(), 58u);  // RenderFrame: 59 marks
  EXPECT_EQ(g.data()->bars.series.name, g.data()->series[1].name);
  h.frames(3);
  EXPECT_EQ(g.lastBars().size(), 58u);
  // Each series has its own budget.
  g.budgets().set(g.data()->series[1], 40.0);
  EXPECT_DOUBLE_EQ(g.budgets().msFor(g.data()->series[0]), 16.67);
  g.selectSeries(0);
  ASSERT_TRUE(h.until([&] { return g.data()->selected == 0 && !g.pending(); }));
}

TEST(InstWidget, TheFrameGraphCanBeHidden) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_TRUE(h.until([&] { return h.widget().frameGraph().ready(); }));
  h.frames(2);
  EXPECT_GT(h.widget().frameStats().frameBars, 0u);
  h.widget().options().showFrameGraph = false;
  h.frames(2);
  EXPECT_EQ(h.widget().frameStats().frameBars, 0u);
  EXPECT_EQ(h.widget().screenGeometry().frameGraphY1, 0.0f);
}

TEST(InstWidget, PlotLanesAndTheMarkersLaneAreDrawnBelowTheRows) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_TRUE(h.until([&] { return h.widget().plotLanes().ready(); }));
  h.frames(3);
  PlotLanesView& p = h.widget().plotLanes();
  EXPECT_EQ(p.plotCount(), 1u);
  EXPECT_TRUE(p.hasMarkers());
  const TimelineFrameStats& s = h.widget().frameStats();
  EXPECT_EQ(s.plotLanes, 1u);
  EXPECT_GT(s.plotColumns, 10u);
  EXPECT_EQ(s.markerTicks, 1u);
  const auto range = p.lastRange(0);
  ASSERT_TRUE(range.has_value());
  const PlotSeriesIndex& series = p.index()->series[0];
  ASSERT_TRUE(series.hasFinite);
  EXPECT_LE(range->first, series.minValue);  // the whole series is inside the value axis
  EXPECT_GE(range->second, series.maxValue);
  EXPECT_LT(range->first, range->second);

  // Hovering the plot lane reads out the value under the cursor.
  const auto y = h.widget().sectionLaneCenterY(GlobalLane::Kind::Plot, 0);
  ASSERT_TRUE(y.has_value());
  const TimelineScreenGeometry& g = h.widget().screenGeometry();
  h.mouse(g.plotX0 + g.plotWidth * 0.5f, *y);
  h.frames(1);
  EXPECT_TRUE(hasLine(h.widget().lastTooltip(), "frame_ms = "));
  EXPECT_EQ(p.hover().kind, PlotHover::Kind::Plot);

  // Hovering the marker's tick gives its text.
  ASSERT_FALSE(p.lastMarkerItems().empty());
  const auto my = h.widget().sectionLaneCenterY(GlobalLane::Kind::Markers);
  ASSERT_TRUE(my.has_value());
  h.mouse(p.lastMarkerItems()[0].x, *my);
  h.frames(1);
  EXPECT_TRUE(hasLine(h.widget().lastTooltip(), "Marker: Hitch"));
  EXPECT_TRUE(hasLine(h.widget().lastTooltip(), "frame 59 takes"));
}

TEST(InstWidget, ThePlotGroupCollapsesWhenItsHeaderIsClicked) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_TRUE(h.until([&] { return h.widget().plotLanes().ready(); }));
  h.frames(3);
  PlotLanesView& p = h.widget().plotLanes();
  ASSERT_EQ(h.widget().frameStats().plotLanes, 1u);
  const auto y = h.widget().sectionLaneCenterY(GlobalLane::Kind::PlotsHeader);
  ASSERT_TRUE(y.has_value());
  const TimelineScreenGeometry& g = h.widget().screenGeometry();
  h.click(g.plotX0 + 40.0f, *y);
  h.frames(2);
  EXPECT_TRUE(p.collapsed());
  EXPECT_EQ(h.widget().frameStats().plotLanes, 0u);
  EXPECT_EQ(h.widget().frameStats().markerTicks, 1u);  // the markers lane stays
  h.click(g.plotX0 + 40.0f, *h.widget().sectionLaneCenterY(GlobalLane::Kind::PlotsHeader));
  h.frames(2);
  EXPECT_FALSE(p.collapsed());
  EXPECT_EQ(h.widget().frameStats().plotLanes, 1u);
  h.widget().options().showPlots = false;
  h.frames(2);
  EXPECT_EQ(h.widget().frameStats().plotLanes, 0u);
  EXPECT_EQ(h.widget().frameStats().markerTicks, 0u);
}

// ---- lifetime
// ------------------------------------------------------------------------------------------------

TEST(InstLifetime, ClosingTheDocumentWithThePairingInFlightIsSafe) {
  Harness h;
  for (int i = 0; i < 6; ++i) {
    h.app.openFile(fixture(i % 2 ? "fibers.tachyon" : "scopes.tachyon"));
    ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr && !h.app.loading(); }));
    h.bus().setGroupByFiber(i % 2 == 1);
    h.frames(1 + i % 3);  // the widget has issued the pairing, perhaps not finished it
    h.app.commands().run("file.close");
    h.frames(3);
    EXPECT_FALSE(h.widget().hasDocument());
  }
  // And the app is still usable.
  h.open("scopes.tachyon");
  h.waitLanes();
  EXPECT_GT(h.widget().frameStats().scopeItems, 0u);
}

TEST(InstLifetime, OpeningAnotherDocumentWhileTheFirstOneIsStillPairingDropsTheOldLanes) {
  Harness h;
  h.app.openFile(fixture("scopes.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr && !h.app.loading(); }));
  h.frames(2);
  const uint64_t generation = h.app.documentGeneration();
  h.app.openFile(fixture("frames.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.documentGeneration() != generation && !h.app.loading(); }));
  h.frames(3);
  EXPECT_FALSE(h.widget().scopeLanes().ready());  // frames.tachyon has no scopes: nothing was paired for it
  EXPECT_EQ(h.widget().frameStats().scopeItems, 0u);
  ASSERT_TRUE(h.until([&] { return h.widget().frameGraph().ready(); }));
}

TEST(InstLifetime, TheTimelineSurvivesADocumentWithAnIndexingJobStillRunning) {
  Harness h;
  h.app.openFile(fixture("frames.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr && !h.app.loading(); }));
  h.frames(1);  // the plot index job and the frame query were started
  h.app.commands().run("file.close");
  h.frames(3);
  EXPECT_FALSE(h.widget().hasDocument());
  EXPECT_FALSE(h.widget().plotLanes().ready());
}
