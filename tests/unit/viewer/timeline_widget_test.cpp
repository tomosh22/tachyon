// WP-42: the threads timeline panel driven headless (Null renderer, real ImGui frames, injected input events)
// on the tiny / medium / frames fixtures.
#include "viewer/app.h"
#include "viewer/timeline/timeline_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

AppOptions headlessOptions() {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_timeline_test_no_such_settings.json";
  return o;
}

class Harness {
 public:
  explicit Harness(bool deferBuild = false) : app(makeNullRenderer(), headlessOptions()) {
    std::string error;
    EXPECT_TRUE(app.init(error)) << error;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // the Null backend does not set it
    io.ConfigInputTrickleEventQueue = false;
    auto* entry = app.panels().find(ThreadsTimelinePanel::kName);
    EXPECT_NE(entry, nullptr);
    panel = entry ? dynamic_cast<ThreadsTimelinePanel*>(entry->panel.get()) : nullptr;
    EXPECT_NE(panel, nullptr);
    if (panel) panel->widget().options().deferLodBuild = deferBuild;
    for (PanelEntry& e : app.panels().entries()) {
      if (e.panel.get() != panel) e.open = false;  // keep only the timeline on screen
    }
  }

  TimelineWidget& widget() { return panel->widget(); }
  SelectionBus& bus() { return app.selection(); }

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

  // Opens a fixture and runs until the document is shown (and, unless deferred, the LOD is built).
  void open(const char* name, bool waitForLod = true) {
    app.openFile(fixture(name));
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    frames(2);
    if (waitForLod) {
      ASSERT_TRUE(until([&] { return widget().lodFinished(); }));
      frames(2);
    }
  }

  float ui() const { return ImGui::GetFontSize() / 13.0f; }
  float plotCenterX() {
    return widget().screenGeometry().plotX0 + widget().screenGeometry().plotWidth * 0.5f;
  }
  // Screen y inside the state band of row `index` (rows are laid out from the content origin).
  float bandY(size_t index = 0) {
    float y = widget().screenGeometry().rowsContentY0;
    const auto& order = widget().rowOrder();
    const float collapsed = (2 + 12 + 10 + 2 * 0) * ui();
    for (size_t i = 0; i < index && i < order.size(); ++i) {
      y += collapsed + 2.0f * ui();
      if (widget().flagsOf(order[i]).expanded) y += 1e6f;  // tests with expanded rows aim at row 0 only
    }
    return y + 5.0f * ui();
  }
  float stripY(size_t index = 0) { return bandY(index) + 12.0f * ui(); }

  void mouse(float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    frames(1);
  }
  void button(int b, bool down) {
    ImGui::GetIO().AddMouseButtonEvent(b, down);
    frames(1);
  }
  void wheel(float dy) {
    ImGui::GetIO().AddMouseWheelEvent(0.0f, dy);
    frames(1);
  }
  void key(ImGuiKey k) {
    ImGui::GetIO().AddKeyEvent(k, true);
    frames(1);
    ImGui::GetIO().AddKeyEvent(k, false);
    frames(1);
  }
  void dragFrom(float x0, float y, float x1, int b = 0) {
    mouse(x0, y);
    button(b, true);
    const int steps = 4;
    for (int i = 1; i <= steps; ++i) mouse(x0 + (x1 - x0) * static_cast<float>(i) / steps, y);
    button(b, false);
  }

  App app;
  ThreadsTimelinePanel* panel = nullptr;
};

}  // namespace

TEST(TimelineWidget, DrawsNothingWithoutADocument) {
  Harness h;
  h.frames(3);
  EXPECT_FALSE(h.widget().hasDocument());
  EXPECT_TRUE(h.widget().rowOrder().empty());
  EXPECT_EQ(h.widget().frameStats().rowsDrawn, 0u);
}

