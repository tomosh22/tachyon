// tachyon-cli command set (WP-13): argument handling, exit codes, --json, and the capture / resume / no-start
// flows, driven in-process against an in-process agent whose backend is scriptable.
#include "../agent/fake_launch_backend.h"
#include "../agent/fixture_util.h"
#include "../control/test_util.h"
#include "agent/agent.h"
#include "cli/cli.h"
#include "control/server.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <thread>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;
using agent::testing_util::FakeLaunchBackend;
using control::testing_util::TempDir;
using control::testing_util::testAddress;

struct CliResult {
  int code;
  std::string out, err;
};

CliResult runCli(std::vector<std::string> args) {
  std::ostringstream out, err;
  const int code = run(args, out, err);
  return {code, out.str(), err.str()};
}

struct Rig {
  TempDir dir;
  fs::path fixture = agent::testing_util::writeFixtureTrace(dir.path() / "fixture.tachyon");
  std::string address = testAddress("clicmd");
  FakeLaunchBackend* backend = nullptr;
  std::unique_ptr<agent::Agent> core;
  std::unique_ptr<control::Server> server;

  Rig() {
    auto fake = std::make_unique<FakeLaunchBackend>(fixture);
    backend = fake.get();
    agent::AgentConfig ac;
    ac.backendName = "fake";
    ac.agentAddress = address;
    core = std::make_unique<agent::Agent>(std::move(fake), ac);
    auto l = control::listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<control::Server>(std::move(*l), *core);
    core->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
  }
  ~Rig() {
    server->stop();
    core->shutdown();
  }

  CliResult cli(std::vector<std::string> args) {
    args.insert(args.begin(), {"--socket", address});
    return runCli(std::move(args));
  }
  std::string out(const std::string& name = "out.tachyon") const { return (dir.path() / name).string(); }
};

// ---- usage errors ----------------------------------------------------------------------------------------

TEST(CliUsage, MalformedCommandLinesAreRejectedBeforeConnecting) {
  // No agent exists; every one of these must fail with the usage code, not with a connection failure.
  const std::vector<std::vector<std::string>> bad = {
      {"capture"},                                                      // no target
      {"start"},                                                        // no target
      {"capture", "--launch"},                                          // missing value
      {"capture", "--launch", "a.exe", "--system", "--duration", "1"},  // two targets
      {"capture", "--launch", "a.exe", "--attach", "5"},
      {"capture", "--mode", "launch"},                 // launch without exe
      {"capture", "--system"},                         // nothing ends a system capture
      {"capture", "--launch", "a.exe", "--no-start"},  // capture starts by itself
      {"start", "--system", "--no-start"},             // only launches can be parked
      {"start", "--launch", "a.exe", "--no-start", "--wait"},
      {"start", "--launch", "a.exe", "--child-filter", "*.exe"},  // needs --follow-children
      {"start", "--launch", "a.exe", "--duration", "2"},          // capture only
      {"capture", "--launch", "a.exe", "--duration", "0"},
      {"capture", "--launch", "a.exe", "--duration", "-3"},
      {"capture", "--launch", "a.exe", "--duration", "abc"},
      {"capture", "--attach", "notanumber"},
      {"status", "--duration", "1"},
      {"resume", "--launch", "a.exe"},
      {"capture", "--launch", "a.exe", "--hz"},
  };
  for (const auto& args : bad) {
    std::vector<std::string> full = {"--socket", "/nonexistent/agent.sock"};
    full.insert(full.end(), args.begin(), args.end());
    const CliResult r = runCli(full);
    std::string joined;
    for (const auto& a : args) joined += a + " ";
    EXPECT_EQ(r.code, kExitUsage) << joined << "\n" << r.err;
    EXPECT_NE(r.err.find("usage:"), std::string::npos) << joined;
  }
}

TEST(CliUsage, ExitCodeForUnreachableAgentIsTwo) {
  EXPECT_EQ(runCli({"--socket", "/nonexistent/agent.sock", "status"}).code, kExitConnectionFailure);
  EXPECT_EQ(runCli({"--socket", "/nonexistent/agent.sock", "capture", "--launch", "a.exe"}).code,
            kExitConnectionFailure);
}

// ---- argument plumbing -----------------------------------------------------------------------------------

