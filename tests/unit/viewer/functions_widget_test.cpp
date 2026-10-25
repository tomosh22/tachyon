// WP-44: the Function List and Butterfly panels driven headless (Null renderer, real ImGui frames, injected
// input) on the tiny and medium fixtures: rows equal bottomUpFunctions, the butterfly equals
// analysis::butterfly, selection sync between Function List, Butterfly and Call Graph in both directions,
// the subtree toggle, grouping, stale results, symbol generation changes and the context menu.
#include "analysis/query.h"
#include "analysis/query_executor.h"
#include "viewer/analysis_context.h"
#include "viewer/app.h"
#include "viewer/callgraph/callgraph_panel.h"
#include "viewer/functions/butterfly_panel.h"
#include "viewer/functions/function_list_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fl = tachyon::viewer::functions;
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
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_functions_test_no_such_settings.json";
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
    list = find<FunctionListPanel>(FunctionListPanel::kName);
    butterfly = find<ButterflyPanel>(ButterflyPanel::kName);
    graph = find<CallGraphPanel>(CallGraphPanel::kName);
    for (PanelEntry& e : app.panels().entries()) {
      const IPanel* p = e.panel.get();
      e.open = p == list || p == butterfly || p == graph;  // only the three analysis panels on screen
    }
  }

  template <typename T>
  T* find(const char* name) {
    PanelEntry* entry = app.panels().find(name);
    EXPECT_NE(entry, nullptr) << name;
    T* panel = entry ? dynamic_cast<T*>(entry->panel.get()) : nullptr;
    EXPECT_NE(panel, nullptr) << name;
    return panel;
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

  bool listSettled() {
    return !list->pending() && list->table() &&
           list->requestedSpec().symbolGeneration == app.symbolGeneration() &&
           list->shownSpec() == list->requestedSpec();
  }
  bool butterflySettled() {
    const auto& key = bus().state().selectedFunction;
    if (!key) return !butterfly->pending();
    return !butterfly->pending() && butterfly->data() && butterfly->shownSpec().key == key &&
           butterfly->requestedSpec().base.symbolGeneration == app.symbolGeneration() &&
           butterfly->shownSpec() == butterfly->requestedSpec();
  }
  bool graphSettled() {
    return !graph->pending() && graph->tree() && graph->shownSpec() == graph->requestedSpec() &&
           graph->requestedSpec().symbolGeneration == app.symbolGeneration();
  }
  bool settled() {
    const symbols::SymbolService* svc = app.document()->symbols().get();
    const symbols::SymbolProgress p = svc->progress();
    return p.total > 0 && p.done == p.total && listSettled() && butterflySettled() && graphSettled();
  }
  void open(const char* name) {
    app.openFile(fixture(name));
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    stable();
  }
  // Waits for the symbols and for a symbol generation that stays put for 100 ms, then for the panels.
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
  // After a change of the bus or an option: lets the panels notice it, then waits for their results.
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

  // Independent references on a context of the test's own.
  std::unique_ptr<analysis::QueryContext> refContext(analysis::AggregationOptions options = {}) {
    return std::make_unique<analysis::QueryContext>(model(), *app.document()->symbols(), options);
  }

  uint32_t entryNamed(const std::string& function) {
    const fl::Table& t = *list->table();
    for (size_t i = 0; i < t.entries.size(); ++i)
      if (t.entries[i].function == function) return static_cast<uint32_t>(i);
    ADD_FAILURE() << "no function " << function;
    return 0;
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
  // Places the windows side by side so that clicks hit the intended panel.
  void layout() {
    frames(2);
    ImGui::SetWindowPos(FunctionListPanel::kName, ImVec2(0, 20));
    ImGui::SetWindowSize(FunctionListPanel::kName, ImVec2(620, 330));
    ImGui::SetWindowPos(ButterflyPanel::kName, ImVec2(640, 20));
    ImGui::SetWindowSize(ButterflyPanel::kName, ImVec2(620, 330));
    ImGui::SetWindowPos(CallGraphPanel::kName, ImVec2(0, 370));
    ImGui::SetWindowSize(CallGraphPanel::kName, ImVec2(620, 330));
    frames(3);
  }
  ImVec2 listCell(size_t item) {
    for (const auto& d : list->drawnRows())
      if (d.item == item) return ImVec2(d.x0 + 60.0f, (d.y0 + d.y1) * 0.5f);
    ADD_FAILURE() << "item " << item << " is not on screen";
    return ImVec2(0, 0);
  }
  ImVec2 butterflyCell(size_t item) {
    for (const auto& d : butterfly->drawnRows())
      if (d.item == item) return ImVec2(d.x0 + 60.0f, (d.y0 + d.y1) * 0.5f);
    ADD_FAILURE() << "butterfly item " << item << " is not on screen";
    return ImVec2(0, 0);
  }

  App app;
  FunctionListPanel* list = nullptr;
  ButterflyPanel* butterfly = nullptr;
  CallGraphPanel* graph = nullptr;
};

void expectTableEqualsQuery(const fl::Table& table, const std::vector<analysis::FunctionRow>& ref,
                            const analysis::QueryContext& ctx) {
  ASSERT_EQ(table.entries.size(), ref.size());
  for (size_t i = 0; i < ref.size(); ++i) {
    const fl::Entry& e = table.entries[i];
    EXPECT_EQ(e.key, ref[i].key) << i;
    EXPECT_EQ(e.inclusiveTicks, ref[i].inclusiveTicks) << i;
    EXPECT_EQ(e.exclusiveTicks, ref[i].exclusiveTicks) << i;
    EXPECT_EQ(e.instances, ref[i].instances) << i;
    EXPECT_EQ(e.full, analysis::functionKeyName(ctx, ref[i].key)) << i;
  }
}

void expectLinesEqual(const std::vector<fl::Line>& lines, const std::vector<analysis::FunctionRow>& ref) {
  ASSERT_EQ(lines.size(), ref.size());
  for (size_t i = 0; i < ref.size(); ++i) {
    EXPECT_EQ(lines[i].key, ref[i].key) << i;
    EXPECT_EQ(lines[i].inclusiveTicks, ref[i].inclusiveTicks) << i;
    EXPECT_EQ(lines[i].exclusiveTicks, ref[i].exclusiveTicks) << i;
    EXPECT_EQ(lines[i].instances, ref[i].instances) << i;
  }
}

}  // namespace

