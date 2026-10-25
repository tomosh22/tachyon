// tachyon-cli tests: argument handling and exit codes, driven in-process against an in-process agent.
#include "cli/cli.h"
#include "../control/test_util.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "fixture_util.h"
#include "process_util.h"

#include <gtest/gtest.h>

#include <sstream>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;
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

struct AgentHarness {
  TempDir dir;
  fs::path fixture = agent::testing_util::writeFixtureTrace(dir.path() / "fixture.tachyon");
  std::string address = testAddress("cli");
  std::unique_ptr<agent::Agent> core;
  std::unique_ptr<control::Server> server;

  AgentHarness() {
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture;
    agent::AgentConfig ac;
    ac.backendName = "mock";
    ac.agentAddress = address;
    core = std::make_unique<agent::Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto l = control::listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<control::Server>(std::move(*l), *core);
    core->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
  }
  ~AgentHarness() {
    server->stop();
    core->shutdown();
  }
};

TEST(Cli, UsageErrorsExitThree) {
  EXPECT_EQ(runCli({}).code, kExitUsage);
  EXPECT_EQ(runCli({"frobnicate"}).code, kExitUsage);
  EXPECT_EQ(runCli({"start"}).code, kExitUsage);  // missing --mode
  EXPECT_EQ(runCli({"start", "--mode", "warp"}).code, kExitUsage);
  EXPECT_EQ(runCli({"flush", "--name", "x"}).code, kExitUsage);  // missing --seconds
  EXPECT_EQ(runCli({"status", "--bogus"}).code, kExitUsage);
  EXPECT_EQ(runCli({"status", "--wait"}).code, kExitUsage);
  EXPECT_EQ(runCli({"start", "--mode", "system", "--hz", "abc"}).code, kExitUsage);
}

TEST(Cli, ConnectionFailureExitsTwo) {
  auto r = runCli({"--socket", testAddress("nobody"), "status"});
  EXPECT_EQ(r.code, kExitConnectionFailure);
  EXPECT_NE(r.err.find("cannot connect"), std::string::npos);
}

TEST(Cli, StatusTextAndJson) {
  AgentHarness h;
  auto text = runCli({"--socket", h.address, "status"});
  EXPECT_EQ(text.code, kExitOk);
  EXPECT_NE(text.out.find("state: idle"), std::string::npos) << text.out;
  EXPECT_NE(text.out.find("backend: mock"), std::string::npos);

  auto json = runCli({"--socket", h.address, "--json", "status"});
  EXPECT_EQ(json.code, kExitOk);
  EXPECT_NE(json.out.find("\"state\":\"idle\""), std::string::npos) << json.out;
  EXPECT_NE(json.out.find("\"ok\":true"), std::string::npos);
}

TEST(Cli, StartStopWaitProducesCaptureDone) {
  AgentHarness h;
  const std::string out = (h.dir.path() / "x.tachyon").string();
  auto start = runCli({"--socket", h.address, "start", "--mode", "system", "--out", out, "--hz", "4000"});
  EXPECT_EQ(start.code, kExitOk) << start.err;
  auto stop = runCli({"--socket", h.address, "stop", "--wait"});
  EXPECT_EQ(stop.code, kExitOk) << stop.err;
  EXPECT_NE(stop.out.find("capture_done"), std::string::npos) << stop.out;
  EXPECT_NE(stop.out.find(out), std::string::npos);
  EXPECT_EQ(agent::testing_util::readTextFile(out), agent::testing_util::readTextFile(h.fixture));
}

TEST(Cli, JsonWaitPrintsEventLine) {
  AgentHarness h;
  const std::string out = (h.dir.path() / "j.tachyon").string();
  ASSERT_EQ(runCli({"--socket", h.address, "start", "--mode", "system", "--out", out}).code, kExitOk);
  auto stop = runCli({"--socket", h.address, "--json", "stop", "--wait"});
  EXPECT_EQ(stop.code, kExitOk);
  EXPECT_NE(stop.out.find("\"event\":\"capture_done\""), std::string::npos) << stop.out;
}

TEST(Cli, ProtocolErrorExitsOne) {
  AgentHarness h;
  auto stop = runCli({"--socket", h.address, "stop"});
  EXPECT_EQ(stop.code, kExitProtocolError);
  EXPECT_NE(stop.err.find("ENOTFOUND"), std::string::npos) << stop.err;

  const std::string out = (h.dir.path() / "e.tachyon").string();
  ASSERT_EQ(runCli({"--socket", h.address, "start", "--mode", "system", "--out", out}).code, kExitOk);
  auto again = runCli({"--socket", h.address, "start", "--mode", "system", "--out", out});
  EXPECT_EQ(again.code, kExitProtocolError);
  EXPECT_NE(again.err.find("EBUSY"), std::string::npos);
}

TEST(Cli, FlushAndCancel) {
  AgentHarness h;
  const std::string out = (h.dir.path() / "c.tachyon").string();
  ASSERT_EQ(
      runCli({"--socket", h.address, "start", "--mode", "system", "--out", out, "--circular-mb", "64"}).code,
      kExitOk);
  auto flush = runCli({"--socket", h.address, "flush", "--name", "hitch-1", "--seconds", "10"});
  EXPECT_EQ(flush.code, kExitOk) << flush.err;
  EXPECT_TRUE(fs::exists(h.dir.path() / "hitch-1.tachyon"));
  EXPECT_EQ(runCli({"--socket", h.address, "cancel"}).code, kExitOk);
  EXPECT_FALSE(fs::exists(h.dir.path() / "c.tachyon"));
}

TEST(Cli, StartOptionsReachTheAgent) {
  AgentHarness h;
  const std::string out = (h.dir.path() / "o.tachyon").string();
  auto r = runCli({"--socket",
                   h.address,
                   "start",
                   "--mode",
                   "launch",
                   "--exe",
                   "game.exe",
                   "--args",
                   "-a",
                   "b",
                   "--env",
                   "K=V",
                   "--cwd",
                   "/w",
                   "--follow-children",
                   "--child-filter",
                   "*.exe",
                   "--no-cswitch",
                   "--no-kernel-stacks",
                   "--defer-start",
                   "--out",
                   out});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(runCli({"--socket", h.address, "start", "--mode", "attach"}).code, kExitProtocolError)
      << "attach without --pid is rejected by the agent";
}

}  // namespace
}  // namespace tachyon::cli