TEST(CliStart, LaunchFlagsReachTheBackend) {
  Rig rig;
  const CliResult r = rig.cli({"start",
                               "--launch",
                               "C:\\games\\game.exe",
                               "--args",
                               "--scenario spin --duration 3 \"two words\"",
                               "--arg",
                               "--literal",
                               "--arg",
                               "x y",
                               "--cwd",
                               "work",
                               "--env",
                               "A=1",
                               "--env",
                               "B=",
                               "--hz",
                               "4000",
                               "--follow-children",
                               "--child-filter",
                               "*.exe",
                               "--circular-mb",
                               "64",
                               "--out",
                               rig.out(),
                               "--no-cswitch",
                               "--no-readythread",
                               "--no-kernel-stacks"});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  const capture::CaptureOptions o = rig.backend->lastStart();
  EXPECT_EQ(o.mode, capture::CaptureMode::Launch);
  EXPECT_EQ(o.exe, "C:\\games\\game.exe");
  EXPECT_EQ(o.args, (std::vector<std::string>{"--scenario", "spin", "--duration", "3", "two words",
                                              "--literal", "x y"}));
  EXPECT_EQ(o.cwd, "work");
  EXPECT_EQ(o.env.at("A"), "1");
  EXPECT_EQ(o.env.at("B"), "");
  EXPECT_EQ(o.samplingHz, 4000u);
  EXPECT_TRUE(o.followChildren);
  EXPECT_EQ(o.childFilter, "*.exe");
  EXPECT_EQ(o.circularMb, 64u);
  EXPECT_FALSE(o.stackCSwitch);
  EXPECT_FALSE(o.stackReady);
  EXPECT_TRUE(o.stackProfile);
  EXPECT_FALSE(o.kernelStacks);
  EXPECT_TRUE(o.startImmediately);
}

TEST(CliStart, AttachAndSystemTargets) {
  Rig rig;
  ASSERT_EQ(rig.cli({"start", "--attach", "1234", "--out", rig.out()}).code, kExitOk);
  EXPECT_EQ(rig.backend->lastStart().mode, capture::CaptureMode::Attach);
  EXPECT_EQ(rig.backend->lastStart().pid, 1234u);
  ASSERT_EQ(rig.cli({"stop", "--wait"}).code, kExitOk);
  ASSERT_EQ(rig.cli({"start", "--system", "--out", rig.out()}).code, kExitOk);
  EXPECT_EQ(rig.backend->lastStart().mode, capture::CaptureMode::System);
  EXPECT_EQ(rig.cli({"cancel"}).code, kExitOk);
}

TEST(CliStart, LegacyModeFlagsStillWork) {
  Rig rig;
  ASSERT_EQ(rig.cli({"start", "--mode", "launch", "--exe", "game.exe", "--args", "-a", "b", "--defer-start",
                     "--out", rig.out()})
                .code,
            kExitOk);
  EXPECT_EQ(rig.backend->lastStart().args, (std::vector<std::string>{"-a", "b"}));
  EXPECT_FALSE(rig.backend->lastStart().startImmediately);
  EXPECT_EQ(rig.cli({"cancel"}).code, kExitOk);
}

// ---- no-start / resume -----------------------------------------------------------------------------------

TEST(CliResume, NoStartParksThenResumeRecords) {
  Rig rig;
  ASSERT_EQ(rig.cli({"start", "--launch", "game.exe", "--no-start", "--out", rig.out()}).code, kExitOk);
  EXPECT_TRUE(rig.backend->launchPending());
  const CliResult status = rig.cli({"status"});
  EXPECT_NE(status.out.find("state: starting"), std::string::npos) << status.out;

  EXPECT_EQ(rig.cli({"stop"}).code, kExitProtocolError) << "a parked launch cannot be stopped";
  ASSERT_EQ(rig.cli({"resume"}).code, kExitOk);
  EXPECT_NE(rig.cli({"status"}).out.find("state: capturing"), std::string::npos);
  const CliResult stop = rig.cli({"stop", "--wait"});
  EXPECT_EQ(stop.code, kExitOk) << stop.err;
  EXPECT_NE(stop.out.find("capture_done"), std::string::npos);
  EXPECT_TRUE(fs::exists(rig.out()));
}

TEST(CliResume, ResumeWithoutAParkedLaunchIsAnError) {
  Rig rig;
  // Idle: resume is just a start of exe "-"; the fake accepts anything, so assert via the busy path instead.
  ASSERT_EQ(rig.cli({"start", "--launch", "game.exe", "--out", rig.out()}).code, kExitOk);
  EXPECT_EQ(rig.cli({"resume"}).code, kExitProtocolError) << "already capturing";
  EXPECT_EQ(rig.cli({"cancel"}).code, kExitOk);
}

TEST(CliResume, CancelDropsAParkedLaunch) {
  Rig rig;
  ASSERT_EQ(rig.cli({"start", "--launch", "game.exe", "--no-start", "--out", rig.out()}).code, kExitOk);
  EXPECT_EQ(rig.cli({"cancel"}).code, kExitOk);
  EXPECT_NE(rig.cli({"status"}).out.find("state: idle"), std::string::npos);
}

// ---- capture ---------------------------------------------------------------------------------------------

