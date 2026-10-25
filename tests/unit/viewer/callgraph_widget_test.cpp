// WP-43: the Call Graph panel driven headless (Null renderer, real ImGui frames, injected input) on the tiny
// and medium fixtures: results arrive, selection <-> bus, context menu actions, filter changes, symbol
// generation changes, option toggles, the subtree resolver and a 100 000 deep tree.
#include "analysis/query.h"
#include "analysis/query_executor.h"
#include "viewer/analysis_context.h"
#include "viewer/app.h"
#include "viewer/callgraph/callgraph_panel.h"
#include "viewer/callgraph/subtree_resolver.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace cg = tachyon::viewer::callgraph;
namespace fs = std::filesystem;
using analysis::NodeAgg;
using symbols::FunctionKey;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

AppOptions headlessOptions() {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_callgraph_test_no_such_settings.json";
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
    PanelEntry* entry = app.panels().find(CallGraphPanel::kName);
    EXPECT_NE(entry, nullptr);
    panel = entry ? dynamic_cast<CallGraphPanel*>(entry->panel.get()) : nullptr;
    EXPECT_NE(panel, nullptr);
    for (PanelEntry& e : app.panels().entries()) {
      if (e.panel.get() != panel) e.open = false;  // only the call graph on screen
    }
  }

  SelectionBus& bus() { return app.selection(); }
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

  // The symbols are resolved and the panel shows the tree of the current state.
  bool settled() {
    const symbols::SymbolService* svc = app.document()->symbols().get();
    const symbols::SymbolProgress p = svc->progress();
    return p.total > 0 && p.done == p.total && !panel->pending() && panel->tree() &&
           panel->requestedSpec().symbolGeneration == app.symbolGeneration() &&
           panel->shownSpec() == panel->requestedSpec();
  }
  void open(const char* name) {
    app.openFile(fixture(name));
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    // Module events reach the panel through the app's mailbox: wait until the symbol generation is stable.
    stable();
  }
  // Waits for the symbols and for a symbol generation that stays put for 100 ms, then for the panel.
  void stable() {
    for (;;) {
      ASSERT_TRUE(until([&] { return settled(); }));
      const uint64_t generation = app.symbolGeneration();
      const auto until100 = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (std::chrono::steady_clock::now() < until100) {
        frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (generation == app.symbolGeneration()) break;
    }
    ASSERT_TRUE(until([&] { return settled(); }));
  }
  // After a change of the bus filter or the options: lets the panel notice it, then waits for its result.
  void settle() {
    frames(2);
    ASSERT_TRUE(until([&] { return settled(); }));
    frames(2);
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

  // Independent reference: the same query on a context of the test's own.
  Result<NodeAgg> reference(const analysis::Filter& filter, analysis::AggregationOptions options = {}) {
    analysis::QueryContext ctx(model(), *app.document()->symbols(), options);
    return analysis::topDownTree(ctx, filter);
  }

  size_t rowNamed(const std::string& function) {
    for (size_t i = 0; i < panel->rows().size(); ++i)
      if (panel->nameOf(panel->rows()[i].node->key).function == function) return i;
    return SIZE_MAX;
  }
  // Breadth-first search of the whole tree for a function name (test side, independent of the panel code).
  const NodeAgg* nodeNamed(const std::string& function) {
    std::vector<const NodeAgg*> queue{panel->tree().get()};
    for (size_t h = 0; h < queue.size(); ++h) {
      if (panel->nameOf(queue[h]->key).function == function) return queue[h];
      for (const NodeAgg& c : queue[h]->children) queue.push_back(&c);
    }
    return nullptr;
  }

  void mouse(float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    frames(1);
  }
  void click(float x, float y, int button = 0) {
    mouse(x, y);
    ImGui::GetIO().AddMouseButtonEvent(button, true);
    frames(1);
    ImGui::GetIO().AddMouseButtonEvent(button, false);
    frames(1);
  }
  // Centre of the Function cell of a drawn row, right of its arrow.
  ImVec2 cellOf(size_t row) {
    for (const CallGraphPanel::DrawnRow& d : panel->drawnRows())
      if (d.row == row) return ImVec2(d.arrowX1 + 40.0f, (d.y0 + d.y1) * 0.5f);
    ADD_FAILURE() << "row " << row << " is not on screen";
    return ImVec2(0, 0);
  }

  App app;
  CallGraphPanel* panel = nullptr;
};

NodeAgg deepChain(size_t depth) {
  NodeAgg cur;
  cur.key = FunctionKey{ModuleIdx(0), depth};
  cur.inclusiveTicks = cur.exclusiveTicks = 100;
  cur.sampleCount = 1;
  for (size_t i = depth; i-- > 1;) {
    NodeAgg parent;
    parent.key = FunctionKey{ModuleIdx(0), i};
    parent.inclusiveTicks = 100;
    parent.sampleCount = 1;
    parent.children.push_back(std::move(cur));
    cur = std::move(parent);
  }
  NodeAgg root;
  root.key = analysis::kRootKey;
  root.inclusiveTicks = 100;
  root.sampleCount = 1;
  root.children.push_back(std::move(cur));
  return root;
}

}  // namespace