TEST(FunctionListWidget, RowsEqualBottomUpFunctionsOnTinyAndMedium) {
  for (const char* name : {"tiny.tachyon", "medium.tachyon"}) {
    Harness h;
    h.open(name);
    const auto ctx = h.refContext();
    const auto ref = analysis::bottomUpFunctions(*ctx, analysis::Filter{});
    ASSERT_TRUE(ref.ok());
    ASSERT_FALSE(ref->empty()) << name;
    expectTableEqualsQuery(*h.list->table(), *ref, *ctx);
    EXPECT_EQ(h.list->view().size(), ref->size());
    // The default sort is exclusive descending (the query's primary order; ties go by key, not by inclusive).
    EXPECT_EQ(h.list->sort().column, FunctionListPanel::SortColumn::Exclusive);
    EXPECT_FALSE(h.list->sort().ascending);
    ASSERT_EQ(h.list->view().size(), ref->size());
    const fl::Table& table = *h.list->table();
    for (size_t i = 1; i < ref->size(); ++i) {
      const fl::Entry &a = table.entries[h.list->view()[i - 1]], &b = table.entries[h.list->view()[i]];
      EXPECT_TRUE(a.exclusiveTicks > b.exclusiveTicks ||
                  (a.exclusiveTicks == b.exclusiveTicks && a.key < b.key))
          << name << " " << i;
    }
    EXPECT_FALSE(h.list->drawnRows().empty());
    EXPECT_EQ(h.list->message(), "");
    // The percentage base is the total weight: the sum of exclusive ticks.
    uint64_t sum = 0;
    for (const auto& r : *ref) sum += r.exclusiveTicks;
    EXPECT_EQ(h.list->table()->totalTicks, sum);
  }
}

TEST(FunctionListWidget, SortSpecsFromImGuiTableAreAdoptedAndEveryColumnSorts) {
  Harness h;
  h.open("tiny.tachyon");
  const fl::Table& t = *h.list->table();
  ASSERT_GE(t.entries.size(), 4u);
  const uint64_t queries = h.list->issuedQueries();
  const auto ticks = [&](fl::SortColumn c, const fl::Entry& e) -> uint64_t {
    switch (c) {
      case fl::SortColumn::Inclusive: return e.inclusiveTicks;
      case fl::SortColumn::Exclusive: return e.exclusiveTicks;
      default: return e.instances;
    }
  };
  for (const auto column :
       {fl::SortColumn::Inclusive, fl::SortColumn::Exclusive, fl::SortColumn::Instances}) {
    for (const bool asc : {true, false}) {
      h.list->setSort({column, asc});
      h.frames(3);  // the ImGui table adopts the sort and reports it back
      EXPECT_EQ(h.list->sort(), (fl::SortSpec{column, asc}));
      const auto& v = h.list->view();
      for (size_t i = 1; i < v.size(); ++i) {
        const uint64_t a = ticks(column, t.entries[v[i - 1]]), b = ticks(column, t.entries[v[i]]);
        EXPECT_TRUE(asc ? a <= b : a >= b) << i;
        if (a == b) EXPECT_LT(t.entries[v[i - 1]].keyRank, t.entries[v[i]].keyRank);
      }
    }
  }
  h.list->setSort({fl::SortColumn::Function, true});
  h.frames(3);
  EXPECT_EQ(h.list->sort(), (fl::SortSpec{fl::SortColumn::Function, true}));
  const auto& v = h.list->view();
  for (size_t i = 1; i < v.size(); ++i)
    EXPECT_LE(t.entries[v[i - 1]].functionRank, t.entries[v[i]].functionRank);
  h.list->setSort({fl::SortColumn::Module, false});
  h.frames(3);
  EXPECT_EQ(h.list->sort(), (fl::SortSpec{fl::SortColumn::Module, false}));
  for (size_t i = 1; i < v.size(); ++i) EXPECT_GE(t.entries[v[i - 1]].moduleRank, t.entries[v[i]].moduleRank);
  EXPECT_EQ(h.list->issuedQueries(), queries) << "sorting must not query";
}

