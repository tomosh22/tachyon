// WP-41b (Linux): an agent that runs as root on behalf of another user (pkexec, sudo, --socket-owner) hands
// its control socket and the SDK sockets to that user, and to nobody else. Needs root (it switches to uid
// 65534 in forked children, like WP-73's ConnectionFromAnotherUidIsRefused); skips cleanly otherwise.
#include "../../unit/agent/process_util.h"
#include "../../unit/control/test_util.h"
#include "../../unit/inst/trace_builder.h"
#include "agent/privileges.h"
#include "control/client.h"
#include "control/transport.h"

#include <grp.h>
#include <gtest/gtest.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace tachyon {
namespace {

namespace fs = std::filesystem;
using agent::testing_util::Process;
using agent::testing_util::readTextFile;
using control::testing_util::TempDir;

constexpr uid_t kNobody = 65534;

struct stat statOf(const fs::path& p) {
  struct stat st {};
  if (::stat(p.c_str(), &st) != 0) st.st_mode = ~mode_t{0};
  return st;
}

// What a child running as `nobody` finds when it uses the agent.
enum ChildResult : int {
  kRefused = 0,  // connect() failed with EACCES / EPERM
  kConnected = 1,
  kOtherError = 2,
  kSetupError = 3,
  kProtocolError = 4,
};

// Forks a child that drops to nobody and connects a plain stream socket to `path`.
int connectAsNobody(const fs::path& path) {
  const pid_t child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (::setgroups(0, nullptr) != 0 || ::setgid(kNobody) != 0 || ::setuid(kNobody) != 0)
      ::_exit(kSetupError);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) ::_exit(kOtherError);
    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    const int err = errno;
    ::close(fd);
    ::_exit(rc == 0 ? kConnected : (err == EACCES || err == EPERM) ? kRefused : kOtherError);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Forks a child that drops to nobody, uses the control socket (status), says inst_hello with its own pid and
// connects to the instrumentation socket it is given. kConnected when everything worked.
int helloAsNobody(const std::string& address) {
  const pid_t child = ::fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (::setgroups(0, nullptr) != 0 || ::setgid(kNobody) != 0 || ::setuid(kNobody) != 0)
      ::_exit(kSetupError);
    auto client = control::Client::connect(address);
    if (!client.ok()) ::_exit(kRefused);
    auto status = client->call(control::StatusRequest{}, 10000);
    if (!status.ok() || !status->ok) ::_exit(kProtocolError);
    control::InstHelloRequest hello;
    hello.pid = static_cast<uint32_t>(::getpid());
    hello.api_version = 0x00010000;
    auto reply = client->call(hello, 10000);
    if (!reply.ok() || !reply->ok) ::_exit(kProtocolError);
    const auto* body = std::get_if<control::InstHelloBody>(&reply->body);
    if (body == nullptr) ::_exit(kProtocolError);
    auto raw = control::connectTo(body->pipe);
    ::_exit(raw.ok() ? kConnected : kRefused);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// A real tachyon-agent (mock backend) whose socket directory and file are reachable by other users, so that
// only the owner and the 0600 mode decide who gets in.
struct OpenDirAgent {
  TempDir dir;
  fs::path sockDir;
  std::string address;
  Process process;

  explicit OpenDirAgent(const std::vector<std::string>& extraArgs) {
    ::chmod(dir.path().c_str(), 0755);
    sockDir = dir.path() / "run";
    fs::create_directories(sockDir);
    ::chmod(sockDir.c_str(), 0755);
    address = (sockDir / "tachyon-agent.sock").string();
    const fs::path fixture = dir.path() / "fixture.tachyon";
    inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
    std::vector<std::string> args = {"--backend", "mock",           "--socket",
                                     address,     "--mock-fixture", fixture.string()};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    process = Process::spawn(TACHYON_AGENT_EXE, args, dir.path() / "agent.log");
  }
  ~OpenDirAgent() {
    process.requestTerminate();
    int code = -1;
    if (!process.waitFor(10000, &code)) process.kill();
  }
  std::string log() const { return readTextFile(dir.path() / "agent.log"); }
  Result<control::Client> connectAsRoot() const { return control::Client::connectWithRetry(address, 15000); }
};

bool skipUnlessRootWithNobody() {
  if (::geteuid() != 0) return true;
  const struct passwd* nobody = ::getpwnam("nobody");
  return nobody == nullptr || nobody->pw_uid != kNobody;
}

control::StartRequest systemStart(const fs::path& out) {
  control::StartRequest start;
  start.mode = capture::CaptureMode::System;
  start.out_path = out.string();
  return start;
}

TEST(SocketOwnerLinux, ExplicitOwnerCanUseTheControlAndSdkSockets) {
  if (skipUnlessRootWithNobody()) GTEST_SKIP() << "needs root and a user 'nobody' with uid 65534";
  OpenDirAgent agent({"--socket-owner", std::to_string(kNobody)});
  ASSERT_TRUE(agent.process.valid());
  auto root = agent.connectAsRoot();
  ASSERT_TRUE(root.ok()) << agent.log();

  // The socket belongs to nobody and is still owner-only.
  const struct stat st = statOf(agent.address);
  EXPECT_EQ(st.st_uid, kNobody);
  EXPECT_EQ(st.st_mode & 07777, 0600u);
  EXPECT_EQ(connectAsNobody(agent.address), kConnected) << agent.log();

  // A capture is needed before inst_hello is answered.
  auto started = root->call(systemStart(agent.dir.path() / "own.tachyon"), 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;
  EXPECT_EQ(helloAsNobody(agent.address), kConnected) << agent.log();

  // The per-process socket was handed over too, with its 0600 mode, and the peer pid check let the owner's
  // process through (nothing refused).
  fs::path instSocket;
  for (const auto& entry : fs::directory_iterator(agent.sockDir)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("tachyon-inst-", 0) == 0) instSocket = entry.path();
  }
  ASSERT_FALSE(instSocket.empty()) << "no tachyon-inst-<pid>.sock next to the agent socket";
  const struct stat inst = statOf(instSocket);
  EXPECT_EQ(inst.st_uid, kNobody);
  EXPECT_EQ(inst.st_mode & 07777, 0600u);

  auto stop = root->call(control::StopRequest{}, 10000);
  ASSERT_TRUE(stop.ok());
  ASSERT_TRUE(stop->ok) << stop->error;
  auto done = root->waitForEvent(
      [](const control::Event& e) { return std::holds_alternative<control::CaptureDoneEvent>(e); }, 60000);
  ASSERT_TRUE(done.ok());
  for (const std::string& w : std::get<control::CaptureDoneEvent>(*done).summary.warnings)
    EXPECT_EQ(w.find("refused"), std::string::npos) << w;
}

TEST(SocketOwnerLinux, WithoutTheOptionAnotherUserIsRefused) {
  if (skipUnlessRootWithNobody()) GTEST_SKIP() << "needs root and a user 'nobody' with uid 65534";
  OpenDirAgent agent({});
  ASSERT_TRUE(agent.process.valid());
  auto root = agent.connectAsRoot();
  ASSERT_TRUE(root.ok()) << agent.log();
  EXPECT_EQ(statOf(agent.address).st_uid, 0u);
  EXPECT_EQ(statOf(agent.address).st_mode & 07777, 0600u);
  EXPECT_EQ(connectAsNobody(agent.address), kRefused);

  auto started = root->call(systemStart(agent.dir.path() / "none.tachyon"), 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;
  EXPECT_EQ(helloAsNobody(agent.address), kRefused);
  auto cancel = root->call(control::CancelRequest{}, 10000);
  EXPECT_TRUE(cancel.ok());
}

TEST(SocketOwnerLinux, SudoAndPkexecEnvironmentIsHonouredButTheOptionWins) {
  if (skipUnlessRootWithNobody()) GTEST_SKIP() << "needs root and a user 'nobody' with uid 65534";
  ::setenv("SUDO_UID", "65534", 1);
  ::setenv("PKEXEC_UID", "1", 1);
  {
    // PKEXEC_UID comes first, so the agent hands the socket to uid 1 (daemon), not to nobody.
    OpenDirAgent agent({});
    ASSERT_TRUE(agent.process.valid());
    ASSERT_TRUE(agent.connectAsRoot().ok()) << agent.log();
    EXPECT_EQ(statOf(agent.address).st_uid, 1u);
    EXPECT_EQ(connectAsNobody(agent.address), kRefused);
  }
  {
    OpenDirAgent agent({"--socket-owner", "65534"});
    ASSERT_TRUE(agent.process.valid());
    ASSERT_TRUE(agent.connectAsRoot().ok()) << agent.log();
    EXPECT_EQ(statOf(agent.address).st_uid, kNobody);
  }
  ::unsetenv("PKEXEC_UID");
  {
    OpenDirAgent agent({});
    ASSERT_TRUE(agent.process.valid());
    ASSERT_TRUE(agent.connectAsRoot().ok()) << agent.log();
    EXPECT_EQ(statOf(agent.address).st_uid, kNobody) << "SUDO_UID alone";
  }
  ::unsetenv("SUDO_UID");
}

TEST(SocketOwnerLinux, ResolvesTheOwnerFromTheEnvironment) {
  uint32_t uid = 0;
  EXPECT_TRUE(agent::socketOwnerFromEnvironment(true, "1000", nullptr, uid));
  EXPECT_EQ(uid, 1000u);
  EXPECT_TRUE(agent::socketOwnerFromEnvironment(true, nullptr, "1001", uid));
  EXPECT_EQ(uid, 1001u);
  EXPECT_TRUE(agent::socketOwnerFromEnvironment(true, "bogus", "1002", uid));  // falls through to sudo
  EXPECT_EQ(uid, 1002u);
  EXPECT_FALSE(agent::socketOwnerFromEnvironment(false, "1000", "1001", uid))
      << "not root: nothing to hand over";
  EXPECT_FALSE(agent::socketOwnerFromEnvironment(true, "0", "0", uid)) << "never root";
  EXPECT_FALSE(agent::socketOwnerFromEnvironment(true, "-5", "4294967295", uid));
  EXPECT_FALSE(agent::socketOwnerFromEnvironment(true, "", "12x", uid));
  EXPECT_FALSE(agent::socketOwnerFromEnvironment(true, nullptr, nullptr, uid));
  EXPECT_TRUE(agent::parseSocketOwnerUid("65534", uid));
  EXPECT_FALSE(agent::parseSocketOwnerUid("0", uid));
  EXPECT_FALSE(agent::parseSocketOwnerUid("99999999999", uid));
}

TEST(SocketOwnerLinux, BadOptionIsAUsageError) {
  const fs::path log = fs::temp_directory_path() / ("tachyon-owner-" + std::to_string(::getpid()) + ".log");
  const int code = agent::testing_util::runToCompletion(TACHYON_AGENT_EXE,
                                                        {"--backend", "mock", "--socket-owner", "root"}, log);
  EXPECT_EQ(code, 2) << readTextFile(log);
  std::error_code ec;
  fs::remove(log, ec);
}

}  // namespace
}  // namespace tachyon
