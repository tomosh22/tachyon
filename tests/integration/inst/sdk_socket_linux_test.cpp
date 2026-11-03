// WP-73 (Linux): the SDK <-> agent Unix socket path against a real tachyon-agent process with the mock
// backend. Covers the socket file and directory permissions, the SDK stream arriving through the agent (and
// not through a sidecar file), the SO_PEERCRED check that ties an inst_hello to the connecting process, and
// the refusal of connections from another uid. The same path with real sampling is in
// tests/linux/capture/sdk_socket_test.cpp.
#include "../../unit/agent/process_util.h"
#include "../../unit/control/test_util.h"
#include "../../unit/inst/trace_builder.h"
#include "control/client.h"
#include "control/transport.h"
#include "inst/merge.h"
#include "trace/trace_reader.h"

#include <grp.h>
#include <gtest/gtest.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <thread>

namespace tachyon {
namespace {

namespace fs = std::filesystem;
using agent::testing_util::Process;
using agent::testing_util::readTextFile;
using control::testing_util::TempDir;

// Sets TACHYON_AGENT_PIPE / TACHYON_SIDECAR for the children spawned inside its lifetime.
class ScopedEnv {
 public:
  ScopedEnv(const std::string& agentAddress, const std::string& sidecar) {
    ::setenv("TACHYON_AGENT_PIPE", agentAddress.c_str(), 1);
    ::setenv("TACHYON_SIDECAR", sidecar.c_str(), 1);
  }
  ~ScopedEnv() {
    ::unsetenv("TACHYON_AGENT_PIPE");
    ::unsetenv("TACHYON_SIDECAR");
  }
};

mode_t permissionsOf(const fs::path& p) {
  struct stat st {};
  if (::stat(p.c_str(), &st) != 0) return ~mode_t{0};
  return st.st_mode & 07777;
}

bool isSocket(const fs::path& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

// The first tachyon-inst-<pid>.sock in `dir`, or an empty path.
fs::path findInstSocket(const fs::path& dir) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("tachyon-inst-", 0) == 0 && name.size() > 5 && name.substr(name.size() - 5) == ".sock" &&
        isSocket(entry.path()))
      return entry.path();
  }
  return {};
}

// Forks a child that drops to `nobody`, connects to the socket at `path` and exits 0 on EACCES/EPERM, 1 on a
// successful connect, 2 on another error. Only async-signal-safe calls after the fork.
int connectAsNobody(const fs::path& path) {
  const pid_t child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (::setgroups(0, nullptr) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0) ::_exit(3);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) ::_exit(2);
    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    const int err = errno;
    ::close(fd);
    ::_exit(rc == 0 ? 1 : (err == EACCES || err == EPERM) ? 0 : 2);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// A real tachyon-agent (mock backend) in a private 0700 directory.
struct RealAgent {
  TempDir dir;
  fs::path sockDir;
  std::string address;
  Process process;

  RealAgent() {
    sockDir = dir.path() / "run";
    fs::create_directories(sockDir);
    ::chmod(sockDir.c_str(), 0700);
    address = (sockDir / "tachyon-agent.sock").string();
    const fs::path fixture = dir.path() / "fixture.tachyon";
    inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
    process = Process::spawn(TACHYON_AGENT_EXE,
                             {"--backend", "mock", "--socket", address, "--mock-fixture", fixture.string()},
                             dir.path() / "agent.log");
  }
  ~RealAgent() {
    process.requestTerminate();
    int code = -1;
    if (!process.waitFor(10000, &code)) process.kill();
  }
  std::string log() const { return readTextFile(dir.path() / "agent.log"); }
  Result<control::Client> connect() const { return control::Client::connectWithRetry(address, 15000); }
};

bool isCaptureDone(const control::Event& e) {
  return std::holds_alternative<control::CaptureDoneEvent>(e);
}