TEST(CliCapture, DurationStopsAndWaitsForCaptureDone) {
  Rig rig;
  const auto t0 = std::chrono::steady_clock::now();
  const CliResult r = rig.cli({"capture", "--launch", "game.exe", "--duration", "0.4", "--out", rig.out()});
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ASSERT_EQ(r.code, kExitOk) << r.err;
  EXPECT_GE(elapsed, 0.35);
  EXPECT_NE(r.out.find("capture started"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("capture_done: path="), std::string::npos) << r.out;
  EXPECT_TRUE(fs::exists(rig.out()));
  EXPECT_EQ(rig.backend->stopCalls(), 1);
}

TEST(CliCapture, TargetExitEndsTheCaptureBeforeTheDuration) {
  Rig rig;
  std::thread exiter([&] {
    // Wait until the capture is live, then let the "game" exit.
    for (int i = 0; i < 400 && rig.backend->rootPid() == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rig.backend->targetHasExited = true;
  });
  const auto t0 = std::chrono::steady_clock::now();
  const CliResult r = rig.cli({"capture", "--launch", "game.exe", "--duration", "30", "--out", rig.out()});
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  exiter.join();
  ASSERT_EQ(r.code, kExitOk) << r.err;
  EXPECT_LT(elapsed, 10.0) << "returned on target exit, not after 30 s";
  EXPECT_NE(r.out.find("capture_done"), std::string::npos);
  EXPECT_TRUE(fs::exists(rig.out()));
  EXPECT_EQ(rig.backend->stopCalls(), 1) << "the agent stopped it; the CLI did not send a second stop";
}

TEST(CliCapture, WithoutDurationWaitsForTargetExit) {
  Rig rig;
  std::thread exiter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    rig.backend->targetHasExited = true;
  });
  const CliResult r = rig.cli({"capture", "--attach", "4242", "--out", rig.out()});
  exiter.join();
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("capture_done"), std::string::npos);
}

TEST(CliCapture, TimeoutWithoutExitStopsCapturingAndReportsTwo) {
  Rig rig;
  const CliResult r = rig.cli({"--timeout", "1", "capture", "--launch", "game.exe", "--out", rig.out()});
  EXPECT_EQ(r.code, kExitConnectionFailure) << r.err;
  EXPECT_NE(r.err.find("did not exit within --timeout"), std::string::npos) << r.err;
  EXPECT_TRUE(fs::exists(rig.out())) << "the capture was stopped, not abandoned";
  EXPECT_NE(rig.cli({"status"}).out.find("state: idle"), std::string::npos);
}

TEST(CliCapture, StartFailureIsAProtocolError) {
  Rig rig;
  ASSERT_EQ(rig.cli({"start", "--launch", "game.exe", "--out", rig.out()}).code, kExitOk);
  const CliResult r = rig.cli({"capture", "--launch", "other.exe", "--duration", "1"});
  EXPECT_EQ(r.code, kExitProtocolError);
  EXPECT_NE(r.err.find("EBUSY"), std::string::npos) << r.err;
}

TEST(CliCapture, EventsLostGivesExitFour) {
  Rig rig;
  rig.backend->eventsLostOnStop = 17;
  const CliResult r = rig.cli({"capture", "--launch", "game.exe", "--duration", "0.2", "--out", rig.out()});
  EXPECT_EQ(r.code, kExitEventsLost);
  EXPECT_NE(r.out.find("events_lost=17"), std::string::npos) << r.out;
  EXPECT_NE(r.err.find("lost 17 events"), std::string::npos) << r.err;
  EXPECT_TRUE(fs::exists(rig.out())) << "the trace is still written";
}

TEST(CliCapture, JsonModePrintsOnlyJsonLines) {
  Rig rig;
  const CliResult r =
      rig.cli({"--json", "capture", "--launch", "game.exe", "--duration", "0.2", "--out", rig.out()});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  std::istringstream lines(r.out);
  std::string line;
  int responses = 0, done = 0;
  while (std::getline(lines, line)) {
    const auto j = nlohmann::json::parse(line);  // throws (fails the test) on anything that is not JSON
    if (j.contains("event")) {
      if (j["event"] == "capture_done") {
        ++done;
        EXPECT_EQ(j["path"], rig.out());
        EXPECT_EQ(j["events_lost"], 0);
      }
    } else {
      ++responses;
      EXPECT_TRUE(j["ok"].get<bool>());
    }
  }
  EXPECT_EQ(responses, 1);
  EXPECT_EQ(done, 1);
}

TEST(CliJson, StatusAndErrorsAreJson) {
  Rig rig;
  const CliResult status = rig.cli({"--json", "status"});
  ASSERT_EQ(status.code, kExitOk);
  const auto s = nlohmann::json::parse(status.out);
  EXPECT_EQ(s["state"], "idle");
  EXPECT_EQ(s["backend"], "fake");

  const CliResult stop = rig.cli({"--json", "stop"});
  EXPECT_EQ(stop.code, kExitProtocolError);
  const auto e = nlohmann::json::parse(stop.out);
  EXPECT_FALSE(e["ok"].get<bool>());
  EXPECT_EQ(e["code"], "ENOTFOUND");
}

TEST(CliFlush, StillRoutesToTheAgent) {
  Rig rig;
  ASSERT_EQ(rig.cli({"start", "--system", "--out", rig.out()}).code, kExitOk);
  // The fake backend rejects flushes; the error code must come back as a protocol error.
  EXPECT_EQ(rig.cli({"flush", "--name", "hitch", "--seconds", "5"}).code, kExitProtocolError);
  EXPECT_EQ(rig.cli({"cancel"}).code, kExitOk);
}

}  // namespace
}  // namespace tachyon::cli
