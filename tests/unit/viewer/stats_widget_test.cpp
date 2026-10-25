// WP-47: the Function Statistics panel driven headless (Null renderer, real ImGui frames, injected input) on
// the `scopes` fixture: Ctrl+F opens and focuses, typing filters, Enter selects, the numbers equal
// functionStats / scopeStats, the timeline overlay is drawn on the right rows, Previous / Next navigate (and
// wrap), populate-from-selection, Ctrl+F is not stolen from a text field, stale results.
#include "analysis/instances.h"
#include "analysis/query.h"
#include "analysis/query_executor.h"
#include "viewer/app.h"
#include "viewer/builtin_panels.h"
#include "viewer/functions/function_list_panel.h"
#include "viewer/stats/function_stats_panel.h"
#include "viewer/timeline/timeline_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace st = tachyon::viewer::stats;
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
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_stats_test_no_such_settings.json";
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
    panel = find<FunctionStatsPanel>(FunctionStatsPanel::kName);
    timeline = find<ThreadsTimelinePanel>(ThreadsTimelinePanel::kName);
    list = find<FunctionListPanel>(FunctionListPanel::kName);
    for (PanelEntry& e : app.panels().entries()) {
      const IPanel* p = e.panel.get();
      e.open = p == timeline;  // the timeline is on screen; the statistics panel opens with Ctrl+F
    }
  }

  template <typename T>
  T* find(const char* name) {
    PanelEntry* entry = app.panels().find(name);
    EXPECT_NE(entry, nullptr) << name;
    T* p = entry ? dynamic_cast<T*>(entry->panel.get()) : nullptr;
    EXPECT_NE(p, nullptr) << name;
    return p;
  }
  PanelEntry* entryOf(const char* name) { return app.panels().find(name); }

  SelectionBus& bus() { return app.selection(); }
  const model::TraceModel& model() { return app.document()->model(); }
  timeline::TimelineWidget& widget() { return timeline->widget(); }

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
  // The index of the panel was built for the current symbol generation and nothing is in flight.
  bool panelSettled() {
    return panel->index() && !panel->indexPending() && !panel->statsPending() &&
           panel->requestedIndexSymbolGeneration() == app.symbolGeneration() &&
           panel->indexSymbolGeneration() == app.symbolGeneration();
  }
  bool selectionSettled() {
    if (!panel->selected()) return true;
    return panel->result() && panel->result()->key == panel->selected()->key &&
           panel->requestedStatsSpec().symbolGeneration == app.symbolGeneration() &&
           panel->shownStatsSpec() == panel->requestedStatsSpec();
  }
  void open(const char* name) {
    app.openFile(fixture(name));
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    ASSERT_TRUE(until([&] { return symbolsDone(); }));
    // A symbol generation that stays put for 100 ms.
    for (;;) {
      const uint64_t generation = app.symbolGeneration();
      const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (std::chrono::steady_clock::now() < stop) {
        frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (generation == app.symbolGeneration()) break;
    }
    ASSERT_TRUE(until([&] { return widget().lodFinished(); }));
  }
  void openPanel() {
    entryOf(FunctionStatsPanel::kName)->open = true;
    ASSERT_TRUE(until([&] { return panelSettled(); }));
    layout();
  }
  void settle() {
    frames(2);  // the panels notice what changed and issue their queries
    ASSERT_TRUE(until([&] { return symbolsDone() && panelSettled() && selectionSettled(); }));
    frames(2);
  }
  // Timeline on top, statistics panel below, both fully on the 1280x720 display.
  void layout() {
    frames(2);
    ImGui::SetWindowPos(ThreadsTimelinePanel::kName, ImVec2(0, 20));
    ImGui::SetWindowSize(ThreadsTimelinePanel::kName, ImVec2(1260, 195));
    ImGui::SetWindowPos(FunctionStatsPanel::kName, ImVec2(0, 225));
    ImGui::SetWindowSize(FunctionStatsPanel::kName, ImVec2(1000, 490));
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

  std::unique_ptr<analysis::QueryContext> refContext() {
    return std::make_unique<analysis::QueryContext>(model(), *app.document()->symbols());
  }

  void key(ImGuiKey k) {
    ImGui::GetIO().AddKeyEvent(k, true);
    frames(1);
    ImGui::GetIO().AddKeyEvent(k, false);
    frames(1);
  }
  void chord(ImGuiKey mod, ImGuiKey k) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(mod, true);
    io.AddKeyEvent(k, true);
    frames(1);
    io.AddKeyEvent(k, false);
    io.AddKeyEvent(mod, false);
    frames(2);
  }
  void type(const char* text) {
    ImGui::GetIO().AddInputCharactersUTF8(text);
    frames(2);
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

  // Picks the entry named `name` by typing it and pressing Enter.
  void choose(const std::string& name) {
    panel->setQueryText(name);
    ASSERT_FALSE(panel->hits().empty()) << name;
    ASSERT_EQ(panel->index()->entries[panel->hits()[0].entry].name, name);
    panel->chooseResult(context(), 0);
    settle();
  }

  App app;
  FunctionStatsPanel* panel = nullptr;
  ThreadsTimelinePanel* timeline = nullptr;
  FunctionListPanel* list = nullptr;
};

bool windowFocused(const char* name) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  return g.NavWindow && g.NavWindow->RootWindow == ImGui::FindWindowByName(name);
}

}  // namespace