TEST(FunctionListWidget, TextFilterIsClientSideAndDoesNotRequery) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t queries = h.list->issuedQueries();
  const size_t all = h.list->view().size();
  h.list->setFilterText("ENGINE");
  ASSERT_FALSE(h.list->view().empty());
  EXPECT_LT(h.list->view().size(), all);
  for (const uint32_t i : h.list->view()) {
    const fl::Entry& e = h.list->table()->entries[i];
    EXPECT_NE(fl::haystackOf(*h.list->table(), e).find("engine"), std::string_view::npos);
  }
  h.list->setFilterText("game.exe");  // by module
  EXPECT_FALSE(h.list->view().empty());
  for (const uint32_t i : h.list->view()) EXPECT_EQ(h.list->table()->entries[i].module, "game.exe");
  h.list->setFilterText("zzz-nothing");
  EXPECT_TRUE(h.list->view().empty());
  h.frames(3);  // an empty view draws fine
  h.list->setFilterText("");
  EXPECT_EQ(h.list->view().size(), all);
  EXPECT_EQ(h.list->issuedQueries(), queries);
}

TEST(FunctionListWidget, ClickingARowSelectsItsFunctionAndTheOtherPanelsFollow) {
  Harness h;
  h.open("tiny.tachyon");
  h.layout();
  const uint32_t entry = h.list->view()[1];
  const FunctionKey key = h.list->table()->entries[entry].key;
  const ImVec2 p = h.listCell(1);
  h.click(p.x, p.y);
  ASSERT_TRUE(h.bus().state().selectedFunction);
  EXPECT_EQ(*h.bus().state().selectedFunction, key);
  EXPECT_EQ(h.list->selectedKey(), key);
  ASSERT_TRUE(h.list->selectedEntry());
  EXPECT_EQ(*h.list->selectedEntry(), entry);
  h.settle();
  // Butterfly follows.
  ASSERT_TRUE(h.butterfly->data());
  EXPECT_EQ(h.butterfly->data()->view.self.key, key);
  // Call Graph follows.
  EXPECT_EQ(h.graph->selectedKey(), key);
  EXPECT_TRUE(h.graph->selectedId());
}

TEST(FunctionListWidget, ExternalSelectionScrollsHighlightsAndClearsAHidingFilter) {
  Harness h;
  h.open("tiny.tachyon");
  const uint32_t main = h.entryNamed("main");
  const uint32_t update = h.entryNamed("Engine::update");
  const FunctionKey updateKey = h.list->table()->entries[update].key;

  h.list->setFilterText("main");
  const size_t filtered = h.list->view().size();
  ASSERT_LT(filtered, h.list->table()->entries.size());
  h.bus().setSelectedFunction(updateKey);  // hidden by the text filter
  h.frames(3);
  EXPECT_EQ(h.list->filterText(), "");
  ASSERT_TRUE(h.list->selectedEntry());
  EXPECT_EQ(*h.list->selectedEntry(), update);
  const size_t item = fl::itemOfEntry(h.list->items(), update);
  ASSERT_NE(item, SIZE_MAX);
  bool drawn = false;
  for (const auto& d : h.list->drawnRows()) drawn |= d.item == item;
  EXPECT_TRUE(drawn);
  // Nothing was republished.
  EXPECT_EQ(*h.bus().state().selectedFunction, updateKey);

  // A function that is not in the table: no highlight, the key is remembered.
  h.bus().setSelectedFunction(FunctionKey{ModuleIdx(0), 0x7777});
  h.frames(3);
  EXPECT_FALSE(h.list->selectedEntry());
  // Clearing the selection clears the highlight.
  h.bus().setSelectedFunction(std::nullopt);
  h.frames(2);
  EXPECT_FALSE(h.list->selectedKey());
  h.bus().setSelectedFunction(h.list->table()->entries[main].key);
  h.frames(2);
  ASSERT_TRUE(h.list->selectedEntry());
  EXPECT_EQ(*h.list->selectedEntry(), main);
}