TEST(TimelineWidget, OpensAndDrawsAllRowsOfTheTinyTrace) {
  Harness h;
  h.open("tiny.tachyon");
  ASSERT_TRUE(h.widget().hasDocument());
  const auto& m = h.app.document()->model();
  EXPECT_EQ(h.widget().rowOrder().size(), m.threads.size());
  EXPECT_TRUE(h.widget().lodFinished());
  EXPECT_EQ(h.widget().lodReadyThreads(), m.threads.size());
  EXPECT_TRUE(h.widget().lodError().empty());
  const TimelineFrameStats& s = h.widget().frameStats();
  EXPECT_EQ(s.rowsTotal, m.threads.size());
  EXPECT_GT(s.rowsDrawn, 0u);
  EXPECT_EQ(s.rowsPending, 0u);
  EXPECT_GT(s.rects, 0u);
  EXPECT_EQ(h.widget().view(), fitView(h.widget().limits()));
  const TimelineScreenGeometry& g = h.widget().screenGeometry();
  EXPECT_GT(g.plotWidth, 100.0f);
  EXPECT_GT(g.rowsY1, g.rowsY0);
}

TEST(TimelineWidget, RowsAreSortedByRunningTimeMostFirst) {
  Harness h;
  h.open("medium.tachyon");
  const auto& m = h.app.document()->model();
  const std::vector<ThreadIdx> order = h.widget().rowOrder();  // a copy: the widget rewrites its vector
  ASSERT_EQ(order.size(), m.threads.size());
  const auto running = [&](ThreadIdx t) {
    uint64_t sum = 0;
    for (const auto& s : m.statesByThread[t.get()]) {
      if (s.state == model::ThreadState::Running) sum += s.end - s.start;
    }
    return sum;
  };
  for (size_t i = 1; i < order.size(); ++i) EXPECT_GE(running(order[i - 1]), running(order[i]));
  // Changing the sort key re-orders on the next frame.
  h.widget().options().sortKey = SortKey::ThreadId;
  h.widget().setPinned(order.back(), true);  // marks the order dirty
  h.frames(2);
  EXPECT_EQ(h.widget().rowOrder().front(), order.back());  // pinned rows first
  for (size_t i = 2; i < h.widget().rowOrder().size(); ++i) {
    EXPECT_LT(m.threads[h.widget().rowOrder()[i - 1].get()].tid,
              m.threads[h.widget().rowOrder()[i].get()].tid);
  }
}

TEST(TimelineWidget, RowsLookPendingUntilTheirThreadIsReady) {
  Harness h(/*deferBuild=*/true);
  h.open("medium.tachyon", /*waitForLod=*/false);
  h.frames(3);
  const TimelineFrameStats pending = h.widget().frameStats();
  EXPECT_GT(pending.rowsDrawn, 0u);
  EXPECT_EQ(pending.rowsPending, pending.rowsDrawn);
  EXPECT_EQ(pending.rects, 0u);
  EXPECT_FALSE(h.widget().lodFinished());
  // Hover and click on a pending row are harmless.
  h.mouse(h.plotCenterX(), h.bandY());
  h.button(0, true);
  h.button(0, false);
  h.frames(2);
  // Start the build: the viewer stays interactive, rows resolve one by one, finally none is pending.
  h.widget().options().deferLodBuild = false;
  ASSERT_TRUE(h.until([&] { return h.widget().lodFinished(); }));
  h.frames(3);
  EXPECT_EQ(h.widget().frameStats().rowsPending, 0u);
  EXPECT_GT(h.widget().frameStats().rects, 0u);
}

TEST(TimelineWidget, InlineBuildWorksWithoutAJobQueue) {
  Harness h;
  h.widget().options().buildInline = true;
  h.open("tiny.tachyon", /*waitForLod=*/false);
  h.frames(2);
  EXPECT_TRUE(h.widget().lodFinished());
  EXPECT_EQ(h.widget().frameStats().rowsPending, 0u);
}

