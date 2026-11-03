// WP-41: the capture UI inside a headless App. Acceptance: with the real tachyon-agent on --backend mock the
// whole Run -> capture -> stop -> capture_done -> document opened flow works. Also: validation blocks
// sending, the Attach dialog (own pid from the process list), a parked launch with Resume, agent auto-launch
// through an injected launcher (never pkexec), a killed agent (Lost + toast, no hang, reconnect),
// capture_done warnings, and App destruction with a request in flight.
#include "capture_test_util.h"
#include "viewer/capture/capture_paths.h"

#include <atomic>
#include <fstream>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

namespace {

AppOptions captureOptions(std::shared_ptr<IProcessLauncher> launcher) {
  AppOptions o = headlessOptions("capture");
  o.processLauncher = std::move(launcher);
  o.autoConnectAgent = false;
  return o;
}

bool toastContains(App& app, const std::string& text) {
  for (const Toast& t : app.toasts().toasts())
    if (t.text.find(text) != std::string::npos) return true;
  return false;
}

// A viewer wired to an agent address with the mock backend and a private captures directory.
struct CaptureRig {
  explicit CaptureRig(const std::string& tag, const std::string& address = std::string())
      : dir(tag),
        addr(address.empty() ? newAddress(tag) : address),
        launcher(std::make_shared<TestLauncher>()),
        h(captureOptions(launcher)) {
    configureCapture(h.app.settings(), addr, dir.path() / "captures");
  }
  ~CaptureRig() {
    launcher->killAll();
    std::error_code ec;
    fs::remove(addr, ec);
  }
  CaptureController& cap() { return h.app.capture(); }
  bool connected() { return cap().session().snapshot().connection == ConnectionState::Connected; }
  bool phaseIs(AgentPhase p) { return cap().session().snapshot().phase == p; }
  // Starts the agent through the launcher and waits until the viewer is connected to an idle agent.
  void startAgentAndConnect() {
    cap().startAgent(false);
    ASSERT_TRUE(h.until([&] { return connected() && phaseIs(AgentPhase::Idle); }));
  }
  void connectOnly() {
    cap().reconnect();
    ASSERT_TRUE(h.until([&] { return connected() && phaseIs(AgentPhase::Idle); }));
  }
  bool documentOpen() { return !h.app.loading() && h.app.document() != nullptr; }

  TempDir dir;
  std::string addr;
  std::shared_ptr<TestLauncher> launcher;
  Harness h;
};

// A scripted agent: answers status/start/stop and, on stop, announces a capture_done with given warnings.
class ScriptedAgent final : public control::IRequestHandler {
 public:
  ScriptedAgent(const std::string& address, fs::path trace, std::vector<std::string> warnings, uint64_t lost)
      : m_trace(std::move(trace)), m_warnings(std::move(warnings)), m_lost(lost) {
    auto listener = control::listenOn(address);
    EXPECT_TRUE(listener.ok());
    m_server = std::make_unique<control::Server>(std::move(*listener), *this);
    m_server->start();
  }
  ~ScriptedAgent() override { m_server->stop(); }

  control::Response handle(control::ClientId, const control::Request& request) override {
    using namespace control;
    if (std::holds_alternative<StatusRequest>(request.body)) {
      StatusBody s;
      s.state = m_capturing ? capture::CaptureState::Capturing : capture::CaptureState::Idle;
      s.backend = "scripted";
      return makeOkResponse(request.id, s);
    }
    if (std::holds_alternative<StartRequest>(request.body)) {
      m_out = std::get<StartRequest>(request.body).out_path;
      m_capturing = true;
      m_server->broadcast(StateEvent{capture::CaptureState::Capturing, 0.0});
      return makeOkResponse(request.id);
    }
    if (std::holds_alternative<StopRequest>(request.body)) {
      std::error_code ec;
      fs::copy_file(m_trace, m_out, fs::copy_options::overwrite_existing, ec);
      m_capturing = false;
      CaptureSummary summary;
      summary.path = m_out;
      summary.events_lost = m_lost;
      summary.duration_s = 1.5;
      summary.warnings = m_warnings;
      m_server->broadcast(StateEvent{capture::CaptureState::Converting, 0.5});
      m_server->broadcast(CaptureDoneEvent{summary});
      m_server->broadcast(StateEvent{capture::CaptureState::Idle, 0.0});
      return makeOkResponse(request.id);
    }
    return makeErrorResponse(request.id, makeError(ErrorCode::NotFound, "scripted agent: unsupported"));
  }