TEST(StatsWidget, CtrlFOpensAndFocusesThePanelAndTheSearchBox) {
  Harness h;
  h.open("scopes.tachyon");
  PanelEntry* entry = h.entryOf(FunctionStatsPanel::kName);
  ASSERT_NE(entry, nullptr);
  EXPECT_FALSE(entry->open);
  EXPECT_STREQ(FunctionStatsPanel::kName, kFunctionStatsPanelName);
  const uint64_t before = h.panel->focusRequests();
  h.chord(ImGuiMod_Ctrl, ImGuiKey_F);
  EXPECT_TRUE(entry->open);
  EXPECT_EQ(h.panel->focusRequests(), before + 1);
  ASSERT_TRUE(h.until([&] { return h.panelSettled(); }));
  h.frames(3);
  EXPECT_TRUE(windowFocused(FunctionStatsPanel::kName));
  EXPECT_TRUE(h.panel->searchActive());
  // The command is also listed for the palette.
  bool listed = false;
  for (const Command& c : h.app.commands().commands())
    listed = listed || (c.id == "stats.search" && c.shortcut == "Ctrl+F");
  EXPECT_TRUE(listed);
  // A PanelRequest does the same for a panel that was closed in between.
  entry->open = false;
  h.frames(2);
  h.bus().requestPanel(FunctionStatsPanel::kName);
  h.frames(3);
  EXPECT_TRUE(entry->open);
  EXPECT_TRUE(h.panel->searchActive());
}

TEST(StatsWidget, TypingFiltersAndKeysChooseTheEntry) {
  Harness h;
  h.open("scopes.tachyon");
  h.chord(ImGuiMod_Ctrl, ImGuiKey_F);
  ASSERT_TRUE(h.until([&] { return h.panelSettled(); }));
  h.layout();
  h.frames(2);
  ASSERT_TRUE(h.panel->searchActive());
  ASSERT_NE(h.panel->index(), nullptr);
  const size_t total = h.panel->index()->entries.size();
  EXPECT_GT(total, 10u);
  EXPECT_EQ(h.panel->hits().size(), std::min<size_t>(total, st::kMaxSearchResults));  // empty query lists all
  h.type("jobw");
  EXPECT_EQ(h.panel->queryText(), "jobw");
  ASSERT_FALSE(h.panel->hits().empty());
  EXPECT_LT(h.panel->hits().size(), total);
  const st::SearchEntry& top = h.panel->index()->entries[h.panel->hits()[0].entry];
  EXPECT_EQ(top.name, "JobWait");
  EXPECT_EQ(top.kind, st::EntryKind::Scope);
  EXPECT_EQ(top.count, 150u);
  EXPECT_EQ(h.panel->cursor(), 0);
  // "Job::wait" is among the matches too (function), further down.
  bool function = false;
  for (const st::SearchHit& hit : h.panel->hits())
    function = function || h.panel->index()->entries[hit.entry].name == "game.exe!Job::wait";
  EXPECT_TRUE(function);

  // Down / Up move the cursor, Enter chooses the row under it.
  h.key(ImGuiKey_DownArrow);
  EXPECT_EQ(h.panel->cursor(), 1);
  h.key(ImGuiKey_UpArrow);
  EXPECT_EQ(h.panel->cursor(), 0);
  h.key(ImGuiKey_UpArrow);
  EXPECT_EQ(h.panel->cursor(), 0);  // clamped
  h.key(ImGuiKey_Enter);
  ASSERT_TRUE(h.panel->selected().has_value());
  EXPECT_EQ(h.panel->selected()->name, "JobWait");
  ASSERT_TRUE(h.bus().state().selectedFunction.has_value());
  EXPECT_TRUE(h.bus().state().selectedFunction->isScope());
  EXPECT_EQ(h.bus().state().selectedFunction->value, h.model().strings.find("JobWait").get());
  h.frames(2);
  EXPECT_TRUE(h.panel->searchActive());  // the keyboard stays in the box
  h.settle();
  ASSERT_TRUE(h.panel->result());
  EXPECT_EQ(h.panel->result()->stats->count, 150u);

  // Case-insensitive: a different spelling finds the same entry.
  h.panel->setQueryText("JOBWAIT");
  EXPECT_EQ(h.panel->index()->entries[h.panel->hits()[0].entry].name, "JobWait");
  // A click on a row chooses it.
  h.panel->setQueryText("jobrun");
  h.frames(3);
  ASSERT_FALSE(h.panel->drawnRows().empty());
  const auto& row = h.panel->drawnRows()[0];
  h.click((row.x0 + row.x1) * 0.5f, (row.y0 + row.y1) * 0.5f);
  ASSERT_TRUE(h.panel->selected().has_value());
  EXPECT_EQ(h.panel->selected()->name, "JobRun");
}