TEST(FunctionListWidget, ModuleGroupingSumsAndCollapse) {
  Harness h;
  h.open("tiny.tachyon");
  const fl::Table& t = *h.list->table();
  const uint64_t queries = h.list->issuedQueries();
  h.list->setGroupByModule(true);
  h.frames(2);
  const auto& groups = h.list->groups();
  ASSERT_GE(groups.size(), 2u);
  uint64_t exclusive = 0;
  size_t members = 0;
  for (const fl::Group& g : groups) {
    uint64_t incl = 0, excl = 0;
    for (const uint32_t m : g.members) {
      incl += t.entries[m].inclusiveTicks;
      excl += t.entries[m].exclusiveTicks;
      EXPECT_EQ(t.entries[m].module.empty() ? "(no module)" : t.entries[m].module, g.module);
    }
    EXPECT_EQ(g.inclusiveSum, incl);
    EXPECT_EQ(g.exclusiveSum, excl);
    exclusive += excl;
    members += g.members.size();
  }
  EXPECT_EQ(exclusive, t.totalTicks);  // exclusive sums partition the total: exact
  EXPECT_EQ(members, t.entries.size());
  EXPECT_EQ(h.list->items().size(), groups.size() + t.entries.size());
  // Sorted like the rows: exclusive sum descending.
  for (size_t i = 1; i < groups.size(); ++i) EXPECT_GE(groups[i - 1].exclusiveSum, groups[i].exclusiveSum);

  // Collapse the first group.
  const size_t before = h.list->items().size();
  h.list->toggleGroupCollapsed(0);
  EXPECT_EQ(h.list->items().size(), before - groups[0].members.size());
  EXPECT_EQ(h.list->collapsedModules().count(groups[0].module), 1u);
  // A bus selection of a function inside it expands the group again.
  const uint32_t hidden = groups[0].members[0];
  h.bus().setSelectedFunction(t.entries[hidden].key);
  h.frames(3);
  EXPECT_EQ(h.list->collapsedModules().count(groups[0].module), 0u);
  EXPECT_NE(fl::itemOfEntry(h.list->items(), hidden), SIZE_MAX);
  // Back to the flat list.
  h.list->setGroupByModule(false);
  EXPECT_EQ(h.list->items().size(), t.entries.size());
  EXPECT_TRUE(h.list->groups().empty());
  // Grouping does not query.
  EXPECT_EQ(h.list->issuedQueries(), queries);
}

TEST(FunctionListWidget, SubtreeToggleFollowsTheCallGraphFilter) {
  Harness h;
  h.open("tiny.tachyon");
  const PanelContext ctx = h.context();
  size_t row = SIZE_MAX;
  for (size_t i = 0; i < h.graph->rows().size(); ++i)
    if (h.graph->nameOf(h.graph->rows()[i].node->key).function == "Engine::update") row = i;
  ASSERT_NE(row, SIZE_MAX);
  const size_t wholeRows = h.list->table()->entries.size();
  ASSERT_TRUE(h.graph->filterToSubtree(ctx, *h.graph->targetOfRow(row)));
  h.settle();
  ASSERT_TRUE(h.bus().state().timeFilter);
  ASSERT_TRUE(h.bus().state().timeFilter->subtreeRoot);
  const analysis::Filter restricted = fl::makeFilter(h.bus().state().timeFilter, true);
  EXPECT_TRUE(h.list->restrictToSubtree());
  EXPECT_EQ(h.list->shownSpec().filter, restricted);
  EXPECT_TRUE(h.list->shownSpec().filter.subtreeRoot);
  {
    const auto ref_ctx = h.refContext();
    const auto ref = analysis::bottomUpFunctions(*ref_ctx, restricted);
    ASSERT_TRUE(ref.ok());
    expectTableEqualsQuery(*h.list->table(), *ref, *ref_ctx);
    EXPECT_LT(ref->size(), wholeRows);
  }

  // Off: the panel queries with the subtree cleared, the bus filter itself is untouched.
  const uint64_t queries = h.list->issuedQueries();
  h.list->setRestrictToSubtree(false);
  h.settle();
  EXPECT_GT(h.list->issuedQueries(), queries);
  EXPECT_TRUE(h.bus().state().timeFilter->subtreeRoot);
  EXPECT_FALSE(h.list->shownSpec().filter.subtreeRoot);
  EXPECT_EQ(h.list->table()->entries.size(), wholeRows);
  {
    const auto ref_ctx = h.refContext();
    const auto ref = analysis::bottomUpFunctions(*ref_ctx, fl::makeFilter(h.bus().state().timeFilter, false));
    ASSERT_TRUE(ref.ok());
    expectTableEqualsQuery(*h.list->table(), *ref, *ref_ctx);
  }
  // On again: back to the restricted result.
  h.list->setRestrictToSubtree(true);
  h.settle();
  EXPECT_TRUE(h.list->shownSpec().filter.subtreeRoot);
  EXPECT_LT(h.list->table()->entries.size(), wholeRows);
}

