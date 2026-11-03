// WP-61: real tachyon-agent and tachyon-cli processes over TCP loopback (mock backend): the agent's startup
// checks of the token file, the CLI's --agent tcp://... with `capture --fetch`, `fetch`, `processes`, token
// failures, and the rule that the token never shows up in the agent's log.
#include "../control/test_util.h"
#include "control/client.h"
#include "control/tcp.h"
#include "fixture_util.h"
#include "process_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <thread>

#if !TACHYON_PLATFORM_WINDOWS
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using testing_util::Process;
using testing_util::readTextFile;
using testing_util::runToCompletion;

const std::string kToken = "process-test-token-0123456789-ABCDEFGH";

void writeTokenFile(const fs::path& path, const std::string& content) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
#if !TACHYON_PLATFORM_WINDOWS
  ::chmod(path.string().c_str(), 0600);
#endif
}

// A port that is free right now.
uint16_t freePort() {
  auto l = control::listenTcp(control::TcpEndpoint{"127.0.0.1", 0});
  EXPECT_TRUE(l.ok());
  auto ep = control::parseTcpEndpoint((*l)->address());
  return ep.ok() ? ep->port : 0;
}

class RemoteProcess : public ::testing::Test {
 protected:
  void SetUp() override {
    m_fixture = testing_util::writeFixtureTrace(m_dir.path() / "fixture.tachyon");
    m_token = m_dir.path() / "token";
    writeTokenFile(m_token, kToken + "\n");
    m_outDir = m_dir.path() / "agent-out";
    fs::create_directories(m_outDir);
    m_localDir = m_dir.path() / "client";
    fs::create_directories(m_localDir);
    m_port = freePort();
    m_tcp = "tcp://127.0.0.1:" + std::to_string(m_port);
    m_socket = control::testing_util::testAddress("remproc");
  }

  Process startAgent(const std::vector<std::string>& extra = {}) {
    std::vector<std::string> args = {"--backend",        "mock",
                                     "--socket",         m_socket,
                                     "--mock-fixture",   m_fixture.string(),
                                     "--listen",         "127.0.0.1:" + std::to_string(m_port),
                                     "--token-file",     m_token.string(),
                                     "--remote-out-dir", m_outDir.string()};
    args.insert(args.end(), extra.begin(), extra.end());
    Process p = Process::spawn(TACHYON_AGENT_EXE, args, m_dir.path() / "agent.log");
    EXPECT_TRUE(p.valid());
    auto c = control::Client::connectWithRetry(m_tcp, 15000, kToken);
    EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().toString()) << "\n" << agentLog();
    return p;
  }

  std::string agentLog() const { return readTextFile(m_dir.path() / "agent.log"); }

  int cli(std::vector<std::string> args, const std::string& outName, bool withToken = true) {
    std::vector<std::string> full = {"--agent", m_tcp};
    if (withToken) {
      full.push_back("--token-file");
      full.push_back(m_token.string());
    }
    full.insert(full.end(), args.begin(), args.end());
    return runToCompletion(TACHYON_CLI_EXE, full, m_dir.path() / outName);
  }

  void stopAgent(Process& agent) {
    agent.requestTerminate();
    int code = -1;
    if (!agent.waitFor(10000, &code)) agent.kill();
  }

  TempDir m_dir;
  fs::path m_fixture, m_token, m_outDir, m_localDir;
  uint16_t m_port = 0;
  std::string m_tcp, m_socket;
};