TEST(StatsWidget, ScopeStatisticsEqualScopeStats) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  const StringIdx frame = h.model().strings.find("Frame");
  ASSERT_TRUE(frame.valid());
  h.choose("Frame");
  ASSERT_TRUE(h.panel->result());
  auto ref = h.refContext();
  auto want = analysis::scopeStats(*ref, {}, frame);
  ASSERT_TRUE(want.ok());
  const analysis::Stats& got = *h.panel->result()->stats;
  EXPECT_EQ(got.count, 99u);
  EXPECT_EQ(got.count, want->count);
  EXPECT_EQ(got.min, want->min);
  EXPECT_EQ(got.p25, want->p25);
  EXPECT_EQ(got.median, want->median);
  EXPECT_EQ(got.p75, want->p75);
  EXPECT_EQ(got.max, want->max);
  EXPECT_EQ(got.mean, want->mean);
  EXPECT_EQ(got.totalTicks, want->totalTicks);
  EXPECT_EQ(got.histogram, want->histogram);
  // The histogram and box plot drawn in the last frame come from those numbers.
  const st::HistogramLayout& hist = h.panel->lastHistogram();
  ASSERT_FALSE(hist.bars.empty());
  uint32_t drawnCount = 0;
  for (const st::HistogramBar& b : hist.bars) {
    EXPECT_EQ(b.count, want->histogram[b.bucket]);
    drawnCount += b.count;
  }
  EXPECT_EQ(drawnCount, want->count);
  const st::BoxPlotLayout& box = h.panel->lastBoxPlot();
  ASSERT_TRUE(box.valid);
  EXPECT_LE(box.xMin, box.xQ1);
  EXPECT_LE(box.xQ1, box.xMedian);
  EXPECT_LE(box.xMedian, box.xQ3);
  EXPECT_LE(box.xQ3, box.xMax);
  // The bus carries the scope as the selected function.
  EXPECT_EQ(h.bus().state().selectedFunction, std::optional<FunctionKey>(FunctionKey::scope(frame)));
}

TEST(StatsWidget, FunctionStatisticsEqualFunctionStatsAndFollowTheTimeFilter) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  h.choose("game.exe!Engine::update");
  ASSERT_TRUE(h.panel->result());
  auto ref = h.refContext();
  const FunctionKey key = h.panel->selected()->key;
  auto want = analysis::functionStats(*ref, {}, key);
  ASSERT_TRUE(want.ok());
  ASSERT_GT(want->count, 0u);
  EXPECT_EQ(h.panel->result()->stats->count, want->count);
  EXPECT_EQ(h.panel->result()->stats->totalTicks, want->totalTicks);
  EXPECT_EQ(h.panel->result()->stats->median, want->median);
  EXPECT_EQ(h.panel->result()->stats->histogram, want->histogram);
  auto insts = analysis::functionInstances(*ref, {}, key);
  ASSERT_TRUE(insts.ok());
  EXPECT_EQ(*h.panel->result()->instances, **insts);

  // A time range on the bus re-runs the statistics for that range (the Call Graph subtree is ignored).
  const uint64_t t0 = h.model().header.start_ts;
  const uint64_t mid = t0 + (h.model().header.end_ts - t0) / 2;
  ViewFilter vf;
  vf.t0 = t0;
  vf.t1 = mid;
  vf.subtreeRoot = CallstackIdx(1);
  h.bus().setTimeFilter(vf);
  h.settle();
  analysis::Filter f;
  f.t0 = t0;
  f.t1 = mid;
  auto half = analysis::functionStats(*ref, f, key);
  ASSERT_TRUE(half.ok());
  EXPECT_LT(half->count, want->count);
  EXPECT_EQ(h.panel->result()->stats->count, half->count);
  EXPECT_EQ(h.panel->result()->stats->totalTicks, half->totalTicks);
}