TEST(CallGraphWidget, ResultsArriveAndMatchTheQuery) {
  Harness h;
  h.open("tiny.tachyon");
  const auto ref = h.reference(analysis::Filter{});
  ASSERT_TRUE(ref.ok());
  ASSERT_TRUE(h.panel->tree());
  const NodeAgg& root = *h.panel->tree();
  EXPECT_EQ(root.key, analysis::kRootKey);
  EXPECT_EQ(root.inclusiveTicks, ref->inclusiveTicks);
  EXPECT_EQ(root.sampleCount, ref->sampleCount);
  EXPECT_EQ(root.children.size(), ref->children.size());
  EXPECT_GT(root.inclusiveTicks, 0u);

  // The hot path is expanded by default: root, the thread start, main, then the heaviest children.
  ASSERT_GE(h.panel->rows().size(), 4u);
  EXPECT_EQ(h.panel->nameOf(h.panel->rows()[0].node->key).function, "<root>");
  const auto& start = h.panel->nameOf(h.panel->rows()[1].node->key);
  EXPECT_EQ(start.function, "RtlUserThreadStart");
  EXPECT_EQ(start.module, "ntdll.dll");
  EXPECT_NE(h.rowNamed("main"), SIZE_MAX);
  EXPECT_NE(h.rowNamed("Engine::update"), SIZE_MAX);
  // The rows are on screen (virtualized): the first ones were drawn.
  EXPECT_FALSE(h.panel->drawnRows().empty());
  EXPECT_EQ(h.panel->message(), "");
  // Pie of the root: its children.
  EXPECT_FALSE(h.panel->pie().empty());
}

TEST(CallGraphWidget, ClickingARowSelectsItsFunctionOnTheBus) {
  Harness h;
  h.open("tiny.tachyon");
  const size_t row = h.rowNamed("main");
  ASSERT_NE(row, SIZE_MAX);
  const ImVec2 p = h.cellOf(row);
  h.click(p.x, p.y);
  const FunctionKey key = h.panel->rows()[row].node->key;
  ASSERT_TRUE(h.bus().state().selectedFunction);
  EXPECT_EQ(*h.bus().state().selectedFunction, key);
  EXPECT_EQ(h.panel->selectedId(), h.panel->rows()[row].id);
  // The pie shows the children of the selected node.
  ASSERT_FALSE(h.panel->pie().empty());
  EXPECT_EQ(h.panel->pie().size(), h.panel->rows()[row].node->children.size());

  // Clicking the arrow of an expanded row collapses it without selecting something else.
  const size_t startRow = h.rowNamed("RtlUserThreadStart");
  ASSERT_NE(startRow, SIZE_MAX);
  const size_t before = h.panel->rows().size();
  const CallGraphPanel::DrawnRow* d = nullptr;
  for (const auto& r : h.panel->drawnRows())
    if (r.row == startRow) d = &r;
  ASSERT_NE(d, nullptr);
  h.click(d->arrowX1 - 4.0f, (d->y0 + d->y1) * 0.5f);
  EXPECT_LT(h.panel->rows().size(), before);
  EXPECT_EQ(*h.bus().state().selectedFunction, key);
}

