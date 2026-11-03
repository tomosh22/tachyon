// WP-61: the viewer against a remote agent (tcp://, token): the Connect to remote agent flow, the remote
// Run / Attach dialogs, background transfer of finished captures and flushed traces into the captures
// directory (progress, cancel, resume, failures), the process list of the other machine, and what the
// settings keep (a path, never the token). The "remote" agent is an in-process Agent (mock backend) behind a
// TCP listener on loopback; its traces are in a different directory than the viewer's captures.
#include "capture_test_util.h"
#include "control/tcp.h"
#include "viewer/capture/capture_paths.h"

#include <fstream>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

namespace {

const std::string kToken = "viewer-remote-token-0123456789-abcdefg";

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeToken(const fs::path& path, const std::string& token) {
  std::ofstream(path, std::ios::binary) << token << "\n";
#if !defined(_WIN32)
  ::chmod(path.string().c_str(), 0600);
#endif
}

uint32_t selfPid() {
#if defined(_WIN32)
  return static_cast<uint32_t>(_getpid());
#else
  return static_cast<uint32_t>(::getpid());
#endif
}

bool toastContains(App& app, const std::string& text) {
  for (const Toast& t : app.toasts().toasts())
    if (t.text.find(text) != std::string::npos) return true;
  return false;
}

// The remote machine: an Agent with a TCP listener whose traces go to <dir>/remote-out.
struct RemoteAgent {
  explicit RemoteAgent(const fs::path& dir, bool allowLaunch = false)
      : outDir(dir / "remote-out"), localAddress(newAddress("remoteviewer")) {
    fs::create_directories(outDir);
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture("tiny.tachyon");
    agent::AgentConfig ac;
    ac.backendName = "mock";
    ac.agentAddress = localAddress;
    ac.remoteOutDir = outDir;
    ac.allowRemoteLaunch = allowLaunch;
    core = std::make_unique<agent::Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto local = control::listenOn(localAddress);
    EXPECT_TRUE(local.ok());
    server = std::make_unique<control::Server>(std::move(*local), *core);
    auto tcp = control::listenTcp(control::TcpEndpoint{"127.0.0.1", 0});
    EXPECT_TRUE(tcp.ok());
    hostPort = (*tcp)->address();
    control::AuthPolicy policy;
    policy.token = kToken;
    server->addRemoteListener(std::move(*tcp), policy);
    core->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
    tokenFile = dir / "agent.token";
    writeToken(tokenFile, kToken);
  }
  ~RemoteAgent() {
    server->stop();
    core->shutdown();
  }

  fs::path outDir;
  std::string localAddress;
  std::string hostPort;
  fs::path tokenFile;
  std::unique_ptr<agent::Agent> core;
  std::unique_ptr<control::Server> server;
};

struct RemoteRig {
  explicit RemoteRig(const std::string& tag, bool allowLaunch = false)
      : dir(tag), remote(dir.path(), allowLaunch), h([&] {
          AppOptions o = headlessOptions("remote");
          o.autoConnectAgent = false;
          return o;
        }()) {
    h.app.settings().capture.capturesDir = pathToUtf8(dir.path() / "captures");
    h.app.settings().capture.autoConnect = false;
    h.app.settings().capture.agentAddress = newAddress("remoteviewer-unused");
  }
  CaptureController& cap() { return h.app.capture(); }
  bool connected() { return cap().session().snapshot().connection == ConnectionState::Connected; }
  bool phaseIs(AgentPhase p) { return cap().session().snapshot().phase == p; }
  bool connect() {
    return cap().connectRemote(remote.hostPort, pathToUtf8(remote.tokenFile)) &&
           h.until([&] { return connected() && phaseIs(AgentPhase::Idle); });
  }
  bool documentOpen() { return !h.app.loading() && h.app.document() != nullptr; }
  fs::path captures() const { return dir.path() / "captures"; }
  bool transferDone(size_t i = 0) {
    return cap().transfers().size() > i && cap().transfers()[i].state == RemoteTransfer::State::Done;
  }