TEST(StatsWidget, TimelineOverlayIsDrawnOnTheRowsOfTheInstances) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  const StringIdx frame = h.model().strings.find("Frame");
  h.choose("Frame");
  ASSERT_TRUE(h.panel->result());
  const analysis::Instance& any = (*h.panel->result()->instances)[0];
  // Published as the bus' highlight: the key and the same list.
  const InstanceHighlight& hl = h.bus().state().instanceHighlight;
  ASSERT_TRUE(hl.active());
  EXPECT_EQ(hl.key, std::optional<FunctionKey>(FunctionKey::scope(frame)));
  EXPECT_EQ(hl.instances, h.panel->result()->instances);
  h.frames(3);

  // Whole trace: one rect per instance (they are far enough apart), all on the row of their thread.
  const auto& rects = h.widget().lastHighlightRects();
  const size_t n = h.panel->result()->instances->size();
  EXPECT_EQ(n, 99u);
  EXPECT_EQ(rects.size(), n);
  EXPECT_EQ(h.widget().frameStats().highlightRects, n);
  const auto bandY = h.widget().bandCenterY(any.thread);
  ASSERT_TRUE(bandY.has_value());
  for (const timeline::HighlightRectInfo& r : rects) {
    EXPECT_EQ((*hl.instances)[r.instance].thread, r.thread);
    EXPECT_EQ(r.thread, any.thread);  // Frame scopes live on one thread only
    EXPECT_LT(r.y0, *bandY);
    EXPECT_GT(r.y1, *bandY);
    EXPECT_GT(r.x1 - r.x0, 0.0f);
    EXPECT_GE(r.x0, h.widget().screenGeometry().plotX0 - 0.5f);
    EXPECT_LE(r.x1, h.widget().screenGeometry().plotX0 + h.widget().screenGeometry().plotWidth + 0.5f);
  }

  // Zoomed to a quarter of the trace: exactly the instances that overlap the view.
  const auto& lim = h.widget().limits();
  timeline::TimeView v;
  v.start = lim.total() * 0.25;
  v.span = lim.total() * 0.25;
  h.widget().setView(v);
  h.frames(3);
  const uint64_t a = lim.traceStart + static_cast<uint64_t>(h.widget().view().start);
  const uint64_t b = lim.traceStart + static_cast<uint64_t>(h.widget().view().end());
  size_t expected = 0;
  for (const analysis::Instance& in : *h.panel->result()->instances)
    if (in.end >= a && in.begin <= b) ++expected;
  EXPECT_GT(expected, 5u);
  EXPECT_LT(expected, n);
  EXPECT_EQ(h.widget().lastHighlightRects().size(), expected);

  // Hiding the row of the instances draws nothing; clearing the highlight removes the overlay.
  h.widget().setHidden(any.thread, true);
  h.frames(3);
  EXPECT_TRUE(h.widget().lastHighlightRects().empty());
  h.widget().setHidden(any.thread, false);
  h.frames(2);
  EXPECT_FALSE(h.widget().lastHighlightRects().empty());
  h.bus().setInstanceHighlight(std::nullopt, nullptr);
  h.frames(3);
  EXPECT_TRUE(h.widget().lastHighlightRects().empty());
}

TEST(StatsWidget, TimelineOverlayOfASampledFunctionIsDrawnOnEveryThreadItRunsOn) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  h.choose("game.exe!Engine::update");
  ASSERT_TRUE(h.panel->result());
  h.frames(3);
  const auto& insts = *h.panel->result()->instances;
  ASSERT_GT(insts.size(), 5u);
  size_t visible = 0;
  for (const timeline::HighlightRectInfo& r : h.widget().lastHighlightRects()) {
    EXPECT_EQ(insts[r.instance].thread, r.thread);
    ++visible;
  }
  EXPECT_GT(visible, 0u);
  EXPECT_LE(visible, insts.size());
}

