// Acceptance demo (WP-10): real tachyon-agent and tachyon-cli processes with the mock backend.
#include "../control/test_util.h"
#include "control/client.h"
#include "fixture_util.h"
#include "process_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using control::testing_util::testAddress;
using testing_util::Process;
using testing_util::readTextFile;
using testing_util::runToCompletion;

void setEnv(const char* key, const std::string& value) {
#if TACHYON_PLATFORM_WINDOWS
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

class Acceptance : public ::testing::Test {
 protected:
  void SetUp() override {
    m_fixture = testing_util::writeFixtureTrace(m_dir.path() / "fixture.tachyon");
    setEnv("TACHYON_MOCK_FIXTURE", m_fixture.string());
    m_address = testAddress("accept");
  }
  void TearDown() override { setEnv("TACHYON_MOCK_FIXTURE", ""); }

  // Starts the agent and waits until it answers status.
  Process startAgent(const std::vector<std::string>& extra = {}) {
    std::vector<std::string> args = {"--backend", "mock", "--socket", m_address};
    args.insert(args.end(), extra.begin(), extra.end());
    Process p = Process::spawn(TACHYON_AGENT_EXE, args, m_dir.path() / "agent.log");
    EXPECT_TRUE(p.valid());
    auto c = control::Client::connectWithRetry(m_address, 15000);
    EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().toString()) << "\n"
                        << readTextFile(m_dir.path() / "agent.log");
    return p;
  }

  int cli(const std::vector<std::string>& args, const std::string& outName) {
    std::vector<std::string> full = {"--socket", m_address};
    full.insert(full.end(), args.begin(), args.end());
    return runToCompletion(TACHYON_CLI_EXE, full, m_dir.path() / outName);
  }

  void expectIdenticalToFixture(const fs::path& out) {
    ASSERT_TRUE(fs::exists(out));
    EXPECT_TRUE(trace::TraceReader::open(out).ok());
    EXPECT_EQ(readTextFile(out), readTextFile(m_fixture));
  }

  void stopAgent(Process& agent) {
    agent.requestTerminate();
    int code = -1;
    if (!agent.waitFor(10000, &code)) {
      agent.kill();
      // Windows cannot always deliver CTRL_BREAK without a console; only Linux must exit cleanly.
#if !TACHYON_PLATFORM_WINDOWS
      ADD_FAILURE() << "agent did not exit after SIGTERM";
#endif
      return;
    }
#if !TACHYON_PLATFORM_WINDOWS
    EXPECT_EQ(code, 0) << readTextFile(m_dir.path() / "agent.log");
#endif
  }

  TempDir m_dir;
  fs::path m_fixture;
  std::string m_address;
};

TEST_F(Acceptance, StartThenStopWaitYieldsFixtureAndCaptureDone) {
  Process agent = startAgent();
  const fs::path out = m_dir.path() / "x.tachyon";

  EXPECT_EQ(cli({"status"}, "status.txt"), 0);
  EXPECT_NE(readTextFile(m_dir.path() / "status.txt").find("state: idle"), std::string::npos);

  ASSERT_EQ(cli({"start", "--mode", "system", "--out", out.string()}, "start.txt"), 0);
  ASSERT_EQ(cli({"stop", "--wait"}, "stop.txt"), 0);
  const std::string stopOut = readTextFile(m_dir.path() / "stop.txt");
  EXPECT_NE(stopOut.find("capture_done"), std::string::npos) << stopOut;
  EXPECT_NE(stopOut.find("x.tachyon"), std::string::npos);
  expectIdenticalToFixture(out);

  stopAgent(agent);
}

TEST_F(Acceptance, StartWaitCompletesWhenAnotherClientStops) {
  Process agent = startAgent({"--mock-duration-ms", "200", "--mock-steps", "4"});
  const fs::path out = m_dir.path() / "w.tachyon";

  Process waiter = Process::spawn(
      TACHYON_CLI_EXE,
      {"--socket", m_address, "--json", "start", "--mode", "system", "--out", out.string(), "--wait"},
      m_dir.path() / "wait.txt");
  ASSERT_TRUE(waiter.valid());
  // Wait until the capture is running, then stop it from a second CLI process.
  bool capturing = false;
  for (int i = 0; i < 400 && !capturing; ++i) {
    ASSERT_EQ(cli({"--json", "status"}, "poll.txt"), 0);
    capturing = readTextFile(m_dir.path() / "poll.txt").find("\"state\":\"capturing\"") != std::string::npos;
    if (!capturing) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  ASSERT_TRUE(capturing);
  ASSERT_EQ(cli({"stop"}, "stop.txt"), 0);

  int code = -1;
  ASSERT_TRUE(waiter.waitFor(30000, &code));
  EXPECT_EQ(code, 0);
  const std::string text = readTextFile(m_dir.path() / "wait.txt");
  EXPECT_NE(text.find("\"event\":\"capture_done\""), std::string::npos) << text;
  expectIdenticalToFixture(out);

  stopAgent(agent);
}

TEST_F(Acceptance, SecondAgentOnSameAddressRefusesToStart) {
  Process agent = startAgent();
  EXPECT_NE(runToCompletion(TACHYON_AGENT_EXE, {"--backend", "mock", "--socket", m_address},
                            m_dir.path() / "second.log"),
            0);
  stopAgent(agent);
}

TEST_F(Acceptance, UnavailableBackendFailsAtStartup) {
#if TACHYON_PLATFORM_WINDOWS
  const char* unavailable = "perf";
#else
  const char* unavailable = "etw";
#endif
  EXPECT_NE(runToCompletion(TACHYON_AGENT_EXE, {"--backend", unavailable, "--socket", m_address},
                            m_dir.path() / "bad.log"),
            0);
  EXPECT_NE(readTextFile(m_dir.path() / "bad.log").find("not available in this build"), std::string::npos);
}

TEST_F(Acceptance, CliWithoutAgentExitsTwo) {
  EXPECT_EQ(cli({"status"}, "none.txt"), 2);
}

}  // namespace
}  // namespace tachyon::agent