TEST(CallGraphWidget, DoubleClickShowsTheSource) {
  Harness h;
  h.open("tiny.tachyon");
  const size_t row = h.rowNamed("Engine::update");
  ASSERT_NE(row, SIZE_MAX);
  const ImVec2 p = h.cellOf(row);
  const uint64_t serial = h.bus().state().panelRequest.serial;
  h.mouse(p.x, p.y);
  for (int i = 0; i < 2; ++i) {
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    h.frames(1);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    h.frames(1);
  }
  EXPECT_GT(h.bus().state().panelRequest.serial, serial);
  EXPECT_EQ(h.bus().state().panelRequest.name, CallGraphPanel::kSourcePanel);
  ASSERT_TRUE(h.bus().state().selectedFunction);
  EXPECT_EQ(*h.bus().state().selectedFunction, h.panel->rows()[row].node->key);
}

TEST(CallGraphWidget, RightClickSelectsAndOpensTheMenuWithTheRowAsTarget) {
  Harness h;
  h.open("tiny.tachyon");
  const size_t row = h.rowNamed("Engine::update");
  ASSERT_NE(row, SIZE_MAX);
  const ImVec2 p = h.cellOf(row);
  h.click(p.x, p.y, 1);
  ASSERT_TRUE(h.panel->menuTarget());
  EXPECT_EQ(h.panel->menuTarget()->key, h.panel->rows()[row].node->key);
  EXPECT_EQ(h.panel->menuTarget()->fullName, "game.exe!Engine::update");
  EXPECT_FALSE(h.panel->menuTarget()->isRoot);
  EXPECT_EQ(h.panel->selectedId(), h.panel->rows()[row].id);
  EXPECT_TRUE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
}

TEST(CallGraphWidget, MenuActionsPublishOnTheBus) {
  Harness h;
  h.open("tiny.tachyon");
  const PanelContext ctx = h.context();
  const size_t row = h.rowNamed("Engine::update");
  ASSERT_NE(row, SIZE_MAX);
  const auto target = h.panel->targetOfRow(row);
  ASSERT_TRUE(target);

  uint64_t serial = h.bus().state().panelRequest.serial;
  h.panel->showInFunctionList(ctx, *target);
  EXPECT_GT(h.bus().state().panelRequest.serial, serial);
  EXPECT_EQ(h.bus().state().panelRequest.name, "Function List");
  ASSERT_TRUE(h.bus().state().selectedFunction);
  EXPECT_EQ(*h.bus().state().selectedFunction, target->key);

  h.bus().setSelectedFunction(std::nullopt);
  serial = h.bus().state().panelRequest.serial;
  h.panel->showSource(ctx, *target);
  EXPECT_GT(h.bus().state().panelRequest.serial, serial);
  EXPECT_EQ(h.bus().state().panelRequest.name, "Source");
  EXPECT_EQ(*h.bus().state().selectedFunction, target->key);

  h.panel->copyName(*target);
  EXPECT_STREQ(ImGui::GetClipboardText(), "game.exe!Engine::update");

  // Nothing happens for the root row.
  const auto rootTarget = h.panel->targetOfRow(0);
  ASSERT_TRUE(rootTarget);
  EXPECT_TRUE(rootTarget->isRoot);
  serial = h.bus().state().panelRequest.serial;
  h.panel->showSource(ctx, *rootTarget);
  EXPECT_EQ(h.bus().state().panelRequest.serial, serial);
  EXPECT_FALSE(h.panel->filterToSubtree(ctx, *rootTarget));
}

