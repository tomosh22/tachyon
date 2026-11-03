// WP-41: AgentSession against the real tachyon-agent (--backend mock, private socket), an in-process agent
// with the scriptable launch backend (parked launches), a hung agent, and a dying agent.
#include "capture_test_util.h"
#include "viewer/capture/agent_session.h"

#include <fstream>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

namespace {

// Collects the session's events as they are drained.
struct EventLog {
  explicit EventLog(AgentSession& s) : session(s) {}
  void drain() {
    for (AgentEvent& e : session.drainEvents()) events.push_back(std::move(e));
  }
  bool has(AgentEventKind kind) {
    drain();
    for (const AgentEvent& e : events)
      if (e.kind == kind) return true;
    return false;
  }
  const AgentEvent* find(AgentEventKind kind) {
    drain();
    for (const AgentEvent& e : events)
      if (e.kind == kind) return &e;
    return nullptr;
  }
  size_t count(AgentEventKind kind) {
    drain();
    size_t n = 0;
    for (const AgentEvent& e : events) n += e.kind == kind;
    return n;
  }
  AgentSession& session;
  std::vector<AgentEvent> events;
};

AgentSession::Options fastOptions() {
  AgentSession::Options o;
  o.pollIntervalMs = 100;
  o.callTimeoutMs = 3000;
  return o;
}

control::StartRequest systemStart(const fs::path& out) {
  control::StartRequest r;
  r.mode = capture::CaptureMode::System;
  r.out_path = pathToUtf8(out);
  return r;
}

bool waitPhase(AgentSession& s, AgentPhase phase, int seconds = 20) {
  return waitFor([&] { return s.snapshot().phase == phase; }, seconds);
}

}  // namespace

TEST(AgentSessionTest, NamesAreStable) {
  EXPECT_STREQ(connectionStateName(ConnectionState::Lost), "Lost");
  EXPECT_STREQ(agentPhaseName(AgentPhase::Armed), "armed");
}

TEST(AgentSessionTest, FailedConnectIsReportedWithoutBlockingTheCaller) {
  AgentSession session(fastOptions());
  EventLog log(session);
  const auto t0 = std::chrono::steady_clock::now();
  session.connect(newAddress("nobody"), 0, /*userInitiated=*/false);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(500))
      << "connect() returns at once";
  EXPECT_EQ(session.snapshot().connection, ConnectionState::Connecting);
  ASSERT_TRUE(waitFor([&] { return log.has(AgentEventKind::ConnectFailed); }));
  const AgentSnapshot s = session.snapshot();
  EXPECT_EQ(s.connection, ConnectionState::Disconnected);
  EXPECT_NE(s.lastError.find("cannot connect"), std::string::npos) << s.lastError;
  EXPECT_FALSE(log.find(AgentEventKind::ConnectFailed)->userInitiated);
}

TEST(AgentSessionTest, ConnectsAndReportsTheStatus) {
  AgentProcess agent(newAddress("status"));
  AgentSession session(fastOptions());
  EventLog log(session);
  session.connect(agent.address(), 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  const AgentSnapshot s = session.snapshot();
  EXPECT_EQ(s.connection, ConnectionState::Connected);
  EXPECT_EQ(s.backend, "mock");
  EXPECT_EQ(s.address, agent.address());
  EXPECT_TRUE(s.lastError.empty());
  EXPECT_TRUE(log.has(AgentEventKind::Connected));
  EXPECT_TRUE(log.find(AgentEventKind::Connected)->userInitiated);
}

TEST(AgentSessionTest, RunStopDeliversCaptureDone) {
  TempDir dir("session_run");
  AgentProcess agent(newAddress("run"), /*mockDurationMs=*/100);
  AgentSession session(fastOptions());
  EventLog log(session);
  session.connect(agent.address(), 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));

  const fs::path out = dir.path() / "cap.tachyon";
  session.start(systemStart(out), "system");
  ASSERT_TRUE(waitPhase(session, AgentPhase::Capturing));
  AgentSnapshot s = session.snapshot();
  EXPECT_EQ(s.target, "system");
  const double first = s.elapsedSeconds;
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_GT(session.snapshot().elapsedSeconds, first) << "elapsed advances without any traffic";

  session.stop();
  ASSERT_TRUE(waitFor([&] { return log.has(AgentEventKind::CaptureDone); }));
  const AgentEvent* done = log.find(AgentEventKind::CaptureDone);
  EXPECT_EQ(fs::path(done->summary.path), out);
  EXPECT_TRUE(fs::exists(out));
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  s = session.snapshot();
  EXPECT_TRUE(s.target.empty());
  EXPECT_EQ(s.elapsedSeconds, 0.0);
  // The phases came in order: capturing, stopping (conversion takes the mock's 100 ms), idle.
  std::vector<AgentPhase> phases;
  log.drain();
  for (const AgentEvent& e : log.events)
    if (e.kind == AgentEventKind::StateChanged) phases.push_back(e.phase);
  ASSERT_GE(phases.size(), 3u);
  const auto capturing = std::find(phases.begin(), phases.end(), AgentPhase::Capturing);
  const auto stopping = std::find(phases.begin(), phases.end(), AgentPhase::Stopping);
  ASSERT_NE(capturing, phases.end());
  ASSERT_NE(stopping, phases.end());
  EXPECT_LT(capturing, stopping);
  EXPECT_EQ(phases.back(), AgentPhase::Idle);
  EXPECT_EQ(session.lastStartRequest().out_path, pathToUtf8(out));
}