 private:
  fs::path m_trace;
  std::vector<std::string> m_warnings;
  uint64_t m_lost;
  std::string m_out;
  std::atomic<bool> m_capturing{false};
  std::unique_ptr<control::Server> m_server;
};

}  // namespace

// ---- acceptance ----

TEST(CaptureApp, RunCaptureStopOpensTheTrace) {
  CaptureRig rig("app_run");
  rig.startAgentAndConnect();

  // The agent was started through the launcher with the settings' backend, address and extra arguments.
  ASSERT_EQ(rig.launcher->launches(), 1u);
  const LaunchSpec spec = rig.launcher->last();
  EXPECT_EQ(spec.exe, fs::path(TACHYON_AGENT_EXE));
  EXPECT_FALSE(spec.elevate);
  ASSERT_GE(spec.args.size(), 4u);
  EXPECT_EQ(spec.args[0], "--backend");
  EXPECT_EQ(spec.args[1], "mock");
  EXPECT_EQ(spec.args[3], rig.addr);
  EXPECT_EQ(rig.cap().session().snapshot().backend, "mock");

  CaptureController& cap = rig.cap();
  ASSERT_TRUE(rig.h.app.commands().run("capture.run"));
  rig.h.frames(2);
  EXPECT_TRUE(cap.dialogOpen());
  EXPECT_EQ(cap.dialogMode(), capture::CaptureMode::Launch);
  const ImGuiWindow* dialog = ImGui::FindWindowByName("Run Application##capture-dialog");
  ASSERT_NE(dialog, nullptr);
  EXPECT_TRUE(dialog->Active);

  cap.form().exe = TACHYON_AGENT_EXE;
  cap.form().args = "--help \"two words\"";
  cap.form().env = {"TACHYON_TEST=1"};
  cap.form().samplingHz = 4000;
  rig.h.frames(2);
  EXPECT_TRUE(cap.formErrors().empty()) << cap.formErrors().front().message;
  ASSERT_TRUE(cap.submitDialog());
  EXPECT_FALSE(cap.dialogOpen());

  const control::StartRequest& sent = cap.lastRequest();
  EXPECT_EQ(sent.mode, capture::CaptureMode::Launch);
  EXPECT_EQ(sent.exe, TACHYON_AGENT_EXE);
  EXPECT_EQ(sent.args, (std::vector<std::string>{"--help", "two words"}));
  EXPECT_EQ(sent.env.at("TACHYON_TEST"), "1");
  EXPECT_EQ(sent.sampling_hz, 4000u);
  EXPECT_TRUE(sent.start_immediately);
  const fs::path out = pathFromUtf8(sent.out_path);
  EXPECT_EQ(out.parent_path(), fs::absolute(rig.dir.path() / "captures"));
  EXPECT_TRUE(fs::is_directory(out.parent_path())) << "the captures directory is created";

  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  EXPECT_TRUE(rig.h.app.panels().find(kCapturePanelName)->open) << "the Capture window opens with a capture";
  rig.h.frames(2);
  const std::string status = cap.statusText();
  EXPECT_NE(status.find("REC"), std::string::npos) << status;
  EXPECT_NE(status.find("mock"), std::string::npos) << status;

  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); })) << "capture_done opens the trace";
  EXPECT_EQ(rig.h.app.document()->path(), out);
  EXPECT_GT(rig.h.app.document()->model().threads.size(), 0u);
  EXPECT_TRUE(toastContains(rig.h.app, "Capture finished"));
  ASSERT_FALSE(rig.h.app.settings().recentFiles.empty());
  EXPECT_EQ(pathFromUtf8(rig.h.app.settings().recentFiles.front()), out);
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Idle); }));
  EXPECT_EQ(rig.h.app.settings().capture.exe, TACHYON_AGENT_EXE) << "the dialog's values are remembered";
  EXPECT_EQ(rig.h.app.settings().capture.samplingHz, 4000u);
  EXPECT_EQ(cap.statusText().find("REC"), std::string::npos);

  // The Session Explorer lists the new capture with the metadata the mock's fixture carries.
  rig.h.app.commands().run("capture.explorer");
  auto* explorer =
      dynamic_cast<SessionExplorerPanel*>(rig.h.app.panels().find(kSessionExplorerPanelName)->panel.get());
  ASSERT_NE(explorer, nullptr);
  ASSERT_TRUE(rig.h.until([&] {
    explorer->poll();
    for (const SessionInfo& r : explorer->rows())
      if (r.path == out && r.state == SessionState::Ok) return true;
    return false;
  }));
}