TEST(StatsWidget, NextAndPreviousNavigateWrapAndSelectTheThread) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  h.choose("Frame");
  ASSERT_TRUE(h.panel->result());
  const auto& insts = *h.panel->result()->instances;
  const size_t n = insts.size();
  ASSERT_EQ(n, 99u);
  const uint64_t interval = h.model().sampleIntervalTicks;
  const PanelContext ctx = h.context();

  const auto expectAt = [&](uint32_t index, uint64_t serialBefore) {
    ASSERT_EQ(h.panel->currentInstance(), std::optional<uint32_t>(index));
    const NavigationRequest& nav = h.bus().state().navigation;
    EXPECT_GT(nav.serial, serialBefore);
    const analysis::Instance& in = insts[index];
    const st::TickWindow w = st::navigationWindow(in, interval);
    EXPECT_EQ(nav.thread, in.thread);
    EXPECT_EQ(nav.t0, w.t0);
    EXPECT_EQ(nav.t1, w.t1);
    EXPECT_LE(nav.t0, in.begin);
    EXPECT_GE(nav.t1, in.end);
    EXPECT_EQ(h.bus().state().selectedThread, in.thread);
    EXPECT_EQ(h.bus().state().instanceHighlight.current, std::optional<uint32_t>(index));
  };

  uint64_t serial = h.bus().state().navigation.serial;
  ASSERT_TRUE(h.panel->nextInstance(ctx));
  expectAt(0, serial);
  serial = h.bus().state().navigation.serial;
  ASSERT_TRUE(h.panel->nextInstance(ctx));
  expectAt(1, serial);
  serial = h.bus().state().navigation.serial;
  ASSERT_TRUE(h.panel->previousInstance(ctx));
  expectAt(0, serial);
  serial = h.bus().state().navigation.serial;
  ASSERT_TRUE(h.panel->previousInstance(ctx));  // wraps to the last
  expectAt(static_cast<uint32_t>(n - 1), serial);
  serial = h.bus().state().navigation.serial;
  ASSERT_TRUE(h.panel->nextInstance(ctx));  // wraps to the first
  expectAt(0, serial);

  // The timeline follows: the view shows the instance, centred (away from the trace start, where the window
  // is clamped).
  serial = h.bus().state().navigation.serial;
  ASSERT_TRUE(h.panel->goToInstance(ctx, 50));
  expectAt(50, serial);
  h.frames(4);
  const timeline::TimeView view = h.widget().view();
  const auto& lim = h.widget().limits();
  const double centre =
      static_cast<double>(insts[50].begin + (insts[50].end - insts[50].begin) / 2 - lim.traceStart);
  EXPECT_GT(centre, view.start);
  EXPECT_LT(centre, view.end());
  EXPECT_NEAR(centre, view.start + view.span * 0.5, view.span * 0.2);
  EXPECT_LT(view.span, lim.total());
  // The current instance is part of the overlay and is flagged.
  bool currentDrawn = false;
  for (const timeline::HighlightRectInfo& r : h.widget().lastHighlightRects())
    currentDrawn = currentDrawn || r.current;
  EXPECT_TRUE(currentDrawn);
}

TEST(StatsWidget, ButtonsAndF3KeysStepThroughTheInstances) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  h.choose("Frame");
  ASSERT_TRUE(h.panel->result());
  h.frames(3);
  ASSERT_TRUE(h.panel->nextButton().drawn);
  ASSERT_TRUE(h.panel->previousButton().drawn);
  const auto centre = [](const FunctionStatsPanel::ButtonRect& b) {
    return ImVec2((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f);
  };
  ImVec2 c = centre(h.panel->nextButton());
  h.click(c.x, c.y);
  EXPECT_EQ(h.panel->currentInstance(), std::optional<uint32_t>(0u));
  c = centre(h.panel->nextButton());
  h.click(c.x, c.y);
  EXPECT_EQ(h.panel->currentInstance(), std::optional<uint32_t>(1u));
  c = centre(h.panel->previousButton());
  h.click(c.x, c.y);
  EXPECT_EQ(h.panel->currentInstance(), std::optional<uint32_t>(0u));

  h.key(ImGuiKey_F3);
  EXPECT_EQ(h.panel->currentInstance(), std::optional<uint32_t>(1u));
  h.key(ImGuiKey_F3);
  EXPECT_EQ(h.panel->currentInstance(), std::optional<uint32_t>(2u));
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
  h.key(ImGuiKey_F3);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
  h.frames(1);
  EXPECT_EQ(h.panel->currentInstance(), std::optional<uint32_t>(1u));
}