control::CaptureSummary stopAndWait(control::Client& client) {
  auto stop = client.call(control::StopRequest{}, 10000);
  EXPECT_TRUE(stop.ok());
  EXPECT_TRUE(stop->ok) << stop->error;
  auto done = client.waitForEvent(isCaptureDone, 60000);
  EXPECT_TRUE(done.ok());
  return std::get<control::CaptureDoneEvent>(*done).summary;
}

control::StartRequest systemStart(const fs::path& out) {
  control::StartRequest start;
  start.mode = capture::CaptureMode::System;
  start.out_path = out.string();
  return start;
}

TEST(SdkSocketLinux, TestbedStreamsThroughTheAgentSocketWithPrivatePermissions) {
  RealAgent agent;
  ASSERT_TRUE(agent.process.valid());
  auto client = agent.connect();
  ASSERT_TRUE(client.ok()) << agent.log();

  // The control socket is owner-only and so is the directory it lives in.
  EXPECT_TRUE(isSocket(agent.address));
  EXPECT_EQ(permissionsOf(agent.address), 0600u);
  EXPECT_EQ(permissionsOf(agent.sockDir), 0700u);

  const fs::path out = agent.dir.path() / "tb.tachyon";
  auto started = client->call(systemStart(out), 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;

  const fs::path sidecar = agent.dir.path() / "must-not-exist.tachyon-inst";
  Process testbed;
  {
    ScopedEnv env(agent.address, sidecar.string());
    testbed =
        Process::spawn(TACHYON_TESTBED_EXE, {"--instrument", "--scenario", "spin,lock", "--duration", "3"},
                       agent.dir.path() / "testbed.log");
  }
  ASSERT_TRUE(testbed.valid());

  // While the testbed runs, its per-process instrumentation socket exists next to the agent socket, 0600.
  fs::path instSocket;
  int exitCode = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool exited = false;
  while (!exited && std::chrono::steady_clock::now() < deadline) {
    if (instSocket.empty()) {
      instSocket = findInstSocket(agent.sockDir);
      if (!instSocket.empty()) EXPECT_EQ(permissionsOf(instSocket), 0600u);
    }
    exited = testbed.waitFor(50, &exitCode);
  }
  ASSERT_TRUE(exited) << "the testbed did not finish";
  EXPECT_EQ(exitCode, 0) << readTextFile(agent.dir.path() / "testbed.log");
  EXPECT_FALSE(instSocket.empty()) << "no tachyon-inst-<pid>.sock appeared next to the agent socket";

  const control::CaptureSummary summary = stopAndWait(*client);
  EXPECT_EQ(fs::path(summary.path), out);
  // The peer-pid check passed for the testbed: nothing was refused.
  for (const std::string& w : summary.warnings) EXPECT_EQ(w.find("refused"), std::string::npos) << w;

  // No sidecar file: the stream went over the socket.
  EXPECT_FALSE(fs::exists(sidecar));
  for (const auto& entry : fs::recursive_directory_iterator(agent.dir.path()))
    EXPECT_NE(entry.path().extension(), ".tachyon-inst") << entry.path();

  auto opened = trace::TraceReader::open(out);
  ASSERT_TRUE(opened.ok()) << opened.error().toString();
  EXPECT_NE(opened->header().flags & trace::kFlagHasInstrumentation, 0u);
  const trace::StringTable strings = opened->readStrings().value();
  const auto scopes = opened->readScopeEvents().value();
  ASSERT_GT(scopes.size(), 1000u);
  const inst::ScopeBalance balance = inst::checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);
  EXPECT_EQ(balance.begins, balance.ends);
  std::set<std::string> names;
  for (const auto& rec : scopes) {
    const trace::ScopeEventRec r = rec;
    if (r.kind == trace::kScopeBegin) names.insert(std::string(strings.get(r.id)));
  }
  for (const char* expected : {"spin_cycle", "hot_a", "lock_critical_section"})
    EXPECT_TRUE(names.count(expected) != 0) << "missing scope " << expected;
}