TEST(CaptureApp, AutoOpenCanBeTurnedOff) {
  CaptureRig rig("app_noautoopen");
  rig.h.app.settings().capture.autoOpen = false;
  rig.startAgentAndConnect();
  CaptureController& cap = rig.cap();
  cap.openDialog(capture::CaptureMode::System);
  ASSERT_TRUE(cap.submitDialog());
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "Capture finished"); }));
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Idle); }));
  rig.h.frames(10);
  EXPECT_EQ(rig.h.app.document(), nullptr);
  ASSERT_FALSE(rig.h.app.settings().recentFiles.empty()) << "it is still remembered as a recent file";
  EXPECT_TRUE(fs::exists(pathFromUtf8(rig.h.app.settings().recentFiles.front())));
}

TEST(CaptureApp, EveryDialogDrawsAndTheCaptureMenuCommandsExist) {
  CaptureRig rig("app_dialogs");
  struct Case {
    const char* command;
    const char* window;
  };
  for (const Case& c : {Case{"capture.run", "Run Application##capture-dialog"},
                        Case{"capture.attach", "Attach to Process##capture-dialog"},
                        Case{"capture.system", "System Capture##capture-dialog"}}) {
    ASSERT_TRUE(rig.h.app.commands().run(c.command)) << c.command;
    rig.h.frames(3);
    const ImGuiWindow* w = ImGui::FindWindowByName(c.window);
    ASSERT_NE(w, nullptr) << c.window;
    EXPECT_TRUE(w->Active) << c.window;
    rig.cap().closeDialog();
    rig.h.frames(2);
  }
  for (const char* id : {"capture.stop", "capture.cancel", "capture.resume", "capture.agent.reconnect",
                         "capture.window", "capture.explorer"}) {
    EXPECT_TRUE(rig.h.app.commands().run(id)) << id;
  }
  rig.h.frames(3);  // not connected: the commands only produce toasts
  EXPECT_TRUE(rig.h.app.panels().find(kCapturePanelName)->open);
  const ImGuiWindow* panel = ImGui::FindWindowByName(kCapturePanelName);
  ASSERT_NE(panel, nullptr);
  EXPECT_TRUE(panel->Active) << "the Capture window draws, agent settings included";
  EXPECT_NE(rig.cap().statusText().find("Agent:"), std::string::npos);
}

// ---- validation ----

TEST(CaptureApp, ValidationErrorsBlockSending) {
  InProcessAgent agent(newAddress("validate"), fixture("tiny.tachyon"));
  CaptureRig rig("app_validate2", agent.address);
  rig.connectOnly();
  CaptureController& cap = rig.cap();

  cap.openDialog(capture::CaptureMode::Launch);
  cap.form().exe = "";  // nothing chosen
  EXPECT_FALSE(cap.submitDialog());
  ASSERT_FALSE(cap.formErrors().empty());
  EXPECT_EQ(cap.formErrors().front().field, "exe");
  rig.h.frames(3);  // the message is drawn under the field
  EXPECT_TRUE(cap.dialogOpen()) << "the dialog stays open";

  cap.form().exe = pathToUtf8(rig.dir.path() / "no-such-program");
  EXPECT_FALSE(cap.submitDialog());
  cap.form().exe = TACHYON_AGENT_EXE;
  cap.form().samplingHz = 0;
  EXPECT_FALSE(cap.submitDialog());
  cap.form().samplingHz = 8000;
  cap.form().circularMb = 1;
  EXPECT_FALSE(cap.submitDialog());
  cap.form().circularMb = 0;
  cap.form().env = {"no-equals-sign"};
  EXPECT_FALSE(cap.submitDialog());
  cap.form().env.clear();
  cap.form().args = "\"open";
  EXPECT_FALSE(cap.submitDialog());

  rig.h.frames(5);
  EXPECT_EQ(agent.backend->startCalls(), 0) << "nothing reached the agent";
  EXPECT_TRUE(cap.session().lastStartRequest().exe.empty());
  EXPECT_TRUE(rig.phaseIs(AgentPhase::Idle));

  cap.form().args.clear();
  EXPECT_TRUE(cap.submitDialog()) << "fixing the form lets it through";
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  EXPECT_EQ(agent.backend->startCalls(), 1);
}