TEST(StatsWidget, PopulateFromSelectionTakesTheBusFunction) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  // Nothing selected: the button reports it.
  EXPECT_FALSE(h.panel->populateFromSelection(h.context()));
  EXPECT_FALSE(h.panel->message().empty());

  // A function chosen elsewhere (here: the bus, as the Call Graph / Function List do).
  const st::SearchEntry* physics = nullptr;
  for (const st::SearchEntry& e : h.panel->index()->entries)
    if (e.name == "game.exe!Engine::physics") physics = &e;
  ASSERT_NE(physics, nullptr);
  h.bus().setSelectedFunction(physics->key);
  ASSERT_TRUE(h.panel->populateButton().drawn);
  const auto& b = h.panel->populateButton();
  h.click((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f);
  ASSERT_TRUE(h.panel->selected().has_value());
  EXPECT_EQ(h.panel->selected()->key, physics->key);
  EXPECT_EQ(h.panel->queryText(), "game.exe!Engine::physics");
  h.settle();
  ASSERT_TRUE(h.panel->result());
  auto ref = h.refContext();
  auto want = analysis::functionStats(*ref, {}, physics->key);
  ASSERT_TRUE(want.ok());
  EXPECT_EQ(h.panel->result()->stats->count, want->count);
  EXPECT_GT(want->count, 0u);

  // Also a scope selected on the bus (by the timeline, say).
  const StringIdx jobRun = h.model().strings.find("JobRun");
  h.bus().setSelectedFunction(FunctionKey::scope(jobRun));
  EXPECT_TRUE(h.panel->populateFromSelection(h.context()));
  h.settle();
  EXPECT_EQ(h.panel->selected()->name, "JobRun");
  EXPECT_EQ(h.panel->result()->stats->count, 150u);
  // The Function List: choosing a row there and populating takes that function into the panel.
  h.entryOf(FunctionListPanel::kName)->open = true;
  ASSERT_TRUE(h.until([&] {
    return h.list->table() && !h.list->pending() && h.list->shownSpec() == h.list->requestedSpec() &&
           h.list->requestedSpec().symbolGeneration == h.app.symbolGeneration();
  }));
  const functions::Table& table = *h.list->table();
  uint32_t row = UINT32_MAX;
  for (uint32_t i = 0; i < table.entries.size(); ++i)
    if (table.entries[i].function == "Engine::update") row = i;
  ASSERT_NE(row, UINT32_MAX);
  h.list->selectEntry(h.context(), row);
  EXPECT_EQ(h.bus().state().selectedFunction, std::optional<FunctionKey>(table.entries[row].key));
  EXPECT_TRUE(h.panel->populateFromSelection(h.context()));
  EXPECT_EQ(h.panel->selected()->key, table.entries[row].key);
  EXPECT_EQ(h.panel->selected()->name, "game.exe!Engine::update");
  h.settle();
  ASSERT_TRUE(h.panel->result());
  EXPECT_GT(h.panel->result()->stats->count, 0u);
}

TEST(StatsWidget, CtrlFIsNotStolenFromAFocusedTextField) {
  Harness h;
  h.open("scopes.tachyon");
  // The Function List filter box is another text field; the statistics panel stays closed.
  PanelEntry* listEntry = h.entryOf(FunctionListPanel::kName);
  PanelEntry* statsEntry = h.entryOf(FunctionStatsPanel::kName);
  listEntry->open = true;
  h.frames(3);
  ImGui::SetWindowPos(FunctionListPanel::kName, ImVec2(0, 340));
  ImGui::SetWindowSize(FunctionListPanel::kName, ImVec2(700, 300));
  h.frames(3);
  const ImGuiWindow* w = ImGui::FindWindowByName(FunctionListPanel::kName);
  ASSERT_NE(w, nullptr);
  h.click(w->Pos.x + 60.0f, w->Pos.y + w->TitleBarHeight + ImGui::GetStyle().WindowPadding.y + 8.0f);
  h.frames(2);
  ASSERT_TRUE(ImGui::GetIO().WantTextInput) << "the click did not reach the Function List filter box";
  const uint64_t before = h.panel->focusRequests();
  h.chord(ImGuiMod_Ctrl, ImGuiKey_F);
  EXPECT_EQ(h.panel->focusRequests(), before);
  EXPECT_FALSE(statsEntry->open);
  // Out of the text field (click on the timeline) Ctrl+F works again.
  h.click(600.0f, 120.0f);
  h.frames(2);
  ASSERT_FALSE(ImGui::GetIO().WantTextInput);
  h.chord(ImGuiMod_Ctrl, ImGuiKey_F);
  EXPECT_TRUE(statsEntry->open);
  EXPECT_EQ(h.panel->focusRequests(), before + 1);
}