TEST(CallGraphWidget, FilterToSubtreeRequeriesAndClears) {
  Harness h;
  h.open("tiny.tachyon");
  const PanelContext ctx = h.context();
  const size_t row = h.rowNamed("Engine::update");
  ASSERT_NE(row, SIZE_MAX);
  const NodeAgg* updateNode = h.panel->rows()[row].node;
  const uint64_t updateTicks = updateNode->inclusiveTicks;
  const uint32_t updateSamples = updateNode->sampleCount;
  const uint64_t queries = h.panel->issuedQueries();

  ASSERT_TRUE(h.panel->filterToSubtree(ctx, *h.panel->targetOfRow(row)));
  ASSERT_TRUE(h.bus().state().timeFilter);
  ASSERT_TRUE(h.bus().state().timeFilter->subtreeRoot);
  EXPECT_TRUE(h.bus().state().timeFilter->subtreeRoot->valid());
  // The new tree is rooted at the function and carries exactly that subtree's samples.
  h.settle();
  EXPECT_GT(h.panel->issuedQueries(), queries);
  const NodeAgg& root = *h.panel->tree();
  EXPECT_EQ(h.panel->nameOf(root.key).function, "Engine::update");
  // One callstack node stands for the tree node (nodes merge by function; here Engine::update is reached
  // through two different call sites), so the filtered tree is part of it.
  EXPECT_GT(root.inclusiveTicks, 0u);
  EXPECT_LE(root.inclusiveTicks, updateTicks);
  EXPECT_LE(root.sampleCount, updateSamples);

  // Filtering again from inside the subtree builds on the current root.
  const size_t physics = h.rowNamed("Engine::physics");
  if (physics != SIZE_MAX) {
    const auto before = h.bus().state().timeFilter->subtreeRoot;
    ASSERT_TRUE(h.panel->filterToSubtree(ctx, *h.panel->targetOfRow(physics)));
    EXPECT_NE(h.bus().state().timeFilter->subtreeRoot, before);
    h.settle();
    EXPECT_EQ(h.panel->nameOf(h.panel->tree()->key).function, "Engine::physics");
  }

  h.panel->clearSubtreeFilter(ctx);
  EXPECT_FALSE(h.bus().state().timeFilter);  // whole range, nothing else restricted: no filter at all
  h.settle();
  EXPECT_EQ(h.panel->tree()->key, analysis::kRootKey);
  EXPECT_EQ(h.panel->tree()->sampleCount, h.reference(analysis::Filter{})->sampleCount);
}

TEST(CallGraphWidget, FilterToSubtreeKeepsTheTimeRangeOfTheBusFilter) {
  Harness h;
  h.open("tiny.tachyon");
  const model::TraceModel& m = h.model();
  ViewFilter range;
  range.t0 = m.header.start_ts;
  range.t1 = m.header.start_ts + (m.header.end_ts - m.header.start_ts) / 2;
  h.bus().setTimeFilter(range);
  h.settle();
  const PanelContext ctx = h.context();
  const size_t row = h.rowNamed("Engine::update");
  ASSERT_NE(row, SIZE_MAX);
  ASSERT_TRUE(h.panel->filterToSubtree(ctx, *h.panel->targetOfRow(row)));
  const ViewFilter f = *h.bus().state().timeFilter;
  EXPECT_EQ(f.t0, range.t0);
  EXPECT_EQ(f.t1, range.t1);
  ASSERT_TRUE(f.subtreeRoot);
  h.settle();
  // Clearing the subtree keeps the range filter.
  h.panel->clearSubtreeFilter(ctx);
  ASSERT_TRUE(h.bus().state().timeFilter);
  EXPECT_FALSE(h.bus().state().timeFilter->subtreeRoot);
  EXPECT_EQ(h.bus().state().timeFilter->t1, range.t1);
}