TEST(CaptureApp, NothingIsSentWithoutAnAgentOrWhileItIsBusy) {
  CaptureRig rig("app_notconnected");
  CaptureController& cap = rig.cap();
  cap.openDialog(capture::CaptureMode::System);
  EXPECT_FALSE(cap.submitDialog());
  bool agentError = false;
  for (const FormError& e : cap.formErrors()) agentError |= e.field == "agent";
  EXPECT_TRUE(agentError);
  rig.h.frames(3);
  EXPECT_FALSE(rig.connected());
  EXPECT_EQ(cap.statusText(), "Agent: not connected");

  rig.startAgentAndConnect();
  cap.openDialog(capture::CaptureMode::System);
  ASSERT_TRUE(cap.submitDialog());
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  cap.openDialog(capture::CaptureMode::System);
  EXPECT_FALSE(cap.submitDialog()) << "the agent is busy";
}

// ---- attach ----

TEST(CaptureApp, AttachDialogListsThisProcessAndSendsAttachWithItsPid) {
  CaptureRig rig("app_attach");
  rig.startAgentAndConnect();
  CaptureController& cap = rig.cap();
  ASSERT_TRUE(rig.h.app.commands().run("capture.attach"));
  EXPECT_TRUE(cap.processesLoading() || !cap.processes().empty());
  ASSERT_TRUE(rig.h.until([&] { return !cap.processesLoading(); }));
  EXPECT_TRUE(cap.processError().empty()) << cap.processError();
  const uint32_t self = static_cast<uint32_t>(control::testing_util::currentPid());
  bool listed = false;
  for (const ProcessInfo& p : cap.processes()) listed |= p.pid == self;
  ASSERT_TRUE(listed) << "own pid " << self << " is not among " << cap.processes().size() << " processes";
  rig.h.frames(3);  // the table draws

  // Attach without a selection is refused; the filter narrows the list to the process.
  EXPECT_FALSE(cap.submitDialog());
  cap.processFilter() = std::to_string(self);
  const std::vector<size_t> rows = cap.filteredProcesses();
  ASSERT_FALSE(rows.empty());
  rig.h.frames(2);
  cap.selectProcess(self);
  EXPECT_EQ(cap.form().pid, self);
  EXPECT_FALSE(cap.form().processName.empty());
  ASSERT_TRUE(cap.submitDialog()) << (cap.formErrors().empty() ? "" : cap.formErrors().front().message);
  EXPECT_EQ(cap.lastRequest().mode, capture::CaptureMode::Attach);
  EXPECT_EQ(cap.lastRequest().pid, self);
  EXPECT_TRUE(cap.lastRequest().exe.empty());
  EXPECT_EQ(rig.cap().session().lastStartRequest().pid, self);

  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  EXPECT_NE(cap.statusText().find("attach pid " + std::to_string(self)), std::string::npos)
      << cap.statusText();
  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); }));
}