  TempDir dir;
  RemoteAgent remote;
  Harness h;
};

TEST(CaptureRemote, ConnectDialogProbeAndSettings) {
  RemoteRig rig("rem_dialog");
  CaptureController& cap = rig.cap();

  // The probe tells what it found, and why not.
  RemoteProbe good = probeRemoteAgent(rig.remote.hostPort, pathToUtf8(rig.remote.tokenFile));
  EXPECT_TRUE(good.ok) << good.message;
  EXPECT_NE(good.message.find("mock backend"), std::string::npos) << good.message;
  EXPECT_FALSE(probeRemoteAgent("not an address", pathToUtf8(rig.remote.tokenFile)).ok);
  EXPECT_FALSE(probeRemoteAgent(rig.remote.hostPort, "").ok);
  const RemoteProbe missing = probeRemoteAgent(rig.remote.hostPort, pathToUtf8(rig.dir.path() / "nope"));
  EXPECT_FALSE(missing.ok);
  EXPECT_NE(missing.message.find("does not exist"), std::string::npos) << missing.message;
  const fs::path wrong = rig.dir.path() / "wrong.token";
  writeToken(wrong, "WRONG-token-0123456789-abcdefghijklmnop");
  const RemoteProbe denied = probeRemoteAgent(rig.remote.hostPort, pathToUtf8(wrong));
  EXPECT_FALSE(denied.ok);
  EXPECT_NE(denied.message.find("authentication failed"), std::string::npos) << denied.message;

  // The dialog (headless frames draw it) with Test connection, then Connect.
  cap.openRemoteDialog();
  rig.h.frames(2);
  EXPECT_TRUE(cap.remoteDialogOpen());
  ASSERT_NE(ImGui::FindWindowByName("Connect to Remote Agent##remote-dialog"), nullptr);
  cap.remoteAddressField() = rig.remote.hostPort;
  cap.remoteTokenFileField() = pathToUtf8(rig.remote.tokenFile);
  cap.testRemote();
  ASSERT_TRUE(rig.h.until([&] { return cap.remoteProbe().done; }));
  EXPECT_TRUE(cap.remoteProbe().ok) << cap.remoteProbe().message;
  rig.h.frames(2);

  ASSERT_TRUE(rig.connect());
  EXPECT_TRUE(cap.remoteActive());
  const AgentSnapshot snap = cap.session().snapshot();
  EXPECT_TRUE(snap.remote);
  EXPECT_EQ(snap.address, "tcp://" + rig.remote.hostPort);
  EXPECT_EQ(snap.backend, "mock");
  EXPECT_NE(cap.statusText().find("Remote agent"), std::string::npos) << cap.statusText();

  // Only the address and the token file's path are kept, in the settings and on disk.
  EXPECT_EQ(rig.h.app.settings().capture.remoteAddress, rig.remote.hostPort);
  EXPECT_EQ(rig.h.app.settings().capture.remoteTokenFile, pathToUtf8(rig.remote.tokenFile));
  const fs::path saved = rig.dir.path() / "settings.json";
  ASSERT_TRUE(rig.h.app.settings().save(saved).ok());
  EXPECT_EQ(slurp(saved).find(kToken), std::string::npos) << "the token itself must never be saved";
  EXPECT_NE(slurp(saved).find("remoteTokenFile"), std::string::npos);
  const Settings reloaded = Settings::load(saved);
  EXPECT_EQ(reloaded.capture.remoteAddress, rig.remote.hostPort);
  EXPECT_EQ(reloaded.capture.remoteTokenFile, pathToUtf8(rig.remote.tokenFile));

  // Reconnect goes back to the remote agent; Use Local Agent leaves it.
  cap.disconnectAgent();
  ASSERT_TRUE(rig.h.until([&] { return !rig.connected(); }));
  cap.reconnect();
  ASSERT_TRUE(rig.h.until([&] { return rig.connected(); }));
  EXPECT_TRUE(cap.session().snapshot().remote);
  cap.useLocalAgent();
  ASSERT_TRUE(rig.h.until([&] { return !cap.session().snapshot().remote; }));
}

TEST(CaptureRemote, BadAddressOrTokenIsReported) {
  RemoteRig rig("rem_bad");
  CaptureController& cap = rig.cap();
  EXPECT_FALSE(cap.connectRemote("no-port", pathToUtf8(rig.remote.tokenFile)));
  EXPECT_TRUE(toastContains(rig.h.app, "Remote agent address"));

  EXPECT_TRUE(cap.connectRemote(rig.remote.hostPort, pathToUtf8(rig.dir.path() / "missing.token")));
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "does not exist"); }));
  EXPECT_FALSE(rig.connected());

  const fs::path wrong = rig.dir.path() / "wrong.token";
  writeToken(wrong, "WRONG-token-0123456789-abcdefghijklmnop");
  EXPECT_TRUE(cap.connectRemote(rig.remote.hostPort, pathToUtf8(wrong)));
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "authentication failed"); }));
  EXPECT_FALSE(rig.connected());
  EXPECT_EQ(cap.session().snapshot().connection, ConnectionState::Disconnected);
}