TEST(CallGraphWidget, ExternalSelectionExpandsScrollsAndHighlights) {
  Harness h;
  h.open("tiny.tachyon");
  const NodeAgg* draw = h.nodeNamed("Render::draw");
  ASSERT_NE(draw, nullptr);
  h.panel->collapseAllRows();
  EXPECT_EQ(h.panel->rows().size(), 1u + h.panel->tree()->children.size());  // the root stays expanded
  EXPECT_EQ(h.panel->rowOfId(cg::kRootId), 0u);

  h.bus().setSelectedFunction(draw->key);
  h.frames(3);
  ASSERT_TRUE(h.panel->selectedId());
  const size_t row = h.panel->rowOfId(*h.panel->selectedId());
  ASSERT_NE(row, SIZE_MAX);
  EXPECT_EQ(h.panel->rows()[row].node, draw);
  EXPECT_EQ(h.panel->selectedKey(), draw->key);
  // The selection came from the bus: nothing was republished.
  EXPECT_EQ(*h.bus().state().selectedFunction, draw->key);
  bool drawn = false;
  for (const auto& d : h.panel->drawnRows()) drawn |= d.row == row;
  EXPECT_TRUE(drawn);

  // A function that is not in the tree clears the highlight.
  h.bus().setSelectedFunction(FunctionKey{ModuleIdx(0), 0x7777});
  h.frames(3);
  EXPECT_FALSE(h.panel->selectedId());
  h.bus().setSelectedFunction(std::nullopt);
  h.frames(2);
  EXPECT_FALSE(h.panel->selectedKey());
}

TEST(CallGraphWidget, ExpandHotPathStartsAtTheSelectionAndExpandAllCollapseAllWork) {
  Harness h;
  h.open("tiny.tachyon");
  h.panel->collapseAllRows();
  ASSERT_EQ(h.panel->rows().size(), 1u + h.panel->tree()->children.size());
  const PanelContext ctx = h.context();
  h.panel->expandHotPath();  // from the root
  const size_t afterRoot = h.panel->rows().size();
  EXPECT_GT(afterRoot, 3u);

  h.panel->collapseAllRows();
  h.panel->expandAllRows();
  const size_t all = h.panel->rows().size();
  EXPECT_GE(all, afterRoot);
  size_t nodes = 0;
  std::vector<const NodeAgg*> queue{h.panel->tree().get()};
  for (size_t i = 0; i < queue.size(); ++i) {
    ++nodes;
    for (const NodeAgg& c : queue[i]->children) queue.push_back(&c);
  }
  EXPECT_EQ(all, nodes);

  // Hot path from a selected node expands below it (and keeps its ancestors visible).
  h.panel->collapseAllRows();
  h.panel->toggleExpanded(0);
  h.panel->expandHotPath();
  ASSERT_GT(h.panel->rows().size(), 2u);
  h.panel->collapseAllRows();
  const NodeAgg* update = h.nodeNamed("Engine::update");
  ASSERT_NE(update, nullptr);
  h.bus().setSelectedFunction(update->key);
  h.frames(2);
  h.panel->collapseAllRows();
  h.panel->selectRow(ctx, 0);  // root: no bus change
  h.bus().setSelectedFunction(update->key);
  h.frames(2);
  ASSERT_TRUE(h.panel->selectedId());
  h.panel->expandHotPath();
  EXPECT_NE(h.panel->rowOfId(*h.panel->selectedId()), SIZE_MAX);
}

TEST(CallGraphWidget, PercentModeChangesTheColumnBase) {
  Harness h;
  h.open("tiny.tachyon");
  h.panel->setPercentMode(cg::PercentMode::OfParent);
  EXPECT_EQ(h.panel->percentMode(), cg::PercentMode::OfParent);
  h.frames(2);  // draws with the other mode without trouble
  h.panel->setPercentMode(cg::PercentMode::OfRoot);
  h.frames(1);
}