TEST(FunctionListWidget, OnlyTheNewestOfQuicklySucceedingFiltersIsShown) {
  Harness h;
  h.open("medium.tachyon");
  const model::TraceModel& m = h.model();
  const PanelContext ctx = h.context();
  const uint64_t span = m.header.end_ts - m.header.start_ts;
  const uint64_t delivered = h.list->deliveredTables();
  ViewFilter f;
  for (int i = 1; i <= 6; ++i) {  // six requests without a frame in between: each supersedes the last
    f.t0 = m.header.start_ts;
    f.t1 = m.header.start_ts + span * static_cast<uint64_t>(i) / 8;
    h.bus().setTimeFilter(f);
    h.list->update(ctx);
  }
  h.settle();
  const auto rc = h.refContext();
  const auto ref = analysis::bottomUpFunctions(*rc, fl::makeFilter(f, true));
  ASSERT_TRUE(ref.ok());
  expectTableEqualsQuery(*h.list->table(), *ref, *rc);
  EXPECT_EQ(h.list->shownSpec().filter, fl::makeFilter(f, true));
  EXPECT_GE(h.list->deliveredTables(), delivered + 1);
  EXPECT_LE(h.list->deliveredTables(), delivered + 6);

  // Rapid text filter changes never query and the view always matches the last text.
  const uint64_t queries = h.list->issuedQueries();
  for (const char* text : {"a", "ab", "abc", "e", "en", "engine"}) h.list->setFilterText(text);
  h.list->setFilterText("engine");
  for (const uint32_t i : h.list->view())
    EXPECT_NE(fl::haystackOf(*h.list->table(), h.list->table()->entries[i]).find("engine"),
              std::string_view::npos);
  EXPECT_EQ(h.list->issuedQueries(), queries);
}

TEST(FunctionListWidget, ThePreviousTableStaysWhileANewQueryRuns) {
  Harness h;
  h.open("medium.tachyon");
  const auto before = h.list->table();
  ViewFilter f;
  f.t0 = h.model().header.start_ts;
  f.t1 = f.t0 + (h.model().header.end_ts - f.t0) / 2;
  h.bus().setTimeFilter(f);
  h.frames(1);
  EXPECT_TRUE(h.list->table());  // never disappears, greyed while pending
  h.settle();
  EXPECT_NE(h.list->table(), before);
}