TEST(CaptureApp, AttachRequestReachesTheBackendAsAttach) {
  InProcessAgent agent(newAddress("attach_backend"), fixture("tiny.tachyon"));
  CaptureRig rig("app_attach2", agent.address);
  rig.connectOnly();
  CaptureController& cap = rig.cap();
  cap.openDialog(capture::CaptureMode::Attach);
  ASSERT_TRUE(rig.h.until([&] { return !cap.processesLoading(); }));
  cap.form().pid = 555;  // not a real process: the dialog lists, the agent validates
  cap.form().processName = "someprocess";
  cap.form().followChildren = true;
  cap.form().childFilter = "*.exe";
  ASSERT_TRUE(cap.submitDialog());
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  const capture::CaptureOptions seen = agent.backend->lastStart();
  EXPECT_EQ(seen.mode, capture::CaptureMode::Attach);
  EXPECT_EQ(seen.pid, 555u);
  EXPECT_TRUE(seen.followChildren);
  EXPECT_EQ(seen.childFilter, "*.exe");
  EXPECT_NE(pathToUtf8(seen.outPath.filename()).find("someprocess-"), std::string::npos);
}

// ---- parked launch ----

TEST(CaptureApp, ParkedLaunchWaitsForResume) {
  InProcessAgent agent(newAddress("parked_app"), fixture("tiny.tachyon"));
  CaptureRig rig("app_parked", agent.address);
  rig.connectOnly();
  CaptureController& cap = rig.cap();
  cap.openDialog(capture::CaptureMode::Launch);
  cap.form().exe = TACHYON_AGENT_EXE;
  cap.form().startImmediately = false;
  ASSERT_TRUE(cap.submitDialog());
  EXPECT_FALSE(cap.lastRequest().start_immediately);
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Armed); }));
  rig.h.frames(2);
  EXPECT_NE(cap.statusText().find("Armed"), std::string::npos) << cap.statusText();
  EXPECT_EQ(rig.cap().session().snapshot().elapsedSeconds, 0.0);
  EXPECT_TRUE(rig.h.app.panels().find(kCapturePanelName)->open);

  ASSERT_TRUE(rig.h.app.commands().run("capture.resume"));
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  rig.h.app.commands().run("capture.stop");
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); }));
  EXPECT_EQ(agent.backend->stopCalls(), 1);
}

TEST(CaptureApp, CancelOfAParkedLaunchReturnsToIdle) {
  InProcessAgent agent(newAddress("parked_cancel"), fixture("tiny.tachyon"));
  CaptureRig rig("app_parked_cancel", agent.address);
  rig.connectOnly();
  CaptureController& cap = rig.cap();
  cap.openDialog(capture::CaptureMode::Launch);
  cap.form().exe = TACHYON_AGENT_EXE;
  cap.form().startImmediately = false;
  ASSERT_TRUE(cap.submitDialog());
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Armed); }));
  cap.cancelCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Idle); }));
  rig.h.frames(10);
  EXPECT_EQ(rig.h.app.document(), nullptr);
}

// ---- agent start / elevation ----

TEST(CaptureApp, ElevatedAgentStartGoesThroughTheLauncherNeverPkexec) {
  CaptureRig rig("app_elevate");
  rig.launcher->spawn = false;  // record only
  rig.h.app.settings().capture.agentBackend = "perf";
  rig.h.app.settings().capture.agentExe = TACHYON_AGENT_EXE;
  EXPECT_EQ(rig.cap().agentNeedsElevation(), !currentProcessHasCapturePrivileges());
  rig.cap().startAgent(true);
  ASSERT_EQ(rig.launcher->launches(), 1u);
  const LaunchSpec spec = rig.launcher->last();
  EXPECT_TRUE(spec.elevate) << "the launcher is told to elevate; it alone decides how (pkexec / runas)";
  EXPECT_EQ(spec.exe, fs::path(TACHYON_AGENT_EXE));
  EXPECT_EQ(spec.args[1], "perf");
  EXPECT_EQ(rig.cap().session().snapshot().connection, ConnectionState::Connecting);
  rig.cap().startAgent(false);
  ASSERT_EQ(rig.launcher->launches(), 2u);
  EXPECT_FALSE(rig.launcher->last().elevate);

  rig.h.app.settings().capture.agentBackend = "mock";
  EXPECT_FALSE(rig.cap().agentNeedsElevation()) << "the mock backend needs no privileges";
  rig.h.frames(3);  // the Capture window offers the buttons
}