TEST(StatsWidget, RapidSelectionKeepsOnlyTheNewestResult) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  const StringIdx a = h.model().strings.find("JobRun");
  const StringIdx b = h.model().strings.find("Frame");
  std::vector<FunctionKey> published;
  h.bus().subscribe([&](const SelectionState& s, SelectionField f) {
    if (f == SelectionField::InstanceHighlight && s.instanceHighlight.active() && s.instanceHighlight.key)
      published.push_back(*s.instanceHighlight.key);
  });
  const PanelContext ctx = h.context();
  const auto indexOf = [&](StringIdx id) {
    const st::SearchEntry* e = h.panel->index()->find(FunctionKey::scope(id));
    return static_cast<uint32_t>(e - h.panel->index()->entries.data());
  };
  const uint64_t issued = h.panel->issuedStatsQueries();
  h.panel->selectEntry(ctx, indexOf(a));
  h.panel->update(ctx);  // A is issued
  h.panel->selectEntry(ctx, indexOf(b));
  h.panel->update(ctx);  // B supersedes it
  EXPECT_EQ(h.panel->issuedStatsQueries(), issued + 2);
  h.settle();
  ASSERT_TRUE(h.panel->result());
  EXPECT_EQ(h.panel->result()->key, FunctionKey::scope(b));
  EXPECT_EQ(h.panel->result()->stats->count, 99u);
  ASSERT_FALSE(published.empty());
  for (const FunctionKey& k : published)
    EXPECT_EQ(k, FunctionKey::scope(b)) << "a stale result was published";
  EXPECT_EQ(h.bus().state().instanceHighlight.key, std::optional<FunctionKey>(FunctionKey::scope(b)));
}

TEST(StatsWidget, SymbolGenerationChangeRebuildsTheIndexAndKeepsTheSelection) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  h.choose("game.exe!Engine::update");
  const FunctionKey key = h.panel->selected()->key;
  const uint64_t indexQueries = h.panel->issuedIndexQueries();
  const uint64_t statsQueries = h.panel->issuedStatsQueries();
  h.panel->disableThrottle();
  PanelContext ctx = h.context();
  ctx.symbolGeneration += 1;
  h.panel->update(ctx);
  EXPECT_EQ(h.panel->issuedIndexQueries(), indexQueries + 1);
  EXPECT_EQ(h.panel->issuedStatsQueries(), statsQueries + 1);
  EXPECT_EQ(h.panel->requestedIndexSymbolGeneration(), ctx.symbolGeneration);
  // Deliver both on the new generation (no app frames: they would update the panel with the old one).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (h.panel->indexPending() || h.panel->statsPending()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    h.panel->update(ctx);
  }
  h.panel->update(ctx);
  EXPECT_EQ(h.panel->indexSymbolGeneration(), ctx.symbolGeneration);
  ASSERT_TRUE(h.panel->selected().has_value());
  EXPECT_EQ(h.panel->selected()->key, key);
  ASSERT_TRUE(h.panel->result());
  EXPECT_GT(h.panel->result()->stats->count, 0u);
}

TEST(StatsWidget, ClosingTheDocumentDropsEverything) {
  Harness h;
  h.open("scopes.tachyon");
  h.openPanel();
  h.choose("Frame");
  ASSERT_TRUE(h.bus().state().instanceHighlight.active());
  h.panel->onDocumentChanged(nullptr);
  EXPECT_FALSE(h.panel->selected().has_value());
  EXPECT_EQ(h.panel->result(), nullptr);
  EXPECT_EQ(h.panel->index(), nullptr);
  EXPECT_TRUE(h.panel->hits().empty());
}