TEST(CallGraphWidget, FilterChangeRequeriesAndShowsTheNewResult) {
  Harness h;
  h.open("tiny.tachyon");
  const model::TraceModel& m = h.model();
  const uint64_t queries = h.panel->issuedQueries();
  const uint64_t fullSamples = h.panel->tree()->sampleCount;

  ViewFilter half;
  half.t0 = m.header.start_ts;
  half.t1 = m.header.start_ts + (m.header.end_ts - m.header.start_ts) / 3;
  h.bus().setTimeFilter(half);
  h.frames(1);
  EXPECT_GT(h.panel->issuedQueries(), queries);
  // While the query runs the previous tree stays (greyed); it never disappears.
  EXPECT_TRUE(h.panel->tree());
  h.settle();
  const analysis::Filter expected = cg::toQueryFilter(half);
  const auto ref = h.reference(expected);
  ASSERT_TRUE(ref.ok());
  EXPECT_EQ(h.panel->tree()->inclusiveTicks, ref->inclusiveTicks);
  EXPECT_EQ(h.panel->tree()->sampleCount, ref->sampleCount);
  EXPECT_LT(h.panel->tree()->sampleCount, fullSamples);

  // Thread restriction.
  ViewFilter one = half;
  one.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  h.bus().setTimeFilter(one);
  h.settle();
  EXPECT_EQ(h.panel->tree()->sampleCount, h.reference(cg::toQueryFilter(one))->sampleCount);

  // Back to the whole trace.
  h.bus().setTimeFilter(std::nullopt);
  h.settle();
  EXPECT_EQ(h.panel->tree()->sampleCount, fullSamples);
}

TEST(CallGraphWidget, OnlyTheNewestOfQuicklySucceedingFiltersIsShown) {
  Harness h;
  h.open("medium.tachyon");
  const model::TraceModel& m = h.model();
  const PanelContext ctx = h.context();
  const uint64_t span = m.header.end_ts - m.header.start_ts;
  const uint64_t delivered = h.panel->deliveredTrees();
  ViewFilter f;
  for (int i = 1; i <= 6; ++i) {  // six requests without a frame in between: each supersedes the last
    f.t0 = m.header.start_ts;
    f.t1 = m.header.start_ts + span * static_cast<uint64_t>(i) / 8;
    h.bus().setTimeFilter(f);
    h.panel->update(ctx);
  }
  h.settle();
  const auto ref = h.reference(cg::toQueryFilter(f));
  ASSERT_TRUE(ref.ok());
  EXPECT_EQ(h.panel->tree()->sampleCount, ref->sampleCount);
  EXPECT_EQ(h.panel->shownSpec().filter, cg::toQueryFilter(f));
  // At most a few intermediate trees were delivered; the last one is the one that stays.
  EXPECT_GE(h.panel->deliveredTrees(), delivered + 1);
  EXPECT_LE(h.panel->deliveredTrees(), delivered + 6);
}