TEST(TimelineWidget, WheelZoomsAtTheCursor) {
  Harness h;
  h.open("medium.tachyon");
  const TimelineScreenGeometry g = h.widget().screenGeometry();
  const float x = g.plotX0 + g.plotWidth * 0.25f;
  h.mouse(x, h.bandY());
  const TimeView before = h.widget().view();
  const double tickBefore = xToRelTick(before, x, g.plotX0, g.plotWidth);
  h.wheel(3.0f);
  const TimeView after = h.widget().view();
  EXPECT_NEAR(after.span, before.span / std::pow(1.25, 3.0), 1.0);
  EXPECT_NEAR(xToRelTick(after, x, g.plotX0, g.plotWidth), tickBefore, after.span / g.plotWidth * 2);
  // And back out; the view clamps at the whole trace.
  h.wheel(-30.0f);
  EXPECT_EQ(h.widget().view(), fitView(h.widget().limits()));
}

TEST(TimelineWidget, ShiftWheelPansAndDragPans) {
  Harness h;
  h.open("medium.tachyon");
  h.widget().setView({0, h.widget().limits().total() / 8});
  h.frames(2);
  const TimelineScreenGeometry g = h.widget().screenGeometry();
  h.mouse(h.plotCenterX(), h.bandY());
  const TimeView start = h.widget().view();

  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
  h.wheel(-1.0f);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
  h.frames(1);
  EXPECT_GT(h.widget().view().start, start.start);

  const double s0 = h.widget().view().start;
  h.dragFrom(h.plotCenterX(), h.bandY(), h.plotCenterX() - 100.0f);  // drag left: later time comes into view
  const double expected = s0 + 100.0 * start.span / g.plotWidth;
  EXPECT_NEAR(h.widget().view().start, expected, start.span / g.plotWidth * 2);
  EXPECT_TRUE(h.widget().selectionRange() == std::nullopt);  // plain drag never selects

  const double s1 = h.widget().view().start;
  h.dragFrom(h.plotCenterX(), h.bandY(), h.plotCenterX() + 50.0f, ImGuiMouseButton_Middle);
  EXPECT_NEAR(h.widget().view().start, s1 - 50.0 * start.span / g.plotWidth, start.span / g.plotWidth * 2);
}

TEST(TimelineWidget, DragSelectThenFAppliesTheFilterAndEscClearsIt) {
  Harness h;
  h.open("medium.tachyon");
  std::vector<SelectionField> events;
  h.bus().subscribe([&](const SelectionState&, SelectionField f) { events.push_back(f); });
  const TimelineScreenGeometry g = h.widget().screenGeometry();

  // Drag on the ruler selects.
  const float rulerY = g.headerY0 + 5.0f;
  h.dragFrom(g.plotX0 + g.plotWidth * 0.25f, rulerY, g.plotX0 + g.plotWidth * 0.5f);
  ASSERT_TRUE(h.widget().selectionRange().has_value());
  const TickRange sel = *h.widget().selectionRange();
  const TimeLimits& l = h.widget().limits();
  EXPECT_NEAR(toRelative(l, sel.t0), l.total() * 0.25, l.total() / g.plotWidth * 2);
  EXPECT_NEAR(toRelative(l, sel.t1), l.total() * 0.5, l.total() / g.plotWidth * 2);
  EXPECT_FALSE(h.bus().state().timeFilter.has_value());  // not applied yet

  // F applies the selection as the time filter (the "time-range selection field").
  h.mouse(h.plotCenterX(), h.bandY());
  events.clear();
  h.key(ImGuiKey_F);
  ASSERT_TRUE(h.bus().state().timeFilter.has_value());
  EXPECT_EQ(h.bus().state().timeFilter->t0, sel.t0);
  EXPECT_EQ(h.bus().state().timeFilter->t1, sel.t1);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front(), SelectionField::TimeFilter);
  h.frames(2);  // draws the filter overlay and the status line

  // Esc clears both.
  h.key(ImGuiKey_Escape);
  EXPECT_FALSE(h.widget().selectionRange().has_value());
  EXPECT_FALSE(h.bus().state().timeFilter.has_value());
}