TEST(CaptureRemote, AttachFetchesTheTraceAndOpensIt) {
  RemoteRig rig("rem_attach");
  CaptureController& cap = rig.cap();
  ASSERT_TRUE(rig.connect());

  // The Attach dialog lists the processes of the agent's machine (this process is one of them).
  cap.openDialog(capture::CaptureMode::Attach);
  ASSERT_TRUE(rig.h.until([&] { return !cap.processesLoading() && !cap.processes().empty(); }));
  EXPECT_TRUE(cap.processError().empty()) << cap.processError();
  const uint32_t self = selfPid();
  bool found = false;
  for (const ProcessInfo& p : cap.processes()) found |= p.pid == self;
  EXPECT_TRUE(found);
  EXPECT_TRUE(cap.form().remote);
  rig.h.frames(2);
  ASSERT_NE(ImGui::FindWindowByName("Attach to Process (remote agent)##capture-dialog"), nullptr);
  cap.selectProcess(self);
  rig.h.frames(2);
  ASSERT_TRUE(cap.submitDialog());
  // The request names a file, not a directory: the agent decides where its traces go.
  EXPECT_EQ(fs::path(cap.lastRequest().out_path).filename(), cap.lastRequest().out_path);
  EXPECT_FALSE(cap.lastRequest().out_path.empty());

  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); })) << "the fetched trace is opened";
  ASSERT_TRUE(rig.transferDone());

  const RemoteTransfer& t = cap.transfers().front();
  EXPECT_EQ(t.localPath.parent_path(), rig.captures());
  EXPECT_EQ(t.localPath.filename(), fs::path(t.remotePath).filename());
  EXPECT_NE(fs::path(t.remotePath).parent_path(), rig.captures()) << "it really came from elsewhere";
  EXPECT_EQ(slurp(t.localPath), slurp(fixture("tiny.tachyon")));
  EXPECT_FALSE(fs::exists(t.localPath.string() + ".part"));
  EXPECT_EQ(rig.h.app.document()->path(), t.localPath);
  EXPECT_GT(rig.h.app.document()->model().threads.size(), 0u);
  EXPECT_TRUE(toastContains(rig.h.app, "Fetched"));
  ASSERT_FALSE(rig.h.app.settings().recentFiles.empty());
  EXPECT_EQ(pathFromUtf8(rig.h.app.settings().recentFiles.front()), fs::absolute(t.localPath));

  // The Session Explorer lists it like any local capture.
  rig.h.app.commands().run("capture.explorer");
  auto* explorer =
      dynamic_cast<SessionExplorerPanel*>(rig.h.app.panels().find(kSessionExplorerPanelName)->panel.get());
  ASSERT_NE(explorer, nullptr);
  ASSERT_TRUE(rig.h.until([&] {
    explorer->poll();
    for (const SessionInfo& r : explorer->rows())
      if (r.path == t.localPath && r.state == SessionState::Ok) return true;
    return false;
  }));
  rig.h.frames(3);  // draws the Capture window with its transfers list
}

TEST(CaptureRemote, RunDialogUsesRemotePathsAndLaunchNeedsOptIn) {
  RemoteRig rig("rem_run");
  CaptureController& cap = rig.cap();
  ASSERT_TRUE(rig.connect());
  cap.openDialog(capture::CaptureMode::Launch);
  rig.h.frames(2);
  ASSERT_NE(ImGui::FindWindowByName("Run Application (remote agent)##capture-dialog"), nullptr);
  // Paths on the other machine are not checked against this file system.
  cap.form().exe = "/opt/game/bin/game";
  cap.form().cwd = "/opt/game";
  rig.h.frames(2);
  EXPECT_TRUE(cap.formErrors().empty()) << cap.formErrors().front().message;
  cap.form().outPath = "/etc/evil.tachyon";
  rig.h.frames(2);
  EXPECT_FALSE(cap.formErrors().empty()) << "a remote output is a file name, not a path";
  cap.form().outPath = "game-run.tachyon";
  rig.h.frames(2);
  ASSERT_TRUE(cap.submitDialog()) << (cap.formErrors().empty() ? "" : cap.formErrors().front().message);
  EXPECT_EQ(cap.lastRequest().exe, "/opt/game/bin/game");
  EXPECT_EQ(cap.lastRequest().out_path, "game-run.tachyon");
  // The agent was not started with --allow-remote-launch.
  ASSERT_TRUE(rig.h.until([&] { return toastContains(rig.h.app, "ENOPRIV"); }));
  EXPECT_TRUE(toastContains(rig.h.app, "--allow-remote-launch"));
  EXPECT_TRUE(rig.phaseIs(AgentPhase::Idle));
}