TEST(CallGraphWidget, SymbolGenerationChangeRekeysTheQueryContext) {
  Harness h;
  h.open("tiny.tachyon");
  const AnalysisContext& analysis = h.app.document()->analysis();
  EXPECT_EQ(analysis.snapshotCount(), 1u);
  const uint64_t queries = h.panel->issuedQueries();
  PanelContext ctx = h.context();
  ctx.symbolGeneration += 1;  // as if a module had resolved
  h.panel->update(ctx);
  EXPECT_EQ(h.panel->issuedQueries(), queries + 1);
  EXPECT_EQ(h.panel->requestedSpec().symbolGeneration, ctx.symbolGeneration);
  // Poll with the artificial context only (a frame would update with the app's real generation).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    h.panel->update(ctx);
    if (!h.panel->pending() && h.panel->shownSpec() == h.panel->requestedSpec()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(!h.panel->pending() && h.panel->shownSpec() == h.panel->requestedSpec());
  EXPECT_EQ(analysis.snapshotCount(), 1u);  // the old generation's context was released
  EXPECT_EQ(h.panel->shownSpec().symbolGeneration, ctx.symbolGeneration);
  EXPECT_TRUE(h.panel->tree());
}

TEST(CallGraphWidget, OptionTogglesRequeryOnTheirOwnContext) {
  Harness h;
  h.open("tiny.tachyon");
  const AnalysisContext& analysis = h.app.document()->analysis();
  EXPECT_EQ(analysis.snapshotCount(), 1u);
  const uint64_t queries = h.panel->issuedQueries();

  h.panel->setInlineExpansion(true);
  h.frames(1);
  EXPECT_GT(h.panel->issuedQueries(), queries);
  h.settle();
  EXPECT_EQ(analysis.snapshotCount(), 2u);  // inline-expanded contexts are separate
  EXPECT_TRUE(h.panel->requestedSpec().expandInline);
  const auto inlineRef =
      h.reference(analysis::Filter{}, analysis::AggregationOptions{true, analysis::KernelFrames::Show, 0});
  ASSERT_TRUE(inlineRef.ok());
  EXPECT_EQ(h.panel->tree()->inclusiveTicks, inlineRef->inclusiveTicks);

  h.panel->setKernelFrames(analysis::KernelFrames::Group);
  h.settle();
  EXPECT_EQ(analysis.snapshotCount(), 3u);
  h.panel->setInlineExpansion(false);
  h.panel->setKernelFrames(analysis::KernelFrames::Show);
  h.settle();
  EXPECT_EQ(h.panel->tree()->inclusiveTicks, h.reference(analysis::Filter{})->inclusiveTicks);
  EXPECT_EQ(analysis.snapshotCount(), 3u);  // the first context was found again, not rebuilt
}

TEST(CallGraphWidget, ClosingTheDocumentDropsTheTreeAndAnInFlightQuery) {
  Harness h;
  h.open("medium.tachyon");
  ASSERT_TRUE(h.panel->tree());
  ViewFilter f;
  f.t0 = h.model().header.start_ts;
  f.t1 = f.t0 + 1000;
  h.bus().setTimeFilter(f);
  h.frames(1);  // a query may be in flight now
  h.app.commands().run("file.close");
  h.frames(3);
  EXPECT_EQ(h.app.document(), nullptr);
  EXPECT_FALSE(h.panel->tree());
  EXPECT_TRUE(h.panel->rows().empty());
  EXPECT_FALSE(h.panel->pending());
  // And a new document starts from a clean slate.
  h.app.openFile(fixture("tiny.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr && !h.app.loading(); }));
  ASSERT_TRUE(h.until([&] { return h.settled(); }));
  EXPECT_EQ(h.panel->tree()->key, analysis::kRootKey);
}

TEST(CallGraphWidget, AHundredThousandDeepTreeIsDrawnWithoutRecursion) {
  Harness h;
  h.open("tiny.tachyon");
  const auto snapshot = h.app.document()->analysis().snapshot(
      analysis::AggregationOptions{}, h.app.symbolGeneration(), &h.app.queryExecutor());
  ASSERT_TRUE(snapshot);
  constexpr size_t kDepth = 100'000;
  h.panel->injectTree(std::make_shared<const NodeAgg>(deepChain(kDepth)), snapshot, 1e7);
  EXPECT_EQ(h.panel->rows().size(), cg::kMaxVisibleRows);  // the hot path expands the whole chain: capped
  EXPECT_TRUE(h.panel->rowCapReached());
  h.frames(3);
  EXPECT_FALSE(h.panel->drawnRows().empty());

  h.panel->collapseAllRows();
  EXPECT_FALSE(h.panel->expandAllRows());  // cap reached
  EXPECT_EQ(h.panel->rows().size(), cg::kMaxVisibleRows);
  EXPECT_TRUE(h.panel->rowCapReached());
  const PanelContext ctx = h.context();
  h.panel->selectRow(ctx, cg::kMaxVisibleRows - 1);
  h.frames(2);
  const auto target = h.panel->targetOfRow(cg::kMaxVisibleRows - 1);
  ASSERT_TRUE(target);
  EXPECT_EQ(target->path.size(), cg::kMaxVisibleRows - 1);
  h.bus().setSelectedFunction(FunctionKey{ModuleIdx(0), kDepth});  // the deepest node, found breadth first
  h.frames(3);
  ASSERT_TRUE(h.panel->selectedId());
  EXPECT_EQ(h.panel->selectedKey(), (FunctionKey{ModuleIdx(0), kDepth}));
  h.panel->injectTree(nullptr, nullptr, 1e7);  // destroys the deep tree
}

// ---- the subtree resolver ---------------------------------------------------------------------------------

namespace {

struct PathNode {
  const NodeAgg* node;
  std::vector<FunctionKey> path;
};

std::vector<PathNode> collect(const NodeAgg& root, size_t limit) {
  std::vector<PathNode> all;
  std::vector<PathNode> queue;
  queue.push_back({&root, {}});
  for (size_t h = 0; h < queue.size() && all.size() < limit; ++h) {
    all.push_back(queue[h]);
    for (const NodeAgg& c : queue[h].node->children) {
      PathNode child{&c, queue[h].path};
      child.path.push_back(c.key);
      queue.push_back(std::move(child));
    }
  }
  return all;
}

}  // namespace

TEST(CallGraphWidget, EveryNodeOfTinyResolvesToACallstackNodeOfTheSameFunction) {
  Harness h;
  h.open("tiny.tachyon");
  const AnalysisContext& analysis = h.app.document()->analysis();
  for (const bool inl : {false, true}) {
    for (const auto kernel :
         {analysis::KernelFrames::Show, analysis::KernelFrames::Group, analysis::KernelFrames::Hide}) {
      analysis::AggregationOptions options;
      options.expandInline = inl;
      options.kernelFrames = kernel;
      const auto snapshot = analysis.snapshot(options, h.app.symbolGeneration(), &h.app.queryExecutor());
      ASSERT_TRUE(snapshot);
      const auto tree = analysis::topDownTreeShared(snapshot->context(), analysis::Filter{});
      ASSERT_TRUE(tree.ok());
      const auto children = analysis.childIndex();
      for (const PathNode& n : collect(**tree, 1000)) {
        if (n.path.empty()) continue;
        const auto at = callgraph::resolveNodePath(snapshot->context(), *children, CallstackIdx(), n.path);
        ASSERT_TRUE(at) << "inline=" << inl << " kernel=" << static_cast<int>(kernel)
                        << " depth=" << n.path.size();
        analysis::Filter f;
        f.subtreeRoot = *at;
        const auto sub = analysis::topDownTree(snapshot->context(), f);
        ASSERT_TRUE(sub.ok());
        EXPECT_EQ(sub->key, n.node->key);
        EXPECT_LE(sub->inclusiveTicks,
                  n.node->inclusiveTicks);  // one callstack node of possibly several merged
        EXPECT_GT(sub->sampleCount, 0u);
      }
    }
  }
}

TEST(CallGraphWidget, NodesOfMediumResolveToASubtreeThatIsPartOfTheNode) {
  Harness h;
  h.open("medium.tachyon");
  const AnalysisContext& analysis = h.app.document()->analysis();
  const auto snapshot =
      analysis.snapshot(analysis::AggregationOptions{}, h.app.symbolGeneration(), &h.app.queryExecutor());
  ASSERT_TRUE(snapshot);
  const auto tree = analysis::topDownTreeShared(snapshot->context(), analysis::Filter{});
  ASSERT_TRUE(tree.ok());
  const auto children = analysis.childIndex();
  size_t checked = 0;
  for (const PathNode& n : collect(**tree, 400)) {
    if (n.path.empty()) continue;
    const auto at = callgraph::resolveNodePath(snapshot->context(), *children, CallstackIdx(), n.path);
    ASSERT_TRUE(at) << "depth " << n.path.size();
    analysis::Filter f;
    f.subtreeRoot = *at;
    const auto sub = analysis::topDownTree(snapshot->context(), f);
    ASSERT_TRUE(sub.ok());
    EXPECT_EQ(sub->key, n.node->key);
    EXPECT_LE(sub->inclusiveTicks, n.node->inclusiveTicks);  // one model node of possibly several merged ones
    EXPECT_GT(sub->sampleCount, 0u);
    ++checked;
  }
  EXPECT_GT(checked, 20u);
  // A path that does not exist.
  std::vector<FunctionKey> bogus{FunctionKey{ModuleIdx(0), 0xdeadbeef}};
  EXPECT_FALSE(callgraph::resolveNodePath(snapshot->context(), *children, CallstackIdx(), bogus));
  EXPECT_EQ(callgraph::resolveNodePath(snapshot->context(), *children, CallstackIdx(5), {}), CallstackIdx(5));
}