TEST(TimelineWidget, ShiftDragInTheRowsSelectsToo) {
  Harness h;
  h.open("tiny.tachyon");
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
  h.frames(1);
  h.dragFrom(h.plotCenterX() - 80.0f, h.stripY(), h.plotCenterX() + 80.0f);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
  h.frames(1);
  ASSERT_TRUE(h.widget().selectionRange().has_value());
  EXPECT_GT(h.widget().selectionRange()->t1, h.widget().selectionRange()->t0);
  // Programmatic apply keeps other filter fields.
  ViewFilter f;
  f.t0 = 1;
  f.t1 = 2;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  h.bus().setTimeFilter(f);
  EXPECT_TRUE(h.widget().applySelectionAsFilter(h.bus()));
  ASSERT_TRUE(h.bus().state().timeFilter->threads.has_value());
  EXPECT_EQ(h.bus().state().timeFilter->t0, h.widget().selectionRange()->t0);
  h.widget().clearSelection(&h.bus());
  EXPECT_FALSE(h.bus().state().timeFilter.has_value());
  EXPECT_FALSE(h.widget().applySelectionAsFilter(h.bus()));
}

TEST(TimelineWidget, ClickSelectsTheThreadAndTheFunctionUnderTheCursor) {
  Harness h;
  h.open("medium.tachyon");
  const ThreadIdx first = h.widget().rowOrder().front();
  bool selected = false;
  const TimelineScreenGeometry g = h.widget().screenGeometry();
  for (float f = 0.1f; f < 0.95f && !selected; f += 0.05f) {
    const float x = g.plotX0 + g.plotWidth * f;
    h.mouse(x, h.stripY());
    // Hover publishes the item under the cursor.
    if (h.bus().state().hovered.kind == HoveredItem::Kind::Callstack) {
      EXPECT_EQ(h.bus().state().hovered.thread, first);
      EXPECT_TRUE(h.bus().state().hovered.callstack.valid());
      EXPECT_GT(h.bus().state().hovered.ts, h.widget().limits().traceStart);
    }
    h.button(0, true);
    h.button(0, false);
    h.frames(1);
    selected = h.bus().state().selectedFunction.has_value();
  }
  ASSERT_TRUE(selected) << "no sampled column found in the first row";
  EXPECT_EQ(h.bus().state().selectedThread, first);
  EXPECT_TRUE(h.bus().state().selectedFunction->module.valid());
  h.frames(2);  // draws with a selected function and thread

  // Leaving the plot clears the hover.
  h.mouse(g.plotX0 + g.plotWidth * 0.5f, g.headerY0 - 40.0f);
  h.frames(1);
  EXPECT_EQ(h.bus().state().hovered.kind, HoveredItem::Kind::None);
}

TEST(TimelineWidget, KeyboardNavigation) {
  Harness h;
  h.open("medium.tachyon");
  h.mouse(h.plotCenterX(), h.bandY());  // hovering the plot gives the timeline the keyboard
  const TimeView fit = h.widget().view();
  h.key(ImGuiKey_W);
  EXPECT_LT(h.widget().view().span, fit.span);
  const TimeView zoomed = h.widget().view();
  h.key(ImGuiKey_D);
  EXPECT_GT(h.widget().view().start, zoomed.start);
  EXPECT_DOUBLE_EQ(h.widget().view().span, zoomed.span);
  h.key(ImGuiKey_A);
  EXPECT_NEAR(h.widget().view().start, zoomed.start, 1e-6);
  h.key(ImGuiKey_S);
  EXPECT_GT(h.widget().view().span, zoomed.span);
  h.key(ImGuiKey_Home);
  EXPECT_EQ(h.widget().view(), fit);

  // Z zooms to the selection.
  const TimeLimits& l = h.widget().limits();
  h.widget().setSelectionRange(TickRange{l.traceStart + 1000, l.traceStart + 2000});
  h.key(ImGuiKey_Z);
  EXPECT_LT(h.widget().view().span, 1200.0);
  EXPECT_GT(h.widget().view().span, 1000.0);

  // Up / Down walk the selected thread through the visible rows.
  h.widget().fit();
  const auto order = h.widget().rowOrder();
  h.key(ImGuiKey_DownArrow);
  EXPECT_EQ(h.bus().state().selectedThread, order[0]);
  h.key(ImGuiKey_DownArrow);
  EXPECT_EQ(h.bus().state().selectedThread, order[1]);
  h.key(ImGuiKey_UpArrow);
  EXPECT_EQ(h.bus().state().selectedThread, order[0]);
  h.key(ImGuiKey_E);
  EXPECT_TRUE(h.widget().flagsOf(order[0]).expanded);
  h.key(ImGuiKey_P);
  EXPECT_TRUE(h.widget().flagsOf(order[0]).pinned);
}

