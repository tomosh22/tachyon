// Agent behaviour added by WP-13: environment injection, deferred launch (start_immediately=false), target
// exit auto-finish, and the flush-in-flight guard (WP-11 review item). Driven through Agent::handle with a
// scriptable backend, so it runs on every platform.
#include "../control/test_util.h"
#include "agent/agent.h"
#include "capture/windows_etw/launch.h"
#include "fake_launch_backend.h"
#include "fixture_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using capture::CaptureState;
using control::testing_util::TempDir;
using testing_util::FakeLaunchBackend;
using namespace control;

struct Rig {
  TempDir dir;
  fs::path fixture = testing_util::writeFixtureTrace(dir.path() / "fixture.tachyon");
  FakeLaunchBackend* backend = nullptr;
  std::unique_ptr<Agent> agent;
  std::mutex eventMutex;
  std::vector<Event> events;

  Rig() {
    auto fake = std::make_unique<FakeLaunchBackend>(fixture);
    backend = fake.get();
    AgentConfig ac;
    ac.backendName = "fake";
    ac.agentAddress = "agent-address-for-test";
    agent = std::make_unique<Agent>(std::move(fake), ac);
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
  StartRequest launch(bool immediately = true) const {
    StartRequest s;
    s.mode = capture::CaptureMode::Launch;
    s.exe = "game.exe";
    s.out_path = (dir.path() / "out.tachyon").string();
    s.start_immediately = immediately;
    return s;
  }
  CaptureState stateNow() {
    auto r = call(StatusRequest{});
    return std::get<StatusBody>(r.body).state;
  }
  bool waitForState(CaptureState want, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (stateNow() == want) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
  size_t countEvents(auto&& pred) {
    std::lock_guard lock(eventMutex);
    size_t n = 0;
    for (const Event& e : events)
      if (pred(e)) ++n;
    return n;
  }
};

TEST(AgentLaunch, InjectsAgentPipeAndKeepsUserEnvironment) {
  Rig rig;
  StartRequest s = rig.launch();
  s.env = {{"USER_VAR", "1"}, {"TACHYON_AGENT_PIPE", "should-be-replaced"}};
  s.args = {"--scenario", "spin"};
  s.cwd = "somewhere";
  ASSERT_TRUE(rig.call(s).ok);
  const capture::CaptureOptions seen = rig.backend->lastStart();
  EXPECT_EQ(seen.mode, capture::CaptureMode::Launch);
  EXPECT_EQ(seen.exe, "game.exe");
  EXPECT_EQ(seen.args, (std::vector<std::string>{"--scenario", "spin"}));
  EXPECT_EQ(seen.cwd, "somewhere");
  EXPECT_EQ(seen.env.at("USER_VAR"), "1");
  EXPECT_EQ(seen.env.at(std::string(capture::etw::kAgentPipeEnvVar)), "agent-address-for-test");
  EXPECT_EQ(rig.stateNow(), CaptureState::Capturing);
}

TEST(AgentLaunch, OtherModesGetNoInjectedVariable) {
  Rig rig;
  StartRequest s;
  s.mode = capture::CaptureMode::System;
  s.out_path = (rig.dir.path() / "sys.tachyon").string();
  ASSERT_TRUE(rig.call(s).ok);
  EXPECT_TRUE(rig.backend->lastStart().env.empty());
}

TEST(AgentLaunch, DeferredLaunchParksThenStartsOnSecondStart) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch(false)).ok);
  EXPECT_EQ(rig.stateNow(), CaptureState::Starting) << "parked, not recording";
  EXPECT_TRUE(rig.backend->launchPending());

  // Nothing but the trigger or cancel is accepted while parked.
  const Response stop = rig.call(StopRequest{});
  EXPECT_FALSE(stop.ok);
  EXPECT_EQ(stop.code, ErrorCode::Busy);
  EXPECT_NE(stop.error.find("not recording"), std::string::npos) << stop.error;
  const Response flush = rig.call(FlushRequest{"x", 1});
  EXPECT_FALSE(flush.ok);
  const Response second = rig.call(rig.launch(false));
  EXPECT_FALSE(second.ok);
  EXPECT_EQ(second.code, ErrorCode::Busy);

  // The trigger: any start with start_immediately, other fields ignored.
  StartRequest trigger;
  trigger.mode = capture::CaptureMode::Launch;
  trigger.exe = "-";
  trigger.start_immediately = true;
  ASSERT_TRUE(rig.call(trigger).ok);
  EXPECT_EQ(rig.stateNow(), CaptureState::Capturing);
  EXPECT_FALSE(rig.backend->launchPending());
  EXPECT_EQ(rig.backend->lastStart().exe, "game.exe") << "the trigger does not change the launch";

  EXPECT_EQ(rig.call(trigger).code, ErrorCode::Busy) << "a third start while capturing";
  ASSERT_TRUE(rig.call(StopRequest{}).ok);
  ASSERT_TRUE(rig.waitForState(CaptureState::Idle));
  EXPECT_EQ(rig.countEvents([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }), 1u);
}

