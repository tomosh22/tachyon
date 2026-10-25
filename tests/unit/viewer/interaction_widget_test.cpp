// WP-46: thread interaction driven headless (Null renderer, real ImGui frames, injected input) on the tiny
// and medium fixtures: hover -> arrow, click -> panel, Ctrl-hover tooltip, navigation, longest waits.
#include "viewer/app.h"
#include "viewer/interaction/interaction_panel.h"
#include "viewer/timeline/timeline_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;
namespace ia = tachyon::viewer::interaction;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

AppOptions headlessOptions() {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_interaction_test_no_such_settings.json";
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
    auto* entry = app.panels().find(ThreadsTimelinePanel::kName);
    panel = entry ? dynamic_cast<ThreadsTimelinePanel*>(entry->panel.get()) : nullptr;
    EXPECT_NE(panel, nullptr);
    interactionEntry = app.panels().find(ThreadInteractionPanel::kName);
    EXPECT_NE(interactionEntry, nullptr);
    interactionPanel =
        interactionEntry ? dynamic_cast<ThreadInteractionPanel*>(interactionEntry->panel.get()) : nullptr;
    EXPECT_NE(interactionPanel, nullptr);
    for (PanelEntry& e : app.panels().entries()) {
      if (e.panel.get() != panel) e.open = false;  // only the timeline on screen: no window covers the rows
    }
  }

  TimelineWidget& widget() { return panel->widget(); }
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

  void open(const char* name) {
    app.openFile(fixture(name));
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    frames(2);
    ASSERT_TRUE(until([&] { return widget().lodFinished(); }));
    frames(2);
  }
  void waitSymbols() {
    const symbols::SymbolService* svc = app.document()->symbols().get();
    ASSERT_NE(svc, nullptr);
    ASSERT_TRUE(until([&] {
      const symbols::SymbolProgress p = svc->progress();
      return p.total > 0 && p.done == p.total;
    }));
    frames(3);
  }

  void mouse(float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    frames(1);
  }
  void click(float x, float y) {
    mouse(x, y);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frames(1);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frames(1);
  }
  void key(ImGuiKey k) {
    ImGui::GetIO().AddKeyEvent(k, true);
    frames(1);
    ImGui::GetIO().AddKeyEvent(k, false);
    frames(1);
  }

  ThreadIdx thread(uint32_t tid) { return model().tidToThread.at(tid); }
  float xOf(uint64_t ts) {
    const TimelineScreenGeometry& g = widget().screenGeometry();
    return static_cast<float>(
        relTickToX(widget().view(), toRelative(widget().limits(), ts), g.plotX0, g.plotWidth));
  }
  float yOf(ThreadIdx t) {
    const auto y = widget().bandCenterY(t);
    EXPECT_TRUE(y.has_value());
    return y.value_or(0.0f);
  }
  // Shows [t0 - pad, t1 + pad] and lets the LOD queries refresh.
  void show(uint64_t t0, uint64_t t1, uint64_t pad) {
    widget().setView(viewFromRange(widget().limits(), {t0 - pad, t1 + pad}, 0.0));
    frames(3);
  }
  PanelContext context() {
    PanelContext c;
    c.document = app.document().get();
    c.selection = &app.selection();
    c.symbols = app.document() ? app.document()->symbols().get() : nullptr;
    return c;
  }

  App app;
  ThreadsTimelinePanel* panel = nullptr;
  PanelEntry* interactionEntry = nullptr;
  ThreadInteractionPanel* interactionPanel = nullptr;
};

// tiny.json: Main (tid 101) waits 8 ms at +40 ms, woken by Worker (tid 102) after 7.5 ms; the 20 ms wait at
// +348.1 ms has no ReadyRec. 10 MHz clock.
constexpr uint64_t kFirstWaitOut = 400'000;
constexpr uint64_t kFirstWaitReadyAfter = 75'000;
constexpr uint64_t kFirstWaitLength = 80'000;
constexpr uint64_t kSecondWaitOut = 3'481'000;

}  // namespace