TEST(AgentSessionTest, RefusedCommandsComeBackAsCommandFailed) {
  TempDir dir("session_refused");
  AgentProcess agent(newAddress("refused"));
  AgentSession session(fastOptions());
  EventLog log(session);
  session.connect(agent.address(), 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));

  session.stop();  // idle: ENOTFOUND
  ASSERT_TRUE(waitFor([&] { return log.has(AgentEventKind::CommandFailed); }));
  EXPECT_EQ(log.find(AgentEventKind::CommandFailed)->command, "stop");
  EXPECT_EQ(log.find(AgentEventKind::CommandFailed)->code, "ENOTFOUND");
  EXPECT_EQ(session.snapshot().connection, ConnectionState::Connected)
      << "a refusal is not a lost connection";

  session.start(systemStart(dir.path() / "a.tachyon"), "first");
  ASSERT_TRUE(waitPhase(session, AgentPhase::Capturing));
  session.start(systemStart(dir.path() / "b.tachyon"), "second");  // EBUSY
  ASSERT_TRUE(waitFor([&] { return log.count(AgentEventKind::CommandFailed) >= 2; }));
  const AgentEvent* busy = nullptr;
  for (const AgentEvent& e : log.events)
    if (e.kind == AgentEventKind::CommandFailed) busy = &e;
  ASSERT_NE(busy, nullptr);
  EXPECT_EQ(busy->code, "EBUSY");
  EXPECT_EQ(session.snapshot().phase, AgentPhase::Capturing);
  session.cancel();
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  EXPECT_FALSE(fs::exists(dir.path() / "a.tachyon")) << "cancel deletes the output";
}

TEST(AgentSessionTest, CommandsWithoutAConnectionFailCleanly) {
  AgentSession session(fastOptions());
  EventLog log(session);
  session.stop();
  ASSERT_TRUE(waitFor([&] { return log.has(AgentEventKind::CommandFailed); }));
  EXPECT_EQ(log.find(AgentEventKind::CommandFailed)->code, "ENOTCONNECTED");
  EXPECT_EQ(session.snapshot().connection, ConnectionState::Disconnected);
}