TEST(CaptureApp, AgentThatCannotBeLaunchedIsReported) {
  CaptureRig rig("app_launchfail");
  rig.launcher->failWith = "pkexec: not authorized";
  rig.cap().startAgent(true);
  rig.h.frames(2);
  EXPECT_TRUE(toastContains(rig.h.app, "not authorized"));
  EXPECT_EQ(rig.cap().session().snapshot().connection, ConnectionState::Disconnected);

  rig.launcher->failWith.clear();
  rig.h.app.settings().capture.agentExe = pathToUtf8(rig.dir.path() / "missing-agent");
  rig.cap().startAgent(false);
  rig.h.frames(2);
  EXPECT_TRUE(toastContains(rig.h.app, "does not exist"));
  EXPECT_EQ(rig.launcher->launches(), 1u) << "a missing executable is caught before the launcher is asked";
}

TEST(CaptureApp, StartupConnectionIsSilentAndOnlyWhenEnabled) {
  AgentProcess agent(newAddress("startup"));
  TempDir dir("app_startup");
  auto launcher = std::make_shared<TestLauncher>();
  AppOptions options = captureOptions(launcher);
  options.autoConnectAgent = true;
  // The settings file decides the address and autoConnect: write one.
  options.configPath = dir.path() / "settings.json";
  Settings s;
  s.capture.agentAddress = agent.address();
  s.capture.autoConnect = true;
  ASSERT_TRUE(s.save(options.configPath).ok());
  {
    Harness h(options);
    ASSERT_TRUE(h.until([&] { return h.app.capture().session().snapshot().phase == AgentPhase::Idle; }));
    EXPECT_FALSE(toastContains(h.app, "Connected")) << "the startup attempt is silent";
  }
  s.capture.autoConnect = false;
  ASSERT_TRUE(s.save(options.configPath).ok());
  {
    Harness h(options);
    h.frames(20);
    EXPECT_EQ(h.app.capture().session().snapshot().connection, ConnectionState::Disconnected);
  }
  // No agent running: the failed attempt does not toast either.
  s.capture.autoConnect = true;
  s.capture.agentAddress = newAddress("startup-none");
  ASSERT_TRUE(s.save(options.configPath).ok());
  {
    Harness h(options);
    ASSERT_TRUE(h.until([&] {
      return h.app.capture().session().snapshot().connection == ConnectionState::Disconnected &&
             !h.app.capture().session().snapshot().lastError.empty();
    }));
    h.frames(3);
    EXPECT_TRUE(h.app.toasts().empty());
  }
}

// ---- failure ----

TEST(CaptureApp, KilledAgentMidCaptureShowsLostThenReconnects) {
  CaptureRig rig("app_kill");
  rig.startAgentAndConnect();
  CaptureController& cap = rig.cap();
  cap.openDialog(capture::CaptureMode::System);
  ASSERT_TRUE(cap.submitDialog());
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));

  rig.launcher->killAll();
  ASSERT_TRUE(
      rig.h.until([&] { return rig.cap().session().snapshot().connection == ConnectionState::Lost; }, 20))
      << "the lost connection is noticed, nothing hangs";
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "lost the connection"); }));
  rig.h.frames(3);
  EXPECT_EQ(cap.statusText(), "Agent: connection lost");
  EXPECT_EQ(rig.cap().session().snapshot().phase, AgentPhase::Unknown);
  EXPECT_FALSE(rig.cap().session().snapshot().lastError.empty());
  EXPECT_EQ(rig.h.app.document(), nullptr);

  // Restart the agent (the stale socket file is replaced) and reconnect.
  std::error_code ec;
  fs::remove(rig.addr, ec);
  rig.startAgentAndConnect();
  EXPECT_EQ(rig.launcher->launches(), 2u);
  EXPECT_EQ(cap.statusText().find("lost"), std::string::npos);
}

TEST(CaptureApp, AgentErrorsAndRefusalsBecomeToasts) {
  CaptureRig rig("app_refusal");
  rig.startAgentAndConnect();
  rig.cap().stopCapture();  // idle: the agent answers ENOTFOUND
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "refused stop"); }));
  EXPECT_TRUE(toastContains(rig.h.app, "ENOTFOUND"));
  EXPECT_TRUE(rig.connected());
}

