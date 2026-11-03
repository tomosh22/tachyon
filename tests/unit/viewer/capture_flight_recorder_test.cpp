// WP-60: the Flight Recorder panel inside a headless App against the real tachyon-agent (--backend mock).
#include "capture_test_util.h"
#include "viewer/capture/capture_paths.h"

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

namespace {

AppOptions flightOptions(std::shared_ptr<IProcessLauncher> launcher) {
  AppOptions o = headlessOptions("flight");
  o.processLauncher = std::move(launcher);
  o.autoConnectAgent = false;
  return o;
}

bool toastContains(App& app, const std::string& text) {
  for (const Toast& t : app.toasts().toasts())
    if (t.text.find(text) != std::string::npos) return true;
  return false;
}

struct FlightRig {
  explicit FlightRig(const std::string& tag)
      : dir(tag),
        addr(newAddress(tag)),
        launcher(std::make_shared<TestLauncher>()),
        h(flightOptions(launcher)) {
    configureCapture(h.app.settings(), addr, dir.path() / "captures");
    cap().startAgent(false);
    EXPECT_TRUE(h.until([&] { return connected() && phaseIs(AgentPhase::Idle); }));
  }
  ~FlightRig() {
    launcher->killAll();
    std::error_code ec;
    fs::remove(addr, ec);
  }
  CaptureController& cap() { return h.app.capture(); }
  bool connected() { return cap().session().snapshot().connection == ConnectionState::Connected; }
  bool phaseIs(AgentPhase p) { return cap().session().snapshot().phase == p; }
  bool documentOpen() { return !h.app.loading() && h.app.document() != nullptr; }

  TempDir dir;
  std::string addr;
  std::shared_ptr<TestLauncher> launcher;
  Harness h;
};

}  // namespace

TEST(FlightRecorderPanel, ArmsFlushesAndListsTheTraces) {
  FlightRig rig("fr_panel");
  CaptureController& cap = rig.cap();

  // Not capturing: the panel draws, says so, and Flush now does nothing but explain.
  ASSERT_TRUE(rig.h.app.commands().run("capture.flightrecorder"));
  rig.h.frames(3);
  const ImGuiWindow* panelWindow = ImGui::FindWindowByName(kFlightRecorderPanelName);
  ASSERT_NE(panelWindow, nullptr);
  EXPECT_TRUE(panelWindow->Active);
  EXPECT_FALSE(cap.flightRecorderState().armed);
  EXPECT_FALSE(cap.flushNow());

  // A circular buffer in the System dialog starts a flight recorder.
  cap.openDialog(capture::CaptureMode::System);
  cap.form().circularMb = 64;
  ASSERT_TRUE(cap.submitDialog());
  EXPECT_EQ(cap.lastRequest().circular_mb, 64u);
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  ASSERT_TRUE(rig.h.until([&] { return cap.flightRecorderState().armed; }));
  EXPECT_EQ(cap.flightRecorderState().ringMb, 64u);
  EXPECT_TRUE(rig.h.app.panels().find(kFlightRecorderPanelName)->open);
  rig.h.frames(3);

  // Flush now: the agent writes <captures dir>/manual-....tachyon and says so with a flushed event.
  cap.flushSeconds() = 5;
  ASSERT_TRUE(cap.flushNow());
  ASSERT_TRUE(rig.h.until([&] { return cap.flushedCaptures().size() == 1; }));
  const FlushedCapture first = cap.flushedCaptures().front();
  EXPECT_EQ(first.reason, "manual (viewer)");
  EXPECT_EQ(first.name.rfind("manual-", 0), 0u) << first.name;
  const fs::path firstPath = pathFromUtf8(first.path);
  EXPECT_TRUE(fs::exists(firstPath)) << first.path;
  EXPECT_EQ(firstPath.parent_path(), fs::absolute(rig.dir.path() / "captures"));
  EXPECT_TRUE(toastContains(rig.h.app, "Flight recorder: flushed"));

  // A flush that somebody else asked for (the engine's trigger) shows up too.
  {
    auto other = control::Client::connectWithRetry(rig.addr, 5000);
    ASSERT_TRUE(other.ok());
    control::FlushRequest trigger;
    trigger.name = "hitch";
    trigger.seconds = 10;
    trigger.reason = "hitch";
    trigger.trigger = true;
    trigger.pid = 4242;
    auto r = other->call(trigger, 10000);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r->ok) << r->error;
  }
  ASSERT_TRUE(rig.h.until([&] { return cap.flushedCaptures().size() == 2; }));
  EXPECT_EQ(cap.flushedCaptures().back().reason, "hitch");
  EXPECT_NE(cap.flushedCaptures().back().path, first.path);
  rig.h.frames(3);  // the list draws

  // Flushed captures are in the Session Explorer.
  rig.h.app.commands().run("capture.explorer");
  auto* explorer =
      dynamic_cast<SessionExplorerPanel*>(rig.h.app.panels().find(kSessionExplorerPanelName)->panel.get());
  ASSERT_NE(explorer, nullptr);
  ASSERT_TRUE(rig.h.until([&] {
    explorer->poll();
    for (const SessionInfo& r : explorer->rows())
      if (r.path == firstPath && r.state == SessionState::Ok) return true;
    return false;
  }));

  // Open loads the flushed trace.
  cap.openFlushed(0);
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); }));
  EXPECT_EQ(rig.h.app.document()->path(), firstPath);

  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Idle); }));
  EXPECT_FALSE(cap.flightRecorderState().capturing);
}

TEST(FlightRecorderPanel, FlushOfACaptureThatIsNotCircularIsRefusedAndExplained) {
  FlightRig rig("fr_plain");
  CaptureController& cap = rig.cap();
  cap.openDialog(capture::CaptureMode::System);
  ASSERT_TRUE(cap.submitDialog());  // circular buffer off
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  EXPECT_FALSE(cap.flightRecorderState().armed);
  EXPECT_TRUE(cap.flightRecorderState().circularUnknown);
  ASSERT_TRUE(cap.flushNow());  // allowed to try; the agent knows
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "refused flush"); }));
  EXPECT_TRUE(cap.flushedCaptures().empty());
  rig.h.frames(3);
  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Idle); }));
}

TEST(FlightRecorderPanel, DrawsWithoutAnAgent) {
  TempDir dir("fr_none");
  auto launcher = std::make_shared<TestLauncher>();
  Harness h(flightOptions(launcher));
  configureCapture(h.app.settings(), newAddress("fr_none"), dir.path() / "captures");
  ASSERT_TRUE(h.app.commands().run("capture.flightrecorder"));
  h.frames(3);
  const ImGuiWindow* w = ImGui::FindWindowByName(kFlightRecorderPanelName);
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->Active);
  EXPECT_FALSE(h.app.capture().flightRecorderState().connected);
  EXPECT_FALSE(h.app.capture().flushNow());
}