TEST_F(RemoteProcess, AgentRefusesToListenWithoutAGoodTokenFile) {
  auto run = [&](const std::vector<std::string>& extra) {
    std::vector<std::string> args = {"--backend", "mock", "--socket", m_socket};
    args.insert(args.end(), extra.begin(), extra.end());
    return runToCompletion(TACHYON_AGENT_EXE, args, m_dir.path() / "refuse.log", 20000);
  };
  const std::string listen = "127.0.0.1:" + std::to_string(m_port);
  EXPECT_EQ(run({"--listen", listen}), 2) << "no --token-file";
  EXPECT_EQ(run({"--listen", listen, "--token-file", (m_dir.path() / "missing").string()}), 1);
  EXPECT_NE(readTextFile(m_dir.path() / "refuse.log").find("does not exist"), std::string::npos);

  const fs::path bad = m_dir.path() / "bad-token";
  writeTokenFile(bad, "");
  EXPECT_EQ(run({"--listen", listen, "--token-file", bad.string()}), 1) << "empty";
  writeTokenFile(bad, "too-short-token\n");
  EXPECT_EQ(run({"--listen", listen, "--token-file", bad.string()}), 1) << "shorter than 32";
  std::string log = readTextFile(m_dir.path() / "refuse.log");
  EXPECT_NE(log.find("at least 32"), std::string::npos) << log;
  EXPECT_EQ(log.find("too-short-token"), std::string::npos) << "the token is never printed";
#if !TACHYON_PLATFORM_WINDOWS
  writeTokenFile(bad, kToken + "\n");
  ::chmod(bad.string().c_str(), 0640);
  EXPECT_EQ(run({"--listen", listen, "--token-file", bad.string()}), 1) << "group-readable";
  log = readTextFile(m_dir.path() / "refuse.log");
  EXPECT_NE(log.find("chmod 600"), std::string::npos) << log;
  EXPECT_EQ(log.find(kToken), std::string::npos);
  ::chmod(bad.string().c_str(), 0644);
  EXPECT_EQ(run({"--listen", listen, "--token-file", bad.string()}), 1) << "world-readable";
#endif
  EXPECT_EQ(run({"--listen", "7331", "--token-file", m_token.string()}), 2) << "host:port needs the host";
  EXPECT_EQ(run({"--listen", ":7331", "--token-file", m_token.string()}), 2);
  EXPECT_EQ(run({"--token-file", m_token.string()}), 2) << "a token file without --listen";
}

TEST_F(RemoteProcess, CliCapturesAndFetchesOverTcp) {
  Process agent = startAgent();
  const fs::path dir = m_localDir / "fetched";

  // attach is allowed remotely; --fetch copies the finished trace to this machine.
  const int code = cli({"capture", "--attach", "1", "--duration", "1", "--fetch", "--fetch-dir", dir.string(),
                        "--out", "/some/where/else/cap.tachyon"},
                       "capture.txt");
  const std::string out = readTextFile(m_dir.path() / "capture.txt");
  ASSERT_EQ(code, 0) << out << "\n" << agentLog();
  EXPECT_NE(out.find("fetched"), std::string::npos) << out;
  // The agent wrote it in its own output directory (the remote client chose only the name).
  EXPECT_TRUE(fs::exists(m_outDir / "cap.tachyon"));
  const fs::path got = dir / "cap.tachyon";
  ASSERT_TRUE(fs::exists(got)) << out;
  EXPECT_TRUE(trace::TraceReader::open(got).ok());
  EXPECT_EQ(readTextFile(got), readTextFile(m_fixture));
  EXPECT_FALSE(fs::exists(got.string() + ".part"));

  // The explicit command, with --json.
  const fs::path second = m_localDir / "again.tachyon";
  EXPECT_EQ(
      cli({"fetch", (m_outDir / "cap.tachyon").string(), "--out", second.string(), "--json"}, "fetch.json"),
      0);
  const std::string json = readTextFile(m_dir.path() / "fetch.json");
  EXPECT_NE(json.find("\"fetched\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"sha256\""), std::string::npos);
  EXPECT_EQ(readTextFile(second), readTextFile(m_fixture));

  // Anything the agent did not produce is refused: exit 1, nothing written.
  const fs::path denied = m_localDir / "denied";
  EXPECT_EQ(cli({"fetch", m_fixture.string(), "--out", denied.string()}, "denied.txt"), 1);
  EXPECT_FALSE(fs::exists(denied));
  EXPECT_FALSE(fs::exists(denied.string() + ".part"));
  EXPECT_EQ(
      cli({"fetch", (m_outDir / ".." / "fixture.tachyon").string(), "--out", denied.string()}, "denied2.txt"),
      1);
  EXPECT_EQ(cli({"fetch", "/etc/passwd", "--out", denied.string()}, "denied3.txt"), 1);

  // Launch is refused without --allow-remote-launch (ENOPRIV -> exit 1).
  EXPECT_EQ(cli({"capture", "--launch", "/bin/true", "--duration", "1"}, "launch.txt"), 1);
  EXPECT_NE(readTextFile(m_dir.path() / "launch.txt").find("ENOPRIV"), std::string::npos);

  // processes
  EXPECT_EQ(cli({"processes"}, "procs.txt"), 0);
  EXPECT_FALSE(readTextFile(m_dir.path() / "procs.txt").empty());

  // The local socket needs no token.
  EXPECT_EQ(runToCompletion(TACHYON_CLI_EXE, {"--socket", m_socket, "status"}, m_dir.path() / "local.txt"),
            0);

  // The token never reached the log, whatever happened.
  stopAgent(agent);
  const std::string log = agentLog();
  EXPECT_EQ(log.find(kToken), std::string::npos) << log;
  EXPECT_NE(log.find("listening on TCP 127.0.0.1:"), std::string::npos) << log;
  EXPECT_NE(log.find("unencrypted"), std::string::npos);
  EXPECT_NE(log.find("remote launch disabled"), std::string::npos);
}