TEST(AgentLaunch, CancelTerminatesAParkedLaunch) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch(false)).ok);
  const Response cancel = rig.call(CancelRequest{});
  EXPECT_TRUE(cancel.ok) << cancel.error;
  EXPECT_EQ(rig.stateNow(), CaptureState::Idle);
  EXPECT_EQ(rig.backend->stopCalls(), 1);
  EXPECT_FALSE(rig.backend->launchPending());
  // The agent is usable again.
  EXPECT_TRUE(rig.call(rig.launch()).ok);
}

TEST(AgentLaunch, FailedTriggerReturnsToIdle) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch(false)).ok);
  rig.backend->failNextTrigger = true;
  StartRequest trigger = rig.launch(true);
  const Response r = rig.call(trigger);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(rig.stateNow(), CaptureState::Idle);
}

TEST(AgentLaunch, ShutdownTerminatesAParkedLaunch) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch(false)).ok);
  rig.agent->shutdown();
  EXPECT_EQ(rig.backend->stopCalls(), 1);
  EXPECT_FALSE(rig.backend->launchPending());
}

TEST(AgentLaunch, TargetExitFinishesTheCaptureByItself) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch()).ok);
  EXPECT_EQ(rig.stateNow(), CaptureState::Capturing);
  rig.backend->targetHasExited = true;
  ASSERT_TRUE(rig.waitForState(CaptureState::Idle)) << "the watcher should convert and return to idle";
  EXPECT_EQ(rig.countEvents([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }), 1u);
  EXPECT_TRUE(fs::exists(rig.dir.path() / "out.tachyon"));
  // A later explicit stop reports "no capture" rather than doing anything.
  EXPECT_EQ(rig.call(StopRequest{}).code, ErrorCode::NotFound);
}

TEST(AgentLaunch, TargetStillRunningKeepsCapturing) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch()).ok);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // several watcher polls
  EXPECT_EQ(rig.stateNow(), CaptureState::Capturing);
  EXPECT_TRUE(rig.call(StopRequest{}).ok);
  EXPECT_TRUE(rig.waitForState(CaptureState::Idle));
}

// WP-11 review item 6b: an exception out of the backend's flush must not leave the flushing flag set, or the
// next stop / cancel / shutdown would wait forever.
TEST(AgentFlushGuard, ThrowingFlushDoesNotWedgeStop) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch()).ok);
  rig.backend->flushThrows = true;
  EXPECT_THROW(rig.call(FlushRequest{"hitch", 1}), std::runtime_error);

  auto stopped = std::async(std::launch::async, [&] { return rig.call(StopRequest{}); });
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "stop is stuck behind the flush flag";
  EXPECT_TRUE(stopped.get().ok);
  EXPECT_TRUE(rig.waitForState(CaptureState::Idle));
}

TEST(AgentFlushGuard, ErrorReturnOfFlushAlsoClearsTheFlag) {
  Rig rig;
  ASSERT_TRUE(rig.call(rig.launch()).ok);
  EXPECT_FALSE(rig.call(FlushRequest{"hitch", 1}).ok);  // the fake rejects: not circular
  EXPECT_FALSE(rig.call(FlushRequest{"hitch", 1}).ok);  // a second flush is not "already in progress"
  EXPECT_TRUE(rig.call(CancelRequest{}).ok);
}

}  // namespace
}  // namespace tachyon::agent