TEST(SdkSocketLinux, ConnectionFromAnotherUidIsRefused) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root to switch to another uid";
  struct passwd* nobody = ::getpwnam("nobody");
  if (nobody == nullptr || nobody->pw_uid != 65534) GTEST_SKIP() << "no user 'nobody' with uid 65534";
  RealAgent agent;
  ASSERT_TRUE(agent.process.valid());
  auto client = agent.connect();
  ASSERT_TRUE(client.ok()) << agent.log();
  auto started = client->call(systemStart(agent.dir.path() / "uid.tachyon"), 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;

  Process testbed;
  {
    ScopedEnv env(agent.address, "off");
    testbed = Process::spawn(TACHYON_TESTBED_EXE, {"--instrument", "--scenario", "spin", "--duration", "6"},
                             agent.dir.path() / "testbed.log");
  }
  ASSERT_TRUE(testbed.valid());
  fs::path instSocket;
  for (int i = 0; i < 400 && instSocket.empty(); ++i) {
    instSocket = findInstSocket(agent.sockDir);
    if (instSocket.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  ASSERT_FALSE(instSocket.empty());

  // The 0700 directory stops the other uid before it reaches the sockets.
  EXPECT_EQ(connectAsNobody(agent.address), 0) << "control socket in a 0700 directory";
  EXPECT_EQ(connectAsNobody(instSocket), 0) << "instrumentation socket in a 0700 directory";
  // With the directory opened up, the 0600 socket files alone refuse it.
  ASSERT_EQ(::chmod(agent.sockDir.c_str(), 0755), 0);
  ASSERT_EQ(::chmod(agent.dir.path().c_str(), 0755), 0);
  EXPECT_EQ(connectAsNobody(agent.address), 0) << "control socket file mode 0600";
  EXPECT_EQ(connectAsNobody(instSocket), 0) << "instrumentation socket file mode 0600";
  ASSERT_EQ(::chmod(agent.sockDir.c_str(), 0700), 0);

  // The owner still gets through.
  auto again = control::Client::connectWithRetry(agent.address, 5000);
  EXPECT_TRUE(again.ok());

  int code = -1;
  if (!testbed.waitFor(30000, &code)) testbed.kill();
  auto summary = stopAndWait(*client);
  (void)summary;
}

TEST(SdkSocketLinux, InstrumentationConnectionFromTheWrongProcessIsRefused) {
  RealAgent agent;
  ASSERT_TRUE(agent.process.valid());
  auto client = agent.connect();
  ASSERT_TRUE(client.ok()) << agent.log();
  auto started = client->call(systemStart(agent.dir.path() / "peer.tachyon"), 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;

  // This process says it is pid 1 (a system capture accepts any pid in inst_hello), then connects to the pipe
  // the agent hands out. SO_PEERCRED says the peer is the test process, not pid 1, so the agent drops it.
  control::InstHelloRequest hello;
  hello.pid = 1;
  hello.api_version = 0x00010000;
  auto reply = client->call(hello, 10000);
  ASSERT_TRUE(reply.ok());
  ASSERT_TRUE(reply->ok) << reply->error;
  const auto* body = std::get_if<control::InstHelloBody>(&reply->body);
  ASSERT_NE(body, nullptr);
  auto raw = control::connectTo(body->pipe);
  ASSERT_TRUE(raw.ok()) << raw.error().toString();
  std::string line;
  auto read = (*raw)->readLine(line, 5000);  // the agent closes the connection: end of stream, never data
  EXPECT_FALSE(read.ok() && *read);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const control::CaptureSummary summary = stopAndWait(*client);
  bool refused = false;
  for (const std::string& w : summary.warnings) refused |= w.find("refused") != std::string::npos;
  EXPECT_TRUE(refused) << "the capture must report the refused instrumentation connection";
}

}  // namespace
}  // namespace tachyon