TEST_F(RemoteProcess, AllowRemoteLaunchOptsIn) {
  Process agent = startAgent({"--allow-remote-launch"});
  const int code = cli({"start", "--launch", "/bin/true"}, "start.txt");
  EXPECT_EQ(code, 0) << readTextFile(m_dir.path() / "start.txt") << agentLog();
  EXPECT_EQ(cli({"cancel"}, "cancel.txt"), 0);
  stopAgent(agent);
  EXPECT_NE(agentLog().find("remote launch ALLOWED"), std::string::npos);
}

TEST_F(RemoteProcess, TokenProblemsAreReportedAndLogged) {
  Process agent = startAgent();
  // No token at all: usage error before anything is sent.
  EXPECT_EQ(cli({"status"}, "notoken.txt", /*withToken=*/false), 3);
  EXPECT_NE(readTextFile(m_dir.path() / "notoken.txt").find("token"), std::string::npos);

  // TACHYON_AGENT_TOKEN_FILE works instead of --token-file.
#if !TACHYON_PLATFORM_WINDOWS
  ::setenv("TACHYON_AGENT_TOKEN_FILE", m_token.string().c_str(), 1);
  EXPECT_EQ(cli({"status"}, "envtoken.txt", /*withToken=*/false), 0);
  ::unsetenv("TACHYON_AGENT_TOKEN_FILE");
#endif

  // A wrong token: connection failure (2), not a usage error; the agent logs the peer, never the token.
  const fs::path wrong = m_dir.path() / "wrong";
  const std::string wrongToken = "WRONG-token-0123456789-ABCDEFGHIJKLMNOP";
  writeTokenFile(wrong, wrongToken + "\n");
  auto status = [&] {
    return runToCompletion(TACHYON_CLI_EXE, {"--agent", m_tcp, "--token-file", wrong.string(), "status"},
                           m_dir.path() / "wrong.txt");
  };
  EXPECT_EQ(status(), 2);
  EXPECT_NE(readTextFile(m_dir.path() / "wrong.txt").find("authentication failed"), std::string::npos);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(status(), 2);
  // Five failures from this address: even the right token waits now.
  EXPECT_EQ(cli({"status"}, "blocked.txt"), 2);
  EXPECT_NE(readTextFile(m_dir.path() / "blocked.txt").find("too many"), std::string::npos);

  stopAgent(agent);
  const std::string log = agentLog();
  EXPECT_NE(log.find("authentication failed from 127.0.0.1:"), std::string::npos) << log;
  EXPECT_EQ(log.find(kToken), std::string::npos);
  EXPECT_EQ(log.find(wrongToken), std::string::npos);
}

}  // namespace
}  // namespace tachyon::agent