TEST(TimelineWidget, ExpandedRowQueriesTheFlameChartAndPinHideReorder) {
  Harness h;
  h.open("medium.tachyon");
  const auto order = h.widget().rowOrder();
  ASSERT_GE(order.size(), 4u);
  h.frames(1);
  EXPECT_EQ(h.widget().frameStats().flameQueries, 0u);

  h.widget().setExpanded(order[0], true);
  h.frames(2);
  // The query happened on the first frame after expanding; later frames reuse the cache.
  h.widget().setView({0, h.widget().limits().total() / 2});
  h.frames(1);
  EXPECT_EQ(h.widget().frameStats().flameQueries, 1u);
  EXPECT_GT(h.widget().frameStats().flameSpans, 0u);
  h.frames(1);
  EXPECT_EQ(h.widget().frameStats().flameQueries, 0u);  // unchanged view: nothing re-queried
  EXPECT_EQ(h.widget().frameStats().bandQueries, 0u);

  // Hovering a flame depth gives a tooltip without trouble.
  h.mouse(h.plotCenterX(), h.bandY() + 12.0f * h.ui() + 10.0f * h.ui() + 6.0f * h.ui());
  h.frames(2);

  // Pin the last thread: first row. Hide the (new) second row: dropped from the order.
  h.widget().setPinned(order.back(), true);
  h.frames(1);
  EXPECT_EQ(h.widget().rowOrder().front(), order.back());
  const ThreadIdx hidden = h.widget().rowOrder()[1];
  h.widget().setHidden(hidden, true);
  h.frames(1);
  EXPECT_EQ(h.widget().rowOrder().size(), order.size() - 1);
  EXPECT_EQ(std::count(h.widget().rowOrder().begin(), h.widget().rowOrder().end(), hidden), 0);
  h.widget().setHidden(hidden, false);
  h.frames(1);
  EXPECT_EQ(h.widget().rowOrder().size(), order.size());
}

TEST(TimelineWidget, ContextMenuAndGutterInteraction) {
  Harness h;
  h.open("tiny.tachyon");
  const TimelineScreenGeometry g = h.widget().screenGeometry();
  const ThreadIdx first = h.widget().rowOrder().front();
  // Gutter tooltip and click select the thread.
  h.mouse(g.gutterX0 + 20.0f, h.bandY());
  h.frames(2);
  h.button(0, true);
  h.button(0, false);
  EXPECT_EQ(h.bus().state().selectedThread, first);
  // Right click opens the menu; drawing with it open must be fine.
  h.button(1, true);
  h.button(1, false);
  h.frames(3);
  EXPECT_GT(ImGui::GetCurrentContext()->OpenPopupStack.Size, 0);
  h.key(ImGuiKey_Escape);
  h.frames(2);
}

TEST(TimelineWidget, OverviewDragMovesTheView) {
  Harness h;
  h.open("medium.tachyon");
  h.widget().setView({0, h.widget().limits().total() / 10});
  h.frames(2);
  const TimelineScreenGeometry g = h.widget().screenGeometry();
  const float y = (g.overviewY0 + g.overviewY1) * 0.5f;
  h.dragFrom(g.plotX0 + 5.0f, y, g.plotX0 + g.plotWidth * 0.5f);
  const TimeView v = h.widget().view();
  EXPECT_NEAR(v.start + v.span / 2, h.widget().limits().total() * 0.5,
              h.widget().limits().total() / g.plotWidth * 2);
  EXPECT_NEAR(v.span, h.widget().limits().total() / 10, 1.0);
  h.widget().fit();
  h.frames(1);
}