TEST(FunctionListWidget, SymbolGenerationChangeRekeysAndRefindsTheSelection) {
  Harness h;
  h.open("tiny.tachyon");
  const AnalysisContext& analysis = h.app.document()->analysis();
  const uint32_t update = h.entryNamed("Engine::update");
  const FunctionKey key = h.list->table()->entries[update].key;
  h.bus().setSelectedFunction(key);
  h.settle();
  ASSERT_TRUE(h.list->selectedEntry());
  const uint64_t queries = h.list->issuedQueries();

  PanelContext ctx = h.context();
  ctx.symbolGeneration += 1;  // as if a module had resolved
  h.list->update(ctx);
  EXPECT_EQ(h.list->issuedQueries(), queries + 1);
  EXPECT_EQ(h.list->requestedSpec().symbolGeneration, ctx.symbolGeneration);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    h.list->update(ctx);
    if (!h.list->pending() && h.list->shownSpec() == h.list->requestedSpec()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(!h.list->pending() && h.list->shownSpec() == h.list->requestedSpec());
  EXPECT_EQ(h.list->shownSpec().symbolGeneration, ctx.symbolGeneration);
  EXPECT_EQ(analysis.snapshotCount(), 1u);  // the older generation's context was released
  // The selection was found again by key in the new table.
  ASSERT_TRUE(h.list->selectedEntry());
  EXPECT_EQ(h.list->table()->entries[*h.list->selectedEntry()].key, key);

  // The Butterfly re-queries on the new generation as well.
  const uint64_t bfQueries = h.butterfly->issuedQueries();
  h.butterfly->update(ctx);
  EXPECT_EQ(h.butterfly->issuedQueries(), bfQueries + 1);
  EXPECT_EQ(h.butterfly->requestedSpec().base.symbolGeneration, ctx.symbolGeneration);
  while (std::chrono::steady_clock::now() < deadline) {
    h.butterfly->update(ctx);
    if (!h.butterfly->pending()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(h.butterfly->data());
  EXPECT_EQ(h.butterfly->data()->view.self.key, key);
}

TEST(FunctionListWidget, ContextMenuActionsPublishOnTheBus) {
  Harness h;
  h.open("tiny.tachyon");
  h.layout();
  const PanelContext ctx = h.context();
  const uint32_t entry = h.entryNamed("Engine::update");
  const FunctionKey key = h.list->table()->entries[entry].key;

  uint64_t serial = h.bus().state().panelRequest.serial;
  h.list->showInCallGraph(ctx, entry);
  EXPECT_GT(h.bus().state().panelRequest.serial, serial);
  EXPECT_EQ(h.bus().state().panelRequest.name, "Call Graph");
  EXPECT_EQ(*h.bus().state().selectedFunction, key);

  h.bus().setSelectedFunction(std::nullopt);
  serial = h.bus().state().panelRequest.serial;
  h.list->showSource(ctx, entry);
  EXPECT_GT(h.bus().state().panelRequest.serial, serial);
  EXPECT_EQ(h.bus().state().panelRequest.name, "Source");
  EXPECT_EQ(*h.bus().state().selectedFunction, key);

  h.list->copyName(entry);
  EXPECT_STREQ(ImGui::GetClipboardText(), "game.exe!Engine::update");

  // Right click on a row selects it and opens the menu on it.
  const ImVec2 p = h.listCell(0);
  const uint32_t first = h.list->view()[0];
  h.click(p.x, p.y, 1);
  ASSERT_TRUE(h.list->menuEntry());
  EXPECT_EQ(*h.list->menuEntry(), first);
  EXPECT_EQ(*h.bus().state().selectedFunction, h.list->table()->entries[first].key);
  EXPECT_TRUE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
}

TEST(FunctionListWidget, ThePanelRequestForTheFunctionListIsCounted) {
  Harness h;
  h.open("tiny.tachyon");
  EXPECT_EQ(h.list->focusRequests(), 0u);
  h.bus().requestPanel("Function List");
  h.frames(2);
  EXPECT_EQ(h.list->focusRequests(), 1u);
  h.bus().requestPanel("Butterfly");
  h.frames(2);
  EXPECT_EQ(h.butterfly->focusRequests(), 1u);
  EXPECT_EQ(h.list->focusRequests(), 1u);
}

TEST(ButterflyWidget, NothingSelectedShowsNothing) {
  Harness h;
  h.open("tiny.tachyon");
  EXPECT_FALSE(h.bus().state().selectedFunction);
  EXPECT_FALSE(h.butterfly->data());
  EXPECT_EQ(h.butterfly->issuedQueries(), 0u);
  EXPECT_TRUE(h.butterfly->items().empty());
}

TEST(ButterflyWidget, ViewEqualsTheAnalysisButterfly) {
  for (const char* name : {"tiny.tachyon", "medium.tachyon"}) {
    Harness h;
    h.open(name);
    const auto rc = h.refContext();
    // Every function of the list (tiny) / its heaviest ones (medium).
    const size_t n = std::min<size_t>(h.list->table()->entries.size(), 12);
    for (size_t i = 0; i < n; ++i) {
      const FunctionKey key = h.list->table()->entries[h.list->view()[i]].key;
      h.bus().setSelectedFunction(key);
      h.settle();
      ASSERT_TRUE(h.butterfly->data());
      const auto ref = analysis::butterfly(*rc, analysis::Filter{}, key);
      ASSERT_TRUE(ref.ok());
      const fl::ButterflyView& v = h.butterfly->data()->view;
      EXPECT_EQ(v.self.key, key);
      EXPECT_EQ(v.self.inclusiveTicks, ref->self.inclusiveTicks) << name << " " << i;
      EXPECT_EQ(v.self.exclusiveTicks, ref->self.exclusiveTicks);
      EXPECT_EQ(v.self.instances, ref->self.instances);
      expectLinesEqual(v.callers, ref->callers);
      expectLinesEqual(v.callees, ref->callees);
      // The caller percentages sum to 100 (recursion counted once).
      double sum = 0;
      for (const fl::Line& l : v.callers) sum += l.inclusivePercent;
      if (v.self.inclusiveTicks > 0) EXPECT_NEAR(sum, 100.0, 1e-6);
      // And the function list agrees with the butterfly on the function's own numbers.
      const fl::Entry& e = h.list->table()->entries[*h.list->selectedEntry()];
      EXPECT_EQ(e.inclusiveTicks, v.self.inclusiveTicks);
      EXPECT_EQ(e.exclusiveTicks, v.self.exclusiveTicks);
      EXPECT_EQ(e.instances, v.self.instances);
    }
  }
}

TEST(ButterflyWidget, TheStackBaseHasTheRootCallerAndItIsNotNavigable) {
  Harness h;
  h.open("tiny.tachyon");
  h.layout();
  const uint32_t start = h.entryNamed("RtlUserThreadStart");
  h.bus().setSelectedFunction(h.list->table()->entries[start].key);
  h.settle();
  const fl::ButterflyView& v = h.butterfly->data()->view;
  ASSERT_EQ(v.callers.size(), 1u);
  EXPECT_EQ(v.callers[0].key, analysis::kRootKey);
  EXPECT_EQ(v.callers[0].name.function, "<root>");
  EXPECT_FALSE(v.callers[0].navigable);
  EXPECT_NEAR(v.callers[0].inclusivePercent, 100.0, 1e-9);
  // Items: callers header, <root>, self, callees header, callees.
  ASSERT_GE(h.butterfly->items().size(), 4u);
  EXPECT_EQ(h.butterfly->items()[0].kind, ButterflyPanel::Item::Kind::CallersHeader);
  EXPECT_EQ(h.butterfly->items()[1].kind, ButterflyPanel::Item::Kind::Caller);
  EXPECT_EQ(h.butterfly->items()[2].kind, ButterflyPanel::Item::Kind::Self);
  EXPECT_EQ(h.butterfly->items()[3].kind, ButterflyPanel::Item::Kind::CalleesHeader);
  // Clicking <root> selects nothing new.
  const FunctionKey selected = *h.bus().state().selectedFunction;
  const ImVec2 p = h.butterflyCell(1);
  h.click(p.x, p.y);
  EXPECT_EQ(*h.bus().state().selectedFunction, selected);
  EXPECT_FALSE(h.butterfly->navigate(h.context(), v.callers[0]));
}

TEST(ButterflyWidget, ClickingACalleeNavigatesAndEveryPanelFollows) {
  Harness h;
  h.open("tiny.tachyon");
  h.layout();
  const uint32_t main = h.entryNamed("main");
  h.bus().setSelectedFunction(h.list->table()->entries[main].key);
  h.settle();
  const fl::ButterflyView& v = h.butterfly->data()->view;
  ASSERT_FALSE(v.callees.empty());
  ASSERT_FALSE(v.callers.empty());
  const FunctionKey callee = v.callees[0].key;
  // Callee line is item: header, callers..., self, header, callees...
  const size_t calleeItem = 1 + v.callers.size() + 1 + 1;
  const ImVec2 p = h.butterflyCell(calleeItem);
  h.click(p.x, p.y);
  ASSERT_TRUE(h.bus().state().selectedFunction);
  EXPECT_EQ(*h.bus().state().selectedFunction, callee);
  h.settle();
  EXPECT_EQ(h.butterfly->data()->view.self.key, callee);
  ASSERT_TRUE(h.list->selectedEntry());
  EXPECT_EQ(h.list->table()->entries[*h.list->selectedEntry()].key, callee);
  EXPECT_EQ(h.graph->selectedKey(), callee);
  // The old function is now a caller of the new one.
  bool found = false;
  for (const fl::Line& l : h.butterfly->data()->view.callers)
    found |= l.key == h.list->table()->entries[main].key;
  EXPECT_TRUE(found);
}

TEST(ButterflyWidget, CallGraphSelectionDrivesBothPanels) {
  Harness h;
  h.open("tiny.tachyon");
  const PanelContext ctx = h.context();
  size_t row = SIZE_MAX;
  for (size_t i = 0; i < h.graph->rows().size(); ++i)
    if (h.graph->nameOf(h.graph->rows()[i].node->key).function == "Engine::update") row = i;
  ASSERT_NE(row, SIZE_MAX);
  const FunctionKey key = h.graph->rows()[row].node->key;
  h.graph->selectRow(ctx, row);
  h.settle();
  ASSERT_TRUE(h.list->selectedEntry());
  EXPECT_EQ(h.list->table()->entries[*h.list->selectedEntry()].key, key);
  ASSERT_TRUE(h.butterfly->data());
  EXPECT_EQ(h.butterfly->data()->view.self.key, key);

  // And back: the list drives the graph.
  const uint32_t main = h.entryNamed("main");
  h.list->selectEntry(ctx, main);
  h.settle();
  EXPECT_EQ(h.graph->selectedKey(), h.list->table()->entries[main].key);
  EXPECT_EQ(h.butterfly->data()->view.self.key, h.list->table()->entries[main].key);

  // "Show in Function List" of the Call Graph asks for this panel.
  const auto target = h.graph->targetOfRow(row);
  ASSERT_TRUE(target);
  const uint64_t focus = h.list->focusRequests();
  h.graph->showInFunctionList(ctx, *target);
  h.frames(2);
  EXPECT_EQ(h.list->focusRequests(), focus + 1);
  ASSERT_TRUE(h.list->selectedEntry());
  EXPECT_EQ(h.list->table()->entries[*h.list->selectedEntry()].key, key);
}

TEST(ButterflyWidget, UnknownFunctionGivesAnEmptyView) {
  Harness h;
  h.open("tiny.tachyon");
  h.bus().setSelectedFunction(FunctionKey{ModuleIdx(0), 0x7777});
  h.settle();
  ASSERT_TRUE(h.butterfly->data());
  EXPECT_TRUE(h.butterfly->data()->view.empty());
  EXPECT_TRUE(h.butterfly->data()->view.callers.empty());
  EXPECT_TRUE(h.butterfly->data()->view.callees.empty());
  // The synthetic root key is never queried.
  h.bus().setSelectedFunction(analysis::kRootKey);
  h.frames(3);
  EXPECT_FALSE(h.butterfly->data());
}

TEST(ButterflyWidget, FollowsTheTimeFilterAndTheSubtreeToggle) {
  Harness h;
  h.open("tiny.tachyon");
  const PanelContext ctx = h.context();
  const uint32_t main = h.entryNamed("main");
  const FunctionKey key = h.list->table()->entries[main].key;
  h.bus().setSelectedFunction(key);
  h.settle();
  const uint64_t full = h.butterfly->data()->view.self.inclusiveTicks;

  ViewFilter f;
  f.t0 = h.model().header.start_ts;
  f.t1 = f.t0 + (h.model().header.end_ts - f.t0) / 3;
  h.bus().setTimeFilter(f);
  h.settle();
  const auto rc = h.refContext();
  const auto ref = analysis::butterfly(*rc, fl::makeFilter(f, true), key);
  ASSERT_TRUE(ref.ok());
  EXPECT_EQ(h.butterfly->data()->view.self.inclusiveTicks, ref->self.inclusiveTicks);
  EXPECT_LE(ref->self.inclusiveTicks, full);
  expectLinesEqual(h.butterfly->data()->view.callees, ref->callees);

  // With a subtree on the bus filter, the toggle chooses whether it applies.
  size_t row = SIZE_MAX;
  for (size_t i = 0; i < h.graph->rows().size(); ++i)
    if (h.graph->nameOf(h.graph->rows()[i].node->key).function == "Engine::update") row = i;
  ASSERT_NE(row, SIZE_MAX);
  h.bus().setTimeFilter(std::nullopt);
  h.settle();
  ASSERT_TRUE(h.graph->filterToSubtree(ctx, *h.graph->targetOfRow(row)));
  h.bus().setSelectedFunction(key);
  h.settle();
  const auto restricted = analysis::butterfly(*rc, fl::makeFilter(h.bus().state().timeFilter, true), key);
  ASSERT_TRUE(restricted.ok());
  EXPECT_EQ(h.butterfly->data()->view.self.inclusiveTicks, restricted->self.inclusiveTicks);
  h.butterfly->setRestrictToSubtree(false);
  h.settle();
  EXPECT_EQ(h.butterfly->data()->view.self.inclusiveTicks, full);
}

TEST(ButterflyWidget, ClosingTheDocumentDropsEverythingAndANewOneStartsClean) {
  Harness h;
  h.open("medium.tachyon");
  h.bus().setSelectedFunction(h.list->table()->entries[h.list->view()[0]].key);
  h.settle();
  ViewFilter f;
  f.t0 = h.model().header.start_ts;
  f.t1 = f.t0 + 1000;
  h.bus().setTimeFilter(f);
  h.frames(1);  // queries may be in flight now
  h.app.commands().run("file.close");
  h.frames(3);
  EXPECT_EQ(h.app.document(), nullptr);
  EXPECT_FALSE(h.list->table());
  EXPECT_TRUE(h.list->items().empty());
  EXPECT_FALSE(h.list->pending());
  EXPECT_FALSE(h.butterfly->data());
  EXPECT_FALSE(h.butterfly->pending());
  h.app.openFile(fixture("tiny.tachyon"));
  ASSERT_TRUE(h.until([&] { return h.app.document() != nullptr && !h.app.loading(); }));
  ASSERT_TRUE(h.until([&] { return h.settled(); }));
  EXPECT_TRUE(h.list->table());
  EXPECT_FALSE(h.list->selectedKey());
  EXPECT_FALSE(h.butterfly->data());
}