TEST(CaptureRemote, FlushedTracesAreFetchedAndOpenable) {
  RemoteRig rig("rem_flush");
  CaptureController& cap = rig.cap();
  rig.h.app.settings().capture.autoOpen = false;
  ASSERT_TRUE(rig.connect());
  control::StartRequest start;
  start.mode = capture::CaptureMode::System;
  start.circular_mb = 16;
  start.out_path = "ring.tachyon";
  cap.session().start(start, "system");
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  cap.session().flush("hitch", 5, "test");
  ASSERT_TRUE(rig.h.until([&] { return !cap.flushedCaptures().empty(); }));
  EXPECT_FALSE(cap.flushedCaptures().front().remotePath.empty());
  ASSERT_TRUE(rig.h.until([&] { return rig.transferDone(); }));
  const FlushedCapture& f = cap.flushedCaptures().front();
  EXPECT_TRUE(f.fetched);
  EXPECT_EQ(pathFromUtf8(f.path).parent_path(), rig.captures()) << "the list points at the local copy";
  EXPECT_TRUE(fs::exists(f.path));
  EXPECT_FALSE(rig.documentOpen()) << "a flush does not open by itself";
  cap.openFlushed(0);
  ASSERT_TRUE(rig.h.until([&] { return rig.documentOpen(); }));
  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Idle); }));
}

TEST(CaptureRemote, AnInterruptedDownloadResumesAndAFailureCanBeRetried) {
  RemoteRig rig("rem_resume");
  CaptureController& cap = rig.cap();
  rig.h.app.settings().capture.autoOpen = false;
  ASSERT_TRUE(rig.connect());

  // A capture the agent produced.
  control::StartRequest start;
  start.mode = capture::CaptureMode::System;
  start.out_path = "resume.tachyon";
  cap.session().start(start, "system");
  ASSERT_TRUE(rig.h.until([&] { return rig.phaseIs(AgentPhase::Capturing); }));
  cap.stopCapture();
  ASSERT_TRUE(rig.h.until([&] { return rig.transferDone(0); }));
  const fs::path first = cap.transfers().front().localPath;
  const std::string remote = cap.transfers().front().remotePath;
  const std::string expected = slurp(fixture("tiny.tachyon"));
  ASSERT_EQ(slurp(first), expected);

  // Leave half of the file as a .part for the next download of the same remote file (the name is taken, so
  // the next local name is <stem>-1).
  const fs::path second = first.parent_path() / (first.stem().string() + "-1" + first.extension().string());
  {
    std::ofstream part(second.string() + ".part", std::ios::binary);
    part.write(expected.data(), static_cast<std::streamsize>(expected.size() / 2));
  }
  cap.fetchRemote(remote, false, true);
  ASSERT_TRUE(rig.h.until([&] { return rig.transferDone(1); }));
  EXPECT_EQ(cap.transfers()[1].localPath, second);
  EXPECT_EQ(slurp(second), expected) << "continued from the partial file and verified";

  // A path the agent never produced: the transfer fails with the agent's refusal and can be retried.
  cap.fetchRemote("/etc/passwd", false, true);
  ASSERT_TRUE(rig.h.until([&] {
    return cap.transfers().size() > 2 && cap.transfers()[2].state == RemoteTransfer::State::Failed;
  }));
  EXPECT_NE(cap.transfers()[2].error.find("no such trace"), std::string::npos) << cap.transfers()[2].error;
  EXPECT_TRUE(toastContains(rig.h.app, "Could not fetch"));
  EXPECT_FALSE(fs::exists(cap.transfers()[2].localPath));
  cap.retryTransfer(2);
  ASSERT_TRUE(rig.h.until([&] { return cap.transfers()[2].state == RemoteTransfer::State::Failed; }));
  rig.h.frames(3);
}

TEST(CaptureRemote, ProcessListNeedsAConnection) {
  RemoteRig rig("rem_procs");
  CaptureController& cap = rig.cap();
  ASSERT_TRUE(rig.connect());
  cap.disconnectAgent();
  ASSERT_TRUE(rig.h.until([&] { return !rig.connected(); }));
  cap.openDialog(capture::CaptureMode::Attach);
  EXPECT_FALSE(cap.processError().empty());
  EXPECT_FALSE(cap.processesLoading());
}

}  // namespace