TEST(TimelineWidget, FrameMarksAreDrawnAsTicks) {
  Harness h;
  h.open("frames.tachyon");
  ASSERT_FALSE(h.app.document()->model().frameMarks.empty());
  EXPECT_GT(h.widget().frameStats().frameMarksDrawn, 0u);
  // Zooming way in keeps working (ticks outside the view are skipped by binary search).
  h.widget().setView({0, 5000});
  h.frames(2);
  h.widget().fit();
  h.frames(1);
  EXPECT_GT(h.widget().frameStats().frameMarksDrawn, 0u);
}

TEST(TimelineWidget, FollowsTheDocumentAcrossOpenAndClose) {
  Harness h;
  h.open("tiny.tachyon");
  const size_t tinyRows = h.widget().rowOrder().size();
  h.widget().setExpanded(h.widget().rowOrder()[0], true);
  h.widget().setView({10, 100});
  h.widget().setSelectionRange(TickRange{1, 2});
  h.app.commands().run("file.close");
  h.frames(2);
  EXPECT_FALSE(h.widget().hasDocument());
  EXPECT_TRUE(h.widget().rowOrder().empty());
  EXPECT_FALSE(h.widget().selectionRange().has_value());

  h.open("medium.tachyon");
  EXPECT_GT(h.widget().rowOrder().size(), tinyRows);
  EXPECT_EQ(h.widget().view(), fitView(h.widget().limits()));  // not the stale view
  EXPECT_FALSE(h.widget().flagsOf(h.widget().rowOrder()[0]).expanded);
  EXPECT_EQ(h.widget().frameStats().rowsPending, 0u);

  // Re-opening another file while the first LOD may still be building is safe.
  h.app.openFile(fixture("tiny.tachyon"));
  ASSERT_TRUE(h.until([&] { return !h.app.loading(); }));
  h.frames(2);
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until([&] { return !h.app.loading(); }));
  h.frames(2);  // the build of the new document starts; the old job is cancelled
  h.app.commands().run("file.close");
  h.frames(2);
  ASSERT_TRUE(h.until([&] { return h.app.jobs().activeCount() == 0; }));
}

TEST(TimelineWidget, ExternalTimeFilterIsShownAndSurvivesRedraw) {
  Harness h;
  h.open("tiny.tachyon");
  const TimeLimits& l = h.widget().limits();
  ViewFilter f;
  f.t0 = l.traceStart + 100;
  f.t1 = l.traceStart + 5000;
  h.bus().setTimeFilter(f);
  h.frames(3);
  EXPECT_EQ(h.bus().state().timeFilter->t0, f.t0);
  h.bus().setSelectedFunction(FunctionKey{ModuleIdx(0), 0x1000});
  h.frames(2);
  h.mouse(h.plotCenterX(), h.bandY());
  h.key(ImGuiKey_Z);  // with no local selection, Z zooms to the filter
  EXPECT_LT(h.widget().view().span, 6000.0);
}

TEST(TimelineWidget, ColorModeAndToolbarOptionsRedrawCleanly) {
  Harness h;
  h.open("tiny.tachyon");
  h.widget().options().colorMode = FrameColorMode::ByFunction;
  h.widget().options().columnPixels = 1;
  h.frames(2);
  const uint32_t fine = h.widget().frameStats().rects;
  h.widget().options().columnPixels = 4;
  h.widget().fit();
  h.frames(2);
  EXPECT_GT(fine, 0u);
  h.widget().options().showFrameMarks = false;
  h.frames(2);
  EXPECT_EQ(h.widget().frameStats().frameMarksDrawn, 0u);
}
