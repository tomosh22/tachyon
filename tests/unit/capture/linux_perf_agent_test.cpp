// WP-70: the agent driving a LinuxPerfBackend (over the fake perf layer, so no privileges are needed): the
// launch environment injection reaches the real child, a target that exits finishes the capture by itself,
// a deferred launch is triggered by the second start, cancel deletes the output.
#include "../control/test_util.h"
#include "agent/agent.h"
#include "capture/linux_perf/linux_perf_backend.h"
#include "linux_perf_test_util.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using capture::CaptureState;
using namespace control;
using capture::perf::ptest::FakePerfSystem;
using capture::perf::ptest::TempDir;

struct Rig {
  TempDir dir;
  std::shared_ptr<FakePerfSystem> fake = std::make_shared<FakePerfSystem>();
  std::unique_ptr<Agent> agent;
  std::mutex eventMutex;
  std::vector<Event> events;

  Rig() {
    capture::LinuxPerfBackendConfig cfg;
    cfg.system = fake;
    cfg.drainInterval = std::chrono::milliseconds(2);
    AgentConfig ac;
    ac.backendName = "perf";
    ac.agentAddress = "/run/user/1000/tachyon-agent.sock";
    ac.elevated = true;
    agent = std::make_unique<Agent>(std::make_unique<capture::LinuxPerfBackend>(cfg), ac);
    agent->setEventSink([this](const Event& e) {
      std::lock_guard lock(eventMutex);
      events.push_back(e);
    });
  }
  ~Rig() { agent->shutdown(); }

  Response call(RequestBody body) {
    Request r;
    r.id = 1;
    r.body = std::move(body);
    return agent->handle(1, r);
  }
  CaptureState stateNow() { return std::get<StatusBody>(call(StatusRequest{}).body).state; }
  bool waitForState(CaptureState want, int timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (stateNow() == want) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
  std::optional<CaptureDoneEvent> doneEvent() {
    std::lock_guard lock(eventMutex);
    for (const Event& e : events)
      if (const auto* d = std::get_if<CaptureDoneEvent>(&e)) return *d;
    return std::nullopt;
  }
  // Kills the process whose pid the script recorded in pid.txt (the target outlives a capture by design).
  void killRecordedTarget() const {
    std::ifstream in(dir.path() / "pid.txt");
    long pid = 0;
    if (in >> pid && pid > 1) {
      ::kill(static_cast<pid_t>(pid), SIGKILL);
      int st = 0;
      ::waitpid(static_cast<pid_t>(pid), &st, 0);
    }
  }
  StartRequest shell(const std::string& script) const {
    StartRequest s;
    s.mode = capture::CaptureMode::Launch;
    s.exe = "/bin/sh";
    s.args = {"-c", script};
    s.cwd = dir.path().string();
    s.out_path = (dir.path() / "out.tachyon").string();
    return s;
  }
};

TEST(AgentPerf, LaunchInjectsTheAgentAddressAndTheFinishedTargetEndsTheCapture) {
  Rig rig;
  StartRequest s = rig.shell("printf '%s|%s' \"$TACHYON_AGENT_PIPE\" \"$USER_VAR\" > env.txt");
  s.env = {{"USER_VAR", "hello"}, {"TACHYON_AGENT_PIPE", "to-be-overridden"}};
  const Response started = rig.call(s);
  ASSERT_TRUE(started.ok) << started.error;

  // The watcher sees the target exit and the agent converts without anyone sending `stop`.
  ASSERT_TRUE(rig.waitForState(CaptureState::Idle, 8000));
  const auto done = rig.doneEvent();
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->summary.path, s.out_path);
  EXPECT_EQ(done->summary.events_lost, 0u);
  EXPECT_TRUE(fs::exists(s.out_path));

  std::ifstream in(rig.dir.path() / "env.txt");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(text, "/run/user/1000/tachyon-agent.sock|hello") << "the agent address replaces the user's value";
}

TEST(AgentPerf, DeferredLaunchStartsOnTheSecondStartAndStopEndsIt) {
  Rig rig;
  StartRequest s = rig.shell("echo $$ > pid.txt; exec sleep 30");
  s.start_immediately = false;
  ASSERT_TRUE(rig.call(s).ok);
  EXPECT_EQ(rig.stateNow(), CaptureState::Starting);
  for (const auto& e : rig.fake->events())
    EXPECT_TRUE(e.closed) << "only the privilege probe was opened while parked";

  StartRequest go;
  go.mode = capture::CaptureMode::Launch;
  go.exe = "-";
  go.out_path = s.out_path;
  const Response r = rig.call(go);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(rig.stateNow(), CaptureState::Capturing);
  size_t live = 0;
  for (const auto& e : rig.fake->events()) live += e.closed ? 0 : 1;
  EXPECT_EQ(live, rig.fake->cpus.size()) << "the session is running";

  ASSERT_TRUE(rig.call(StopRequest{}).ok);
  ASSERT_TRUE(rig.waitForState(CaptureState::Idle));
  ASSERT_TRUE(rig.doneEvent().has_value());
  rig.killRecordedTarget();  // the target outlives the capture
}

TEST(AgentPerf, StartFailuresMapToProtocolCodes) {
  Rig rig;
  rig.fake->failSystemWideWith = EACCES;
  rig.fake->sys.paranoid = 4;
  StartRequest sys;
  sys.mode = capture::CaptureMode::System;
  sys.out_path = (rig.dir.path() / "sys.tachyon").string();
  const Response denied = rig.call(sys);
  EXPECT_FALSE(denied.ok);
  EXPECT_EQ(denied.code, ErrorCode::NoPrivilege) << "the agent's checkPrivileges probe reports ENOPRIV";
  EXPECT_NE(denied.error.find("kernel.perf_event_paranoid"), std::string::npos) << denied.error;
  EXPECT_EQ(rig.stateNow(), CaptureState::Idle);

  rig.fake->failSystemWideWith = 0;
  StartRequest missing = rig.shell("true");
  missing.exe = "/no/such/program";
  const Response notFound = rig.call(missing);
  EXPECT_FALSE(notFound.ok);
  EXPECT_EQ(notFound.code, ErrorCode::NotFound);
  EXPECT_EQ(rig.stateNow(), CaptureState::Idle);
}

TEST(AgentPerf, CancelStopsAndDeletesTheOutput) {
  Rig rig;
  StartRequest s = rig.shell("echo $$ > pid.txt; exec sleep 30");
  ASSERT_TRUE(rig.call(s).ok);
  ASSERT_EQ(rig.stateNow(), CaptureState::Capturing);
  const Response cancelled = rig.call(CancelRequest{});
  EXPECT_TRUE(cancelled.ok) << cancelled.error;
  ASSERT_TRUE(rig.waitForState(CaptureState::Idle));
  EXPECT_FALSE(fs::exists(s.out_path)) << "cancel deletes the output";
  rig.killRecordedTarget();
}

}  // namespace
}  // namespace tachyon::agent