TEST(AgentSessionTest, KilledAgentBecomesLostAndReconnectsAfterARestart) {
  TempDir dir("session_kill");
  const std::string address = newAddress("kill");
  auto agent = std::make_unique<AgentProcess>(address);
  AgentSession session(fastOptions());
  EventLog log(session);
  session.connect(address, 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  session.start(systemStart(dir.path() / "k.tachyon"), "system");
  ASSERT_TRUE(waitPhase(session, AgentPhase::Capturing));

  agent->kill();
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(waitFor([&] { return session.snapshot().connection == ConnectionState::Lost; }, 15))
      << "no hang";
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
  const AgentSnapshot lost = session.snapshot();
  EXPECT_EQ(lost.phase, AgentPhase::Unknown);
  EXPECT_FALSE(lost.lastError.empty());
  ASSERT_TRUE(log.has(AgentEventKind::ConnectionLost));
  EXPECT_NE(log.find(AgentEventKind::ConnectionLost)->message.find("lost the connection"), std::string::npos);

  // Commands while Lost fail instead of hanging.
  session.stop();
  ASSERT_TRUE(waitFor([&] { return log.has(AgentEventKind::CommandFailed); }));

  agent.reset();  // removes the stale socket
  agent = std::make_unique<AgentProcess>(address);
  session.connect(address, 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  EXPECT_EQ(session.snapshot().connection, ConnectionState::Connected);
  EXPECT_TRUE(session.snapshot().lastError.empty()) << "a fresh connection clears the old error";
}

TEST(AgentSessionTest, DestroyingWhileARequestIsInFlightDoesNotWaitForTheTimeout) {
  const std::string address = newAddress("hung");
  BlackholeServer hung(address);
  AgentSession::Options options = fastOptions();
  options.callTimeoutMs = 30000;  // the destructor must not wait this long
  const auto t0 = std::chrono::steady_clock::now();
  {
    AgentSession session(options);
    session.connect(address, 2000);
    ASSERT_TRUE(waitFor([&] { return session.snapshot().connection == ConnectionState::Connected; }, 5));
    session.stop();  // the initial status call is already unanswered, this one queues behind it
    ASSERT_TRUE(waitFor([&] { return session.snapshot().commandInFlight; }, 5));
  }
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
}

TEST(AgentSessionTest, ANewerCommandEndsTheConnectRetries) {
  AgentSession session(fastOptions());
  session.connect(newAddress("never"), 60000);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto t0 = std::chrono::steady_clock::now();
  session.disconnect();  // a newer command ends the retry loop
  ASSERT_TRUE(waitFor([&] { return session.snapshot().connection == ConnectionState::Disconnected; }, 5));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
}

TEST(AgentSessionTest, ParkedLaunchIsArmedUntilResumed) {
  TempDir dir("session_parked");
  InProcessAgent agent(newAddress("parked"), fixture("tiny.tachyon"));
  AgentSession session(fastOptions());
  EventLog log(session);
  session.connect(agent.address, 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));

  control::StartRequest launch;
  launch.mode = capture::CaptureMode::Launch;
  launch.exe = "game";
  launch.start_immediately = false;
  launch.out_path = pathToUtf8(dir.path() / "parked.tachyon");
  session.start(launch, "launch game");
  ASSERT_TRUE(waitPhase(session, AgentPhase::Armed));
  EXPECT_EQ(session.snapshot().target, "launch game");
  EXPECT_EQ(session.snapshot().elapsedSeconds, 0.0);
  EXPECT_EQ(agent.backend->lastStart().exe, "game");

  session.stop();  // refused while parked
  ASSERT_TRUE(waitFor([&] { return log.has(AgentEventKind::CommandFailed); }));
  EXPECT_EQ(log.find(AgentEventKind::CommandFailed)->code, "EBUSY");
  EXPECT_EQ(session.snapshot().phase, AgentPhase::Armed);

  session.resume();
  ASSERT_TRUE(waitPhase(session, AgentPhase::Capturing));
  EXPECT_EQ(session.snapshot().target, "launch game");
  session.stop();
  ASSERT_TRUE(waitFor([&] { return log.has(AgentEventKind::CaptureDone); }));
  EXPECT_TRUE(fs::exists(dir.path() / "parked.tachyon"));
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
}

TEST(AgentSessionTest, ACancelledParkedLaunchGoesBackToIdle) {
  TempDir dir("session_parked_cancel");
  InProcessAgent agent(newAddress("parkedc"), fixture("tiny.tachyon"));
  AgentSession session(fastOptions());
  session.connect(agent.address, 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  control::StartRequest launch;
  launch.mode = capture::CaptureMode::Launch;
  launch.exe = "game";
  launch.start_immediately = false;
  launch.out_path = pathToUtf8(dir.path() / "parked.tachyon");
  session.start(launch, "launch game");
  ASSERT_TRUE(waitPhase(session, AgentPhase::Armed));
  session.cancel();
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  EXPECT_TRUE(session.snapshot().target.empty());
}

TEST(AgentSessionTest, AttachRequestsCarryTheMode) {
  TempDir dir("session_attach");
  InProcessAgent agent(newAddress("attach"), fixture("tiny.tachyon"));
  AgentSession session(fastOptions());
  session.connect(agent.address, 5000);
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
  control::StartRequest attach;
  attach.mode = capture::CaptureMode::Attach;
  attach.pid = 31337;
  attach.sampling_hz = 2000;
  attach.circular_mb = 0;
  attach.out_path = pathToUtf8(dir.path() / "attach.tachyon");
  session.start(attach, "attach pid 31337");
  ASSERT_TRUE(waitPhase(session, AgentPhase::Capturing));
  const capture::CaptureOptions seen = agent.backend->lastStart();
  EXPECT_EQ(seen.mode, capture::CaptureMode::Attach);
  EXPECT_EQ(seen.pid, 31337u);
  EXPECT_EQ(seen.samplingHz, 2000u);
  session.stop();
  ASSERT_TRUE(waitPhase(session, AgentPhase::Idle));
}