TEST(CaptureApp, CaptureDoneWarningsAndEventsLostAreSurfaced) {
  const std::string address = newAddress("scripted");
  std::vector<std::string> warnings;
  for (int i = 0; i < 5; ++i)
    warnings.push_back("instrumentation spool of pid " + std::to_string(i) + " hit its size cap");
  ScriptedAgent agent(address, fixture("tiny.tachyon"), warnings, /*lost=*/12);
  CaptureRig rig("app_warnings2", address);
  rig.connectOnly();
  EXPECT_EQ(rig.cap().session().snapshot().backend, "scripted");
  rig.cap().openDialog(capture::CaptureMode::System);
  ASSERT_TRUE(rig.cap().submitDialog());
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  rig.cap().stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); }));
  // events lost -> a warning toast; the first three warnings are toasted, the rest summarised. The toast
  // queue keeps five, so the summary is the newest.
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "and 2 more capture warnings"); }));
  EXPECT_TRUE(toastContains(rig.h.app, "hit its size cap"));
  bool sawWarningLevel = false;
  for (const Toast& t : rig.h.app.toasts().toasts()) sawWarningLevel |= t.level == ToastLevel::Warning;
  EXPECT_TRUE(sawWarningLevel);
}

// WP-67: what only the agent knows (spool caps, refused connections, failed merges) is in the banner of the
// opened trace too, not only in toasts, and a sentence the trace already says is not repeated.
TEST(CaptureApp, CaptureDoneWarningsAlsoShowInTheBannerOfTheOpenedTrace) {
  const std::string address = newAddress("scripted_banner");
  const std::vector<std::string> warnings = {"instrumentation spool of pid 7 hit its size cap",
                                             "refused a connection from pid 9",
                                             "merge of pid 4 failed: clock mapper"};
  ScriptedAgent agent(address, fixture("tiny.tachyon"), warnings, /*lost=*/0);
  CaptureRig rig("app_warnings_banner", address);
  rig.connectOnly();
  rig.cap().openDialog(capture::CaptureMode::System);
  ASSERT_TRUE(rig.cap().submitDialog());
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  rig.cap().stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); }));
  rig.h.frames(3);
  WarningsBanner& banner = rig.h.app.warningsBanner();
  ASSERT_TRUE(banner.visible());
  size_t fromAgent = 0;
  for (const CaptureWarning& w : banner.warnings()) fromAgent += w.kind == CaptureWarningKind::AgentWarning;
  EXPECT_EQ(fromAgent, 3u);
  bool sawCap = false;
  for (const CaptureWarning& w : banner.warnings())
    sawCap |= w.text.find("hit its size cap") != std::string::npos;
  EXPECT_TRUE(sawCap);
  EXPECT_TRUE(toastContains(rig.h.app, "hit its size cap")) << "the toasts stay";
}

TEST(CaptureApp, DestroyingTheAppWithARequestInFlightIsClean) {
  const std::string address = newAddress("inflight");
  BlackholeServer hung(address);
  const auto t0 = std::chrono::steady_clock::now();
  {
    CaptureRig rig("app_inflight", address);
    rig.cap().reconnect();
    ASSERT_TRUE(rig.h.until([&] { return rig.connected(); }));
    rig.cap().stopCapture();
    rig.cap().cancelCapture();
    rig.h.frames(3);
    ASSERT_TRUE(rig.h.until([&] { return rig.cap().session().snapshot().commandInFlight; }));
    // leaving the scope destroys the App while the worker waits for an answer that never comes
  }
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
}

TEST(CaptureApp, DestroyingTheAppWhileConnectingIsClean) {
  CaptureRig rig("app_connecting");
  rig.launcher->spawn = false;
  rig.cap().startAgent(false);  // nobody ever listens: a long retry loop
  rig.h.frames(3);
  EXPECT_EQ(rig.cap().session().snapshot().connection, ConnectionState::Connecting);
  EXPECT_TRUE(rig.cap().active());
}