TEST(InteractionWidget, HoveringAWakenWaitDrawsTheArrowFromTheWakerToTheSwitchIn) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101), worker = h.thread(102);
  h.show(base + kFirstWaitOut, base + kFirstWaitOut + kFirstWaitLength, 20'000);

  const uint64_t out = base + kFirstWaitOut;
  h.mouse(h.xOf(out + 30'000), h.yOf(main));
  ASSERT_EQ(h.widget().lastArrows().size(), 1u);
  const WakeArrowInfo& a = h.widget().lastArrows()[0];
  EXPECT_FALSE(a.selected);
  EXPECT_EQ(a.episode.thread, main);
  EXPECT_EQ(a.episode.switchOutTs, out);
  EXPECT_EQ(a.episode.wakerThread, worker);
  ASSERT_TRUE(a.geometry.visible);
  // From the waker's row at the ReadyRec position to the woken row at the switch-in.
  EXPECT_NEAR(a.geometry.from.x, h.xOf(out + kFirstWaitReadyAfter), 0.5f);
  EXPECT_NEAR(a.geometry.from.y, h.yOf(worker), 0.5f);
  EXPECT_NEAR(a.geometry.to.x, h.xOf(out + kFirstWaitLength), 0.5f);
  EXPECT_NEAR(a.geometry.to.y, h.yOf(main), 0.5f);
  EXPECT_FALSE(a.geometry.fromClipped);
  EXPECT_FALSE(a.geometry.toClipped);

  // The Ready piece of the same episode gives the same arrow.
  h.mouse(h.xOf(out + kFirstWaitReadyAfter + 2'000), h.yOf(main));
  ASSERT_EQ(h.widget().lastArrows().size(), 1u);
  EXPECT_EQ(h.widget().lastArrows()[0].episode.switchOutTs, out);

  // Leaving the plot (gutter) removes it; nothing is selected, so nothing persists.
  h.mouse(h.widget().screenGeometry().gutterX0 + 5.0f, h.yOf(main));
  EXPECT_TRUE(h.widget().lastArrows().empty());
}

TEST(InteractionWidget, NoArrowOnRunningIntervalsOrWaitsWithoutAReadyRec) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101);
  h.show(base + kFirstWaitOut - 100'000, base + kFirstWaitOut + 200'000, 0);
  h.mouse(h.xOf(base + kFirstWaitOut + 150'000), h.yOf(main));  // Running after the wait
  EXPECT_TRUE(h.widget().lastArrows().empty());

  h.show(base + kSecondWaitOut, base + kSecondWaitOut + 200'000, 50'000);
  h.mouse(h.xOf(base + kSecondWaitOut + 100'000), h.yOf(main));  // the 20 ms wait: nobody woke it
  EXPECT_TRUE(h.widget().lastArrows().empty());
  const auto e = ia::episodeAt(h.model(), main, base + kSecondWaitOut + 100'000);
  ASSERT_TRUE(e);
  EXPECT_FALSE(e->hasWaker);
}

TEST(InteractionWidget, HiddenWakerRowGivesAClippedArrowWithTheWakerNameAtTheEdge) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101), worker = h.thread(102);
  h.widget().setHidden(worker, true);
  h.show(base + kFirstWaitOut, base + kFirstWaitOut + kFirstWaitLength, 20'000);
  h.mouse(h.xOf(base + kFirstWaitOut + 30'000), h.yOf(main));
  ASSERT_EQ(h.widget().lastArrows().size(), 1u);
  const WakeArrowInfo& a = h.widget().lastArrows()[0];
  EXPECT_TRUE(a.geometry.wakerHidden);
  EXPECT_EQ(a.geometry.markerSide, ia::EdgeSide::Top);
  EXPECT_NE(a.wakerLabel.find("Worker"), std::string::npos);
  EXPECT_NE(a.wakerLabel.find("hidden"), std::string::npos);
  EXPECT_NEAR(a.geometry.from.x, h.xOf(base + kFirstWaitOut + kFirstWaitReadyAfter), 0.5f);
  EXPECT_FLOAT_EQ(a.geometry.from.y, h.widget().screenGeometry().rowsY0);
}

TEST(InteractionWidget, ScrolledOffWakerRowIsClippedAtTheViewEdge) {
  Harness h;
  h.open("medium.tachyon");
  const model::TraceModel& m = h.model();
  for (uint32_t t = 0; t < m.threads.size(); ++t) h.widget().setExpanded(ThreadIdx(t), true);
  h.frames(3);
  // A wait whose waker row is at least 6 rows away from the woken one.
  ia::WaitQuery q;
  q.t0 = m.header.start_ts;
  q.maxResults = 100000;
  const std::vector<ThreadIdx> order = h.widget().rowOrder();
  const auto pos = [&](ThreadIdx t) { return std::find(order.begin(), order.end(), t) - order.begin(); };
  std::optional<ia::WaitEpisode> pick;
  for (const ia::WaitEpisode& e : ia::longestWaits(m, q, nullptr).rows) {
    if (e.hasReadyRec && e.hasWaker && e.wakerThread.valid() &&
        std::abs(pos(e.thread) - pos(e.wakerThread)) >= 6) {
      pick = e;
      break;
    }
  }
  ASSERT_TRUE(pick.has_value());
  const ia::WaitEpisode e = *pick;
  h.bus().setSelectedWait(WaitSelection{e.thread, e.switchOutTs});
  h.bus().requestNavigation(e.thread, e.switchOutTs, e.switchInTs);
  h.frames(4);
  const TimelineScreenGeometry g = h.widget().screenGeometry();
  const auto wokenY = h.widget().bandCenterY(e.thread);
  const auto wakerY = h.widget().bandCenterY(e.wakerThread);
  ASSERT_TRUE(wokenY && wakerY);
  EXPECT_GE(*wokenY, g.rowsY0);  // navigation scrolled the woken row into view
  EXPECT_LE(*wokenY, g.rowsY1);
  ASSERT_FALSE(*wakerY >= g.rowsY0 && *wakerY <= g.rowsY1) << "the waker row should be off screen";
  ASSERT_EQ(h.widget().lastArrows().size(), 1u);
  const WakeArrowInfo& a = h.widget().lastArrows()[0];
  EXPECT_TRUE(a.selected);
  EXPECT_TRUE(a.geometry.fromClipped);
  EXPECT_FALSE(a.geometry.wakerHidden);
  EXPECT_EQ(a.geometry.markerSide, *wakerY < g.rowsY0 ? ia::EdgeSide::Top : ia::EdgeSide::Bottom);
  EXPECT_NE(a.wakerLabel.find("(tid "), std::string::npos);
}

TEST(InteractionWidget, ClickOpensThePanelWithBlockingAndUnblockingStacksAndTheFields) {
  Harness h;
  h.open("tiny.tachyon");
  h.waitSymbols();
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101), worker = h.thread(102);
  const uint64_t out = base + kFirstWaitOut;
  h.show(out, out + kFirstWaitLength, 20'000);
  ASSERT_FALSE(h.interactionEntry->open);

  h.click(h.xOf(out + 30'000), h.yOf(main));
  EXPECT_TRUE(h.interactionEntry->open);
  EXPECT_GE(h.interactionPanel->focusRequests(), 1u);
  ASSERT_TRUE(h.bus().state().selectedWait.has_value());
  EXPECT_EQ(h.bus().state().selectedWait->thread, main);
  EXPECT_EQ(h.bus().state().selectedWait->switchOutTs, out);
  EXPECT_EQ(h.bus().state().selectedThread, main);

  h.frames(3);                                                                  // the panel is drawn
  ImGui::SetWindowPos(ThreadInteractionPanel::kName, ImVec2(1270.0f, 710.0f));  // out of the timeline's way
  h.frames(2);
  const auto& e = h.interactionPanel->episode();
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->switchOutTs, out);
  EXPECT_EQ(e->wakerThread, worker);
  EXPECT_EQ(e->waitTicks, kFirstWaitReadyAfter);
  EXPECT_EQ(e->readyLatencyTicks, kFirstWaitLength - kFirstWaitReadyAfter);
  const ia::EpisodeDetails& d = h.interactionPanel->details();
  EXPECT_EQ(d.woken, "Main (tid 101)");
  EXPECT_NE(d.waker.find("(tid 102)"), std::string::npos);
  EXPECT_EQ(d.waitDuration, "7.50 ms");
  EXPECT_EQ(d.readyLatency, "500 us");

  // Blocking stack: leaf first, resolved through the symbol service (exact leaf, return addresses above).
  const auto& blocking = h.interactionPanel->blockingStack();
  ASSERT_EQ(blocking.size(), 4u);
  EXPECT_EQ(blocking[0].text, "ntdll.dll!NtWaitForSingleObject");
  EXPECT_EQ(blocking[1].text, "game.exe!Job::wait");
  EXPECT_EQ(blocking[3].text, "ntdll.dll!RtlUserThreadStart");
  const auto& unblocking = h.interactionPanel->unblockingStack();
  ASSERT_EQ(unblocking.size(), 2u);
  EXPECT_EQ(unblocking[0].text, "game.exe!Sys::signal");

  // The arrow persists after the mouse left (selected arrow), and stays when another interval is hovered.
  h.mouse(h.widget().screenGeometry().gutterX0 + 5.0f, h.yOf(main));
  ASSERT_EQ(h.widget().lastArrows().size(), 1u);
  EXPECT_TRUE(h.widget().lastArrows()[0].selected);

  // Esc clears the selection and the arrow.
  h.mouse(h.xOf(out - 10'000), h.yOf(main));
  h.key(ImGuiKey_Escape);
  EXPECT_FALSE(h.bus().state().selectedWait.has_value());
  h.frames(2);
  EXPECT_TRUE(h.widget().lastArrows().empty());
  EXPECT_FALSE(h.interactionPanel->episode().has_value());
}

TEST(InteractionWidget, ClickOnARunningIntervalDoesNotOpenThePanel) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101);
  h.show(base + kFirstWaitOut - 100'000, base + kFirstWaitOut + 200'000, 0);
  h.click(h.xOf(base + kFirstWaitOut + 150'000), h.yOf(main));
  EXPECT_FALSE(h.interactionEntry->open);
  EXPECT_FALSE(h.bus().state().selectedWait.has_value());
  EXPECT_EQ(h.bus().state().selectedThread, main);  // the normal click behaviour is unchanged
}

TEST(InteractionWidget, ClickOnAWaitWithoutWakerShowsItWithoutAnArrow) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101);
  h.show(base + kSecondWaitOut, base + kSecondWaitOut + 200'000, 50'000);
  h.click(h.xOf(base + kSecondWaitOut + 100'000), h.yOf(main));
  h.frames(3);
  ASSERT_TRUE(h.interactionPanel->episode().has_value());
  EXPECT_EQ(h.interactionPanel->details().waker, "unknown (no wake-up record)");
  EXPECT_EQ(h.interactionPanel->details().readyLatency, "n/a");
  EXPECT_TRUE(h.interactionPanel->unblockingStack().empty());
  EXPECT_TRUE(h.widget().lastArrows().empty());
  EXPECT_FALSE(h.interactionPanel->goToWaker(h.context()));  // nobody to go to
}

TEST(InteractionWidget, CtrlHoverTooltipHasBothStacksAndPlainHoverDoesNot) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101);
  const uint64_t out = base + kFirstWaitOut;
  h.show(out, out + kFirstWaitLength, 20'000);
  h.mouse(h.xOf(out + 30'000), h.yOf(main));
  const auto contains = [&](const char* text) {
    const auto& t = h.widget().lastTooltip();
    return std::find(t.begin(), t.end(), text) != t.end();
  };
  EXPECT_FALSE(h.widget().lastTooltip().empty());
  EXPECT_FALSE(contains("Blocking stack:"));

  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
  h.frames(2);
  EXPECT_TRUE(contains("Blocking stack:"));
  EXPECT_TRUE(contains("Unblocking stack:"));
  const auto& t = h.widget().lastTooltip();
  size_t frames = 0;
  for (const std::string& l : t) {
    if (l.rfind("  ", 0) == 0 && l != "  ..." && l != "  (none)") ++frames;
  }
  EXPECT_EQ(frames, 4u + 2u);  // the blocking stack has 4 frames, the unblocking one 2 (cap is 8)
  EXPECT_TRUE(std::any_of(t.begin(), t.end(), [](const std::string& l) {
    return l.rfind("Woken by: ", 0) == 0 && l.find("(tid 102)") != std::string::npos;
  }));

  // A Running interval gets no interaction lines even with Ctrl.
  h.mouse(h.xOf(out - 10'000), h.yOf(main));
  EXPECT_FALSE(contains("Blocking stack:"));
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
  h.frames(2);
}

TEST(InteractionWidget, GoToWakerAndGoToWokenNavigateAndSelectTheThread) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101), worker = h.thread(102);
  const uint64_t out = base + kFirstWaitOut;
  h.show(out, out + kFirstWaitLength, 20'000);
  h.click(h.xOf(out + 30'000), h.yOf(main));
  h.frames(3);
  ASSERT_TRUE(h.interactionPanel->episode().has_value());

  // Zoom somewhere else first so the navigation has something to do.
  h.widget().setView({0, h.widget().limits().total() / 50});
  h.frames(2);
  ASSERT_TRUE(h.interactionPanel->goToWaker(h.context()));
  h.frames(3);
  EXPECT_EQ(h.bus().state().selectedThread, worker);
  const TimeView v = h.widget().view();
  const double readyRel = toRelative(h.widget().limits(), out + kFirstWaitReadyAfter);
  EXPECT_GE(readyRel, v.start);
  EXPECT_LE(readyRel, v.end());
  EXPECT_NEAR(v.start + v.span / 2, readyRel, v.span * 0.1);  // centred on the wake-up
  EXPECT_LE(v.start, toRelative(h.widget().limits(), out));   // the switch-out is in view as well
  EXPECT_GE(v.end(), toRelative(h.widget().limits(), out + kFirstWaitLength));
  // The persistent arrow is now fully inside the view.
  ASSERT_FALSE(h.widget().lastArrows().empty());
  EXPECT_TRUE(h.widget().lastArrows()[0].geometry.visible);

  h.widget().setView({0, h.widget().limits().total() / 50});
  h.frames(2);
  ASSERT_TRUE(h.interactionPanel->goToWoken(h.context()));
  h.frames(3);
  EXPECT_EQ(h.bus().state().selectedThread, main);
  const TimeView w = h.widget().view();
  EXPECT_LE(w.start, toRelative(h.widget().limits(), out));
  EXPECT_GE(w.end(), toRelative(h.widget().limits(), out + kFirstWaitLength));
}

TEST(InteractionWidget, NavigationUnhidesTheTargetRow) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101), worker = h.thread(102);
  const uint64_t out = base + kFirstWaitOut;
  h.show(out, out + kFirstWaitLength, 20'000);
  h.click(h.xOf(out + 30'000), h.yOf(main));
  h.frames(3);
  h.widget().setHidden(worker, true);
  h.frames(2);
  EXPECT_TRUE(h.widget().flagsOf(worker).hidden);
  ASSERT_TRUE(h.interactionPanel->goToWaker(h.context()));
  h.frames(3);
  EXPECT_FALSE(h.widget().flagsOf(worker).hidden);
  EXPECT_TRUE(h.widget().bandCenterY(worker).has_value());
}

TEST(InteractionWidget, PanelListsTheLongestWaitsOfTheFilterRangeAndFollowsFilterChanges) {
  Harness h;
  h.open("medium.tachyon");
  h.interactionEntry->open = true;
  const auto settle = [&] {
    h.frames(2);
    ASSERT_TRUE(h.until([&] {
      return !h.interactionPanel->waits().pending() &&
             h.interactionPanel->waits().resultGeneration() == h.interactionPanel->waits().generation();
    }));
    h.frames(2);
  };
  h.frames(3);
  settle();
  const model::TraceModel& m = h.model();
  ia::WaitQuery all;
  all.t0 = m.header.start_ts;
  all.maxResults = h.interactionPanel->maxWaits();
  EXPECT_EQ(all.maxResults, 50u);
  EXPECT_EQ(h.interactionPanel->waits().rows(), ia::longestWaits(m, all, nullptr).rows);
  ASSERT_FALSE(h.interactionPanel->waits().rows().empty());

  // Changing the filter range and thread set re-runs the scan; the result matches a direct scan.
  const uint64_t span = m.header.end_ts - m.header.start_ts;
  ViewFilter f;
  f.t0 = m.header.start_ts + span / 4;
  f.t1 = m.header.start_ts + span / 2;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(0), ThreadIdx(1), ThreadIdx(2)};
  h.bus().setTimeFilter(f);
  h.frames(1);
  // Change again before the first scan could be accepted (stale guard): only the newest answer is shown.
  ViewFilter g = f;
  g.t0 = m.header.start_ts + span / 3;
  g.threads = std::nullopt;
  h.bus().setTimeFilter(g);
  settle();
  ia::WaitQuery qg;
  qg.t0 = g.t0;
  qg.t1 = g.t1;
  qg.maxResults = 50;
  EXPECT_EQ(h.interactionPanel->waits().rows(), ia::longestWaits(m, qg, nullptr).rows);
  EXPECT_EQ(h.interactionPanel->waits().query(), qg);
  for (const ia::WaitEpisode& e : h.interactionPanel->waits().rows()) {
    EXPECT_LT(e.switchOutTs, g.t1);
    EXPECT_GT(e.switchOutTs + e.waitTicks, g.t0);
  }

  // N is configurable.
  h.interactionPanel->setMaxWaits(5);
  h.frames(2);  // the next draw issues the request
  settle();
  EXPECT_LE(h.interactionPanel->waits().rows().size(), 5u);
  qg.maxResults = 5;
  EXPECT_EQ(h.interactionPanel->waits().rows(), ia::longestWaits(m, qg, nullptr).rows);
}

TEST(InteractionWidget, SelectingARowOfTheListSelectsTheWaitAndNavigatesToIt) {
  Harness h;
  h.open("medium.tachyon");
  h.interactionEntry->open = true;
  ASSERT_TRUE(h.until(
      [&] { return !h.interactionPanel->waits().pending() && !h.interactionPanel->waits().rows().empty(); }));
  h.frames(2);
  const ia::WaitEpisode e = h.interactionPanel->waits().rows().front();
  h.widget().setView({0, h.widget().limits().total() / 1000});  // far from the wait
  h.frames(2);
  h.interactionPanel->selectWait(h.context(), e);
  h.frames(3);
  ASSERT_TRUE(h.bus().state().selectedWait.has_value());
  EXPECT_EQ(h.bus().state().selectedWait->thread, e.thread);
  EXPECT_EQ(h.bus().state().selectedWait->switchOutTs, e.switchOutTs);
  EXPECT_EQ(h.bus().state().selectedThread, e.thread);
  ASSERT_TRUE(h.interactionPanel->episode().has_value());
  EXPECT_EQ(h.interactionPanel->episode()->switchOutTs, e.switchOutTs);
  const TimeView v = h.widget().view();
  EXPECT_LE(v.start, toRelative(h.widget().limits(), e.switchOutTs));
  EXPECT_GE(v.end(), toRelative(h.widget().limits(), e.switchOutTs + e.waitTicks));
  EXPECT_TRUE(h.widget().bandCenterY(e.thread).has_value());
}

TEST(InteractionWidget, ANewDocumentDropsTheSelectionAndTheList) {
  Harness h;
  h.open("tiny.tachyon");
  const uint64_t base = h.model().header.start_ts;
  const ThreadIdx main = h.thread(101);
  h.show(base + kFirstWaitOut, base + kFirstWaitOut + kFirstWaitLength, 20'000);
  h.click(h.xOf(base + kFirstWaitOut + 30'000), h.yOf(main));
  h.frames(3);
  ASSERT_TRUE(h.interactionPanel->episode().has_value());
  h.app.openFile(fixture("medium.tachyon"));
  ASSERT_TRUE(h.until(
      [&] { return h.app.document() != nullptr && !h.app.loading() && h.model().threads.size() > 4; }));
  h.frames(3);
  EXPECT_FALSE(h.bus().state().selectedWait.has_value());
  EXPECT_FALSE(h.interactionPanel->episode().has_value());
  EXPECT_TRUE(h.widget().lastArrows().empty());
}
