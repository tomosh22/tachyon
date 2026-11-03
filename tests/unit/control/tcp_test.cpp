// WP-61: the TCP transport, the token handshake of the server, the token file rules, SHA-256 / base64 and the
// protocol additions (auth, fetch, processes). TCP loopback only; the Windows CI job runs these too.
#include "control/tcp.h"
#include "control/auth.h"
#include "control/client.h"
#include "control/server.h"
#include "control/sha256.h"
#include "core/log.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !TACHYON_PLATFORM_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tachyon::control {
namespace {

using testing_util::TempDir;

const std::string kToken = "0123456789abcdef0123456789abcdef-secret";  // 38 chars

class EchoHandler final : public IRequestHandler {
 public:
  void onClientAccepted(ClientId, const ClientInfo& info) override {
    std::lock_guard lock(mutex);
    infos.push_back(info);
  }
  void onConnect(ClientId) override { ++connects; }
  void onDisconnect(ClientId) override { ++disconnects; }
  Response handle(ClientId, const Request& request) override {
    ++handled;
    StatusBody s;
    s.progress = static_cast<double>(request.id);
    s.backend = "echo";
    return makeOkResponse(std::nullopt, s);
  }
  std::mutex mutex;
  std::vector<ClientInfo> infos;
  std::atomic<int> connects{0}, disconnects{0}, handled{0};
};

// A server with one local and one TCP (port chosen by the system) listener.
struct TcpFixture {
  EchoHandler handler;
  std::string localAddress = testing_util::testAddress("tcpfix");
  std::unique_ptr<Server> server;
  std::string tcpAddress;  // "tcp://127.0.0.1:port"

  explicit TcpFixture(AuthPolicy policy = {}) {
    if (policy.token.empty()) policy.token = kToken;
    auto local = listenOn(localAddress);
    EXPECT_TRUE(local.ok());
    server = std::make_unique<Server>(std::move(*local), handler);
    auto tcp = listenTcp(TcpEndpoint{"127.0.0.1", 0});
    EXPECT_TRUE(tcp.ok()) << (tcp.ok() ? "" : tcp.error().toString());
    tcpAddress = "tcp://" + (*tcp)->address();
    server->addRemoteListener(std::move(*tcp), std::move(policy));
    server->start();
  }
};

// Captures log messages for the lifetime of the object.
class LogCapture {
 public:
  LogCapture() {
    core::setLogSink([this](core::LogLevel, std::string_view text) {
      std::lock_guard lock(m_mutex);
      m_lines.emplace_back(text);
    });
  }
  ~LogCapture() { core::setLogSink(nullptr); }
  std::string all() const {
    std::lock_guard lock(m_mutex);
    std::string out;
    for (const std::string& l : m_lines) out += l + "\n";
    return out;
  }

 private:
  mutable std::mutex m_mutex;
  std::vector<std::string> m_lines;
};

bool waitUntil(const std::function<bool()>& pred, int ms = 5000) {
  for (int i = 0; i < ms / 5; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// ---- addresses ----

TEST(TcpAddress, ParsesHostsAndLiterals) {
  auto a = parseTcpEndpoint("tcp://127.0.0.1:7331");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->host, "127.0.0.1");
  EXPECT_EQ(a->port, 7331);
  auto b = parseTcpEndpoint("[::1]:80");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b->host, "::1");
  EXPECT_EQ(b->port, 80);
  EXPECT_EQ(formatTcpEndpoint(*b), "[::1]:80");
  auto c = parseTcpEndpoint("tcp://build-box.local:0/");
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c->host, "build-box.local");
  EXPECT_EQ(c->port, 0);
  EXPECT_TRUE(parseTcpEndpoint("0.0.0.0:7331").ok()) << "the wildcard is allowed when named explicitly";
}

TEST(TcpAddress, RejectsMalformedAddresses) {
  for (const char* bad : {"", "7331", ":7331", "host", "host:", "host:99999", "host:12x", "::1:80", "[::1]80",
                          "[::1", "tcp://", "tcp://:80"})
    EXPECT_FALSE(parseTcpEndpoint(bad).ok()) << bad;
  EXPECT_TRUE(isTcpAddress("tcp://h:1"));
  EXPECT_FALSE(isTcpAddress("/run/user/1000/tachyon-agent.sock"));
  EXPECT_FALSE(isTcpAddress("tcp://"));
}

TEST(TcpAddress, LoopbackDetection) {
  EXPECT_TRUE(isLoopbackHost("127.0.0.1"));
  EXPECT_TRUE(isLoopbackHost("::1"));
  EXPECT_FALSE(isLoopbackHost("0.0.0.0"));
  EXPECT_FALSE(isLoopbackHost("10.0.0.5"));
}

TEST(TcpTransport, ConnectionsCarryLinesAndReportThePeer) {
  auto listener = listenTcp(TcpEndpoint{"127.0.0.1", 0});
  ASSERT_TRUE(listener.ok());
  const std::string address = (*listener)->address();
  EXPECT_EQ(address.rfind("127.0.0.1:", 0), 0u);
  auto client = connectTo("tcp://" + address);
  ASSERT_TRUE(client.ok()) << client.error().toString();
  EXPECT_FALSE((*client)->isRemote()) << "the connecting side is not the remote one";
  auto accepted = (*listener)->accept();
  ASSERT_TRUE(accepted.ok());
  EXPECT_TRUE((*accepted)->isRemote());
  EXPECT_EQ((*accepted)->peerHost(), "127.0.0.1");
  EXPECT_EQ((*accepted)->peerAddress().rfind("127.0.0.1:", 0), 0u);

  ASSERT_TRUE((*client)->writeLine("hello").ok());
  std::string line;
  auto got = (*accepted)->readLine(line, 5000);
  ASSERT_TRUE(got.ok());
  EXPECT_TRUE(*got);
  EXPECT_EQ(line, "hello");
  auto timeout = (*accepted)->readLine(line, 50);
  ASSERT_FALSE(timeout.ok());
  EXPECT_EQ(timeout.error().code, ErrorCode::Timeout);
  (*client)->close();
  auto eof = (*accepted)->readLine(line, 5000);
  ASSERT_TRUE(eof.ok());
  EXPECT_FALSE(*eof);
}

TEST(TcpTransport, OverlongLineIsRejected) {
  auto listener = listenTcp(TcpEndpoint{"127.0.0.1", 0});
  ASSERT_TRUE(listener.ok());
  auto client = connectTo("tcp://" + (*listener)->address());
  ASSERT_TRUE(client.ok());
  auto accepted = (*listener)->accept();
  ASSERT_TRUE(accepted.ok());
  std::thread writer([&] { (void)(*client)->writeLine(std::string(kMaxLineBytes + 10, 'x')); });
  std::string line;
  auto got = (*accepted)->readLine(line, 10000);
  EXPECT_FALSE(got.ok());
  if (!got.ok()) EXPECT_EQ(got.error().code, ErrorCode::InvalidArgument);
  (*accepted)->close();
  (*client)->close();
  writer.join();
}

TEST(TcpTransport, ListenerCloseUnblocksAccept) {
  auto listener = listenTcp(TcpEndpoint{"127.0.0.1", 0});
  ASSERT_TRUE(listener.ok());
  std::thread closer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (*listener)->close();
  });
  auto accepted = (*listener)->accept();
  EXPECT_FALSE(accepted.ok());
  if (!accepted.ok()) EXPECT_EQ(accepted.error().code, ErrorCode::Cancelled);
  closer.join();
}

TEST(TcpTransport, SecondListenerOnTheSamePortIsBusy) {
  auto first = listenTcp(TcpEndpoint{"127.0.0.1", 0});
  ASSERT_TRUE(first.ok());
  auto parsed = parseTcpEndpoint((*first)->address());
  ASSERT_TRUE(parsed.ok());
  auto second = listenTcp(*parsed);
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.error().code, ErrorCode::Busy);
}

TEST(TcpTransport, ConnectionRefusedIsAnError) {
  auto listener = listenTcp(TcpEndpoint{"127.0.0.1", 0});
  ASSERT_TRUE(listener.ok());
  const std::string address = (*listener)->address();
  listener->reset();  // nothing listens any more
  auto client = connectTo("tcp://" + address);
  EXPECT_FALSE(client.ok());
}

TEST(TcpTransport, Ipv6LoopbackWorksWhereAvailable) {
  auto listener = listenTcp(TcpEndpoint{"::1", 0});
  if (!listener.ok()) GTEST_SKIP() << "no IPv6 loopback here: " << listener.error().message;
  EXPECT_EQ((*listener)->address().rfind("[::1]:", 0), 0u);
  auto client = connectTo("tcp://" + (*listener)->address());
  ASSERT_TRUE(client.ok()) << client.error().toString();
  auto accepted = (*listener)->accept();
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ((*accepted)->peerHost(), "::1");
}

// ---- handshake ----

TEST(TcpAuth, ValidTokenIsAdmittedAndServed) {
  TcpFixture f;
  auto c = Client::connect(f.tcpAddress, kToken);
  ASSERT_TRUE(c.ok()) << c.error().toString();
  auto r = c->call(StatusRequest{});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->ok);
  ASSERT_TRUE(waitUntil([&] { return f.handler.connects.load() == 1; }));
  std::lock_guard lock(f.handler.mutex);
  ASSERT_EQ(f.handler.infos.size(), 1u);
  EXPECT_TRUE(f.handler.infos[0].remote);
  EXPECT_EQ(f.handler.infos[0].peer.rfind("127.0.0.1:", 0), 0u);
}

TEST(TcpAuth, WrongTokenIsRefusedAndClosed) {
  LogCapture log;
  TcpFixture f;
  auto c = Client::connect(f.tcpAddress, std::string(kToken.size(), 'x'));
  ASSERT_FALSE(c.ok());
  EXPECT_EQ(c.error().code, ErrorCode::NoPrivilege);
  EXPECT_EQ(f.handler.connects.load(), 0) << "the handler never heard of the client";
  // The peer address is logged, the token never.
  const std::string logged = log.all();
  EXPECT_NE(logged.find("authentication failed from 127.0.0.1:"), std::string::npos) << logged;
  EXPECT_EQ(logged.find(kToken), std::string::npos);
  EXPECT_EQ(logged.find(std::string(kToken.size(), 'x')), std::string::npos);
}

TEST(TcpAuth, RawWrongTokenGetsAnErrorResponseThenEof) {
  TcpFixture f;
  auto raw = connectTo(f.tcpAddress);
  ASSERT_TRUE(raw.ok());
  ASSERT_TRUE((*raw)->writeLine("{\"cmd\":\"auth\",\"token\":\"nope\"}").ok());  // no id: allowed for auth
  std::string line;
  auto got = (*raw)->readLine(line, 5000);
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(*got);
  auto msg = decodeServerMessage(line);
  ASSERT_TRUE(msg.ok());
  const auto* resp = std::get_if<Response>(&*msg);
  ASSERT_NE(resp, nullptr);
  EXPECT_FALSE(resp->ok);
  EXPECT_EQ(resp->code, ErrorCode::NoPrivilege);
  EXPECT_FALSE(resp->id.has_value());
  EXPECT_EQ(line.find("nope"), std::string::npos) << "the response must not echo the token";
  auto eof = (*raw)->readLine(line, 5000);
  ASSERT_TRUE(eof.ok());
  EXPECT_FALSE(*eof);
}

TEST(TcpAuth, ARequestBeforeAuthIsRefused) {
  TcpFixture f;
  auto raw = connectTo(f.tcpAddress);
  ASSERT_TRUE(raw.ok());
  ASSERT_TRUE((*raw)->writeLine("{\"id\":7,\"cmd\":\"status\"}").ok());
  std::string line;
  auto got = (*raw)->readLine(line, 5000);
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(*got);
  auto msg = decodeServerMessage(line);
  ASSERT_TRUE(msg.ok());
  const auto* resp = std::get_if<Response>(&*msg);
  ASSERT_NE(resp, nullptr);
  EXPECT_FALSE(resp->ok);
  EXPECT_EQ(resp->code, ErrorCode::NoPrivilege);
  EXPECT_EQ(f.handler.handled.load(), 0);
}

TEST(TcpAuth, NoAuthLineWithinTheTimeoutDropsTheClient) {
  AuthPolicy policy;
  policy.handshakeTimeoutMs = 200;
  TcpFixture f(policy);
  auto raw = connectTo(f.tcpAddress);
  ASSERT_TRUE(raw.ok());
  std::string line;
  const auto start = std::chrono::steady_clock::now();
  auto got = (*raw)->readLine(line, 5000);  // the server's rejection arrives first
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(*got);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
  auto msg = decodeServerMessage(line);
  ASSERT_TRUE(msg.ok());
  const auto* resp = std::get_if<Response>(&*msg);
  ASSERT_NE(resp, nullptr);
  EXPECT_FALSE(resp->ok);
  auto eof = (*raw)->readLine(line, 5000);
  ASSERT_TRUE(eof.ok());
  EXPECT_FALSE(*eof);
}

#if !TACHYON_PLATFORM_WINDOWS
// The handshake timeout bounds the whole auth line: a peer dribbling bytes without a newline is still
// dropped.
TEST(TcpAuth, DribblingBytesDoesNotExtendTheHandshake) {
  AuthPolicy policy;
  policy.handshakeTimeoutMs = 300;
  TcpFixture f(policy);
  const std::string hostPort = f.tcpAddress.substr(f.tcpAddress.find("//") + 2);
  const int port = std::stoi(hostPort.substr(hostPort.rfind(':') + 1));
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
  const auto start = std::chrono::steady_clock::now();
  bool closed = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
    if (::send(fd, " ", 1, MSG_NOSIGNAL) != 1) {
      closed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::close(fd);
  EXPECT_TRUE(closed) << "a dribbling client kept the handshake open for 5 s";
}
#endif

TEST(TcpAuth, UnauthenticatedClientsReceiveNoEvents) {
  TcpFixture f;
  auto raw = connectTo(f.tcpAddress);
  ASSERT_TRUE(raw.ok());
  auto good = Client::connect(f.tcpAddress, kToken);
  ASSERT_TRUE(good.ok());
  f.server->broadcast(ErrorEvent{"hello"});
  auto event = good->nextEvent(5000);
  ASSERT_TRUE(event.ok());
  std::string line;
  auto got = (*raw)->readLine(line, 300);
  EXPECT_FALSE(got.ok()) << "an unauthenticated connection must not see events";
}

TEST(TcpAuth, LocalConnectionsNeedNoToken) {
  TcpFixture f;
  auto c = Client::connect(f.localAddress);
  ASSERT_TRUE(c.ok());
  auto r = c->call(StatusRequest{});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->ok);
  ASSERT_TRUE(waitUntil([&] { return f.handler.connects.load() == 1; }));
  std::lock_guard lock(f.handler.mutex);
  EXPECT_FALSE(f.handler.infos.at(0).remote);
  // An auth line on a local connection is harmless.
  auto again = c->call(AuthRequest{"whatever"});
  ASSERT_TRUE(again.ok());
  EXPECT_TRUE(again->ok);
}

TEST(TcpAuth, ClientNeedsATokenForTcp) {
  TcpFixture f;
  auto c = Client::connect(f.tcpAddress);
  ASSERT_FALSE(c.ok());
  EXPECT_EQ(c.error().code, ErrorCode::InvalidArgument);
}

TEST(TcpAuth, FailuresFromOneAddressAreRateLimited) {
  AuthPolicy policy;
  policy.maxFailures = 3;
  policy.failureWindowMs = 60000;
  TcpFixture f(policy);
  for (int i = 0; i < 3; ++i) {
    auto c = Client::connect(f.tcpAddress, "wrong-wrong-wrong-wrong-wrong-wrong");
    ASSERT_FALSE(c.ok());
    EXPECT_EQ(c.error().code, ErrorCode::NoPrivilege);
  }
  // Even the right token is refused now, with a message that says why.
  auto blocked = Client::connect(f.tcpAddress, kToken);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().code, ErrorCode::NoPrivilege);
  EXPECT_NE(blocked.error().message.find("too many"), std::string::npos) << blocked.error().message;
  EXPECT_EQ(f.handler.connects.load(), 0);
  // Local clients are unaffected.
  EXPECT_TRUE(Client::connect(f.localAddress).ok());
}

TEST(TcpAuth, TheLimitExpiresWithTheWindow) {
  AuthPolicy policy;
  policy.maxFailures = 2;
  policy.failureWindowMs = 300;
  TcpFixture f(policy);
  for (int i = 0; i < 2; ++i)
    EXPECT_FALSE(Client::connect(f.tcpAddress, "wrong-wrong-wrong-wrong-wrong-wrong").ok());
  EXPECT_FALSE(Client::connect(f.tcpAddress, kToken).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  EXPECT_TRUE(Client::connect(f.tcpAddress, kToken).ok());
}

TEST(TcpAuth, ASuccessfulLoginDoesNotCountAgainstTheLimit) {
  AuthPolicy policy;
  policy.maxFailures = 2;
  TcpFixture f(policy);
  for (int i = 0; i < 6; ++i) EXPECT_TRUE(Client::connect(f.tcpAddress, kToken).ok()) << i;
}

TEST(TcpAuth, TooManyUnauthenticatedConnectionsAreClosed) {
  AuthPolicy policy;
  policy.maxPending = 2;
  policy.handshakeTimeoutMs = 3000;
  TcpFixture f(policy);
  std::vector<std::unique_ptr<IConnection>> idle;
  for (int i = 0; i < 2; ++i) {
    auto raw = connectTo(f.tcpAddress);
    ASSERT_TRUE(raw.ok());
    idle.push_back(std::move(*raw));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto third = connectTo(f.tcpAddress);
  ASSERT_TRUE(third.ok());
  std::string line;
  auto got = (*third)->readLine(line, 2000);
  ASSERT_TRUE(got.ok());
  EXPECT_FALSE(*got) << "the third idle connection is closed without a word";
}

// ---- connect deadline split ----

TEST(ConnectDeadline, EveryAddressGetsAShareAndLeftoversCarryOver) {
  EXPECT_EQ(connectShareMs(5000, 1), 5000) << "the last (or only) address gets everything";
  EXPECT_EQ(connectShareMs(5000, 2), 2500) << "a hanging first address cannot take the whole deadline";
  EXPECT_EQ(connectShareMs(5000, 3), 1666);
  // Simulate three addresses of which the first two hang for their whole share: the third still gets time.
  int remaining = 5000;
  int shares[3];
  for (int i = 0; i < 3; ++i) {
    shares[i] = connectShareMs(remaining, static_cast<size_t>(3 - i));
    remaining -= shares[i];
  }
  EXPECT_GT(shares[2], 0);
  EXPECT_GE(shares[2], shares[0]) << "later addresses are never worse off than earlier ones";
  EXPECT_EQ(shares[0] + shares[1] + shares[2], 5000) << "the shares add up to the deadline";
  // A first address that fails at once hands its share to the next ones.
  EXPECT_EQ(connectShareMs(5000, 2), 2500);
  EXPECT_EQ(connectShareMs(4990, 1), 4990);
  EXPECT_EQ(connectShareMs(0, 3), 0);
  EXPECT_EQ(connectShareMs(-5, 3), 0);
  EXPECT_EQ(connectShareMs(1, 5), 1) << "never rounds a positive wait down to a non-wait";
}

// ---- token file ----

#if !TACHYON_PLATFORM_WINDOWS
void writeToken(const std::filesystem::path& path, const std::string& content, mode_t mode) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
  ::chmod(path.c_str(), mode);
}

TEST(TokenFile, AcceptsAGoodFile) {
  TempDir dir;
  const auto path = dir.path() / "token";
  writeToken(path, kToken + "\r\nsecond line ignored\n", 0600);
  auto token = loadTokenFile(path, kMinAgentTokenChars);
  ASSERT_TRUE(token.ok()) << token.error().toString();
  EXPECT_EQ(*token, kToken);
}

TEST(TokenFile, RefusesProblemFiles) {
  TempDir dir;
  const auto path = dir.path() / "token";
  EXPECT_EQ(loadTokenFile(path, 32).error().code, ErrorCode::NotFound);

  writeToken(path, "", 0600);
  EXPECT_EQ(loadTokenFile(path, 32).error().code, ErrorCode::InvalidArgument) << "empty";
  writeToken(path, "\n\n", 0600);
  EXPECT_EQ(loadTokenFile(path, 32).error().code, ErrorCode::InvalidArgument) << "blank";
  writeToken(path, "short-token", 0600);
  auto shortToken = loadTokenFile(path, 32);
  ASSERT_FALSE(shortToken.ok());
  EXPECT_EQ(shortToken.error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(shortToken.error().message.find("short-token"), std::string::npos) << "never echo the token";
  EXPECT_TRUE(loadTokenFile(path, 1).ok()) << "clients accept a short token";
  writeToken(path, "has a blank in the middle of a long enough token", 0600);
  EXPECT_EQ(loadTokenFile(path, 32).error().code, ErrorCode::InvalidArgument);

  for (const mode_t bad :
       {mode_t{0640}, mode_t{0604}, mode_t{0644}, mode_t{0660}, mode_t{0666}, mode_t{0620}}) {
    writeToken(path, kToken, bad);
    auto r = loadTokenFile(path, 32);
    ASSERT_FALSE(r.ok()) << std::oct << bad;
    EXPECT_EQ(r.error().code, ErrorCode::NoPrivilege) << std::oct << bad;
    EXPECT_EQ(r.error().message.find(kToken), std::string::npos);
  }
  writeToken(path, kToken, 0400);
  EXPECT_TRUE(loadTokenFile(path, 32).ok()) << "owner read-only is fine";
  std::filesystem::create_directory(dir.path() / "dir");
  ::chmod((dir.path() / "dir").c_str(), 0700);
  EXPECT_FALSE(loadTokenFile(dir.path() / "dir", 1).ok()) << "a directory is not a token file";
}

TEST(TokenFile, RefusesASymlinkEvenToAGoodFile) {
  TempDir dir;
  const auto real = dir.path() / "real-token";
  writeToken(real, kToken, 0600);
  const auto link = dir.path() / "token";
  std::filesystem::create_symlink(real, link);
  auto r = loadTokenFile(link, 32);
  ASSERT_FALSE(r.ok()) << "a symlink is never followed";
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  EXPECT_NE(r.error().message.find("symbolic link"), std::string::npos) << r.error().message;
  EXPECT_EQ(r.error().message.find(kToken), std::string::npos);
  EXPECT_TRUE(loadTokenFile(real, 32).ok()) << "the real file is fine";

  const auto dangling = dir.path() / "dangling";
  std::filesystem::create_symlink(dir.path() / "nowhere", dangling);
  EXPECT_FALSE(loadTokenFile(dangling, 32).ok());
}

TEST(TokenFile, RefusesAFileOwnedByAnotherUser) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root to give a file to another uid";
  TempDir dir;
  const auto path = dir.path() / "token";
  writeToken(path, kToken, 0600);
  ASSERT_EQ(::chown(path.c_str(), 54321, static_cast<gid_t>(-1)), 0) << std::strerror(errno);
  auto r = loadTokenFile(path, 32);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NoPrivilege);
  EXPECT_NE(r.error().message.find("54321"), std::string::npos) << r.error().message;
  EXPECT_EQ(r.error().message.find(kToken), std::string::npos);
  ASSERT_EQ(::chown(path.c_str(), 0, static_cast<gid_t>(-1)), 0);
  EXPECT_TRUE(loadTokenFile(path, 32).ok()) << "root-owned files are accepted";
}

TEST(TokenFile, RefusesGroupReadableMode0640) {
  TempDir dir;
  const auto path = dir.path() / "token";
  writeToken(path, kToken, 0640);
  auto r = loadTokenFile(path, 32);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NoPrivilege);
  EXPECT_NE(r.error().message.find("0640"), std::string::npos) << r.error().message;
  EXPECT_NE(r.error().message.find("chmod 600"), std::string::npos);
}
#endif

// ---- sha256 / base64 / constant time ----

TEST(Sha256, KnownVectors) {
  auto hex = [](const std::string& s) {
    Sha256 h;
    h.update(s.data(), s.size());
    const auto d = h.finish();
    return toHex(d.data(), d.size());
  };
  EXPECT_EQ(hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // One million 'a', fed in odd-sized pieces to exercise the block buffering.
  Sha256 h;
  const std::string piece(997, 'a');
  size_t left = 1'000'000;
  while (left > 0) {
    const size_t n = std::min(left, piece.size());
    h.update(piece.data(), n);
    left -= n;
  }
  const auto d = h.finish();
  EXPECT_EQ(toHex(d.data(), d.size()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, FileHashMatchesAndMissingFileFails) {
  TempDir dir;
  const auto path = dir.path() / "f.bin";
  std::ofstream(path, std::ios::binary) << "abc";
  auto h = sha256OfFile(path);
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(*h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_FALSE(sha256OfFile(dir.path() / "missing").ok());
}

TEST(Base64, RoundTripsAllLengthsAndRejectsGarbage) {
  std::string data;
  for (int len = 0; len < 40; ++len) {
    auto enc = base64Encode(data.data(), data.size());
    auto dec = base64Decode(enc);
    ASSERT_TRUE(dec.ok()) << len;
    EXPECT_EQ(*dec, data) << len;
    data.push_back(static_cast<char>(len * 37 + 11));
  }
  EXPECT_EQ(base64Encode("Man", 3), "TWFu");
  EXPECT_EQ(base64Encode("Ma", 2), "TWE=");
  EXPECT_EQ(base64Encode("M", 1), "TQ==");
  EXPECT_FALSE(base64Decode("TWF").ok());
  EXPECT_FALSE(base64Decode("TW!u").ok());
  EXPECT_FALSE(base64Decode("=WFu").ok());
  EXPECT_FALSE(base64Decode("TQ==TWFu").ok());
}

TEST(ConstantTime, ComparesLikeEquality) {
  EXPECT_TRUE(constantTimeEquals("abc", "abc"));
  EXPECT_TRUE(constantTimeEquals("", ""));
  EXPECT_FALSE(constantTimeEquals("abc", "abd"));
  EXPECT_FALSE(constantTimeEquals("abc", "abcd"));
  EXPECT_FALSE(constantTimeEquals("abcd", "abc"));
  EXPECT_FALSE(constantTimeEquals("", "a"));
  EXPECT_FALSE(constantTimeEquals(std::string("a\0b", 3), std::string("a\0c", 3)));
}

// ---- protocol additions ----

TEST(ProtocolRemote, AuthFetchAndProcessesRequestsRoundTrip) {
  Request auth;
  auth.id = 3;
  auth.body = AuthRequest{"tok"};
  auto decoded = decodeRequest(encodeRequest(auth));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(std::get<AuthRequest>(decoded->body).token, "tok");
  EXPECT_EQ(cmdName(cmdOf(decoded->body)), "auth");

  Request fetch;
  fetch.id = 4;
  FetchRequest fr;
  fr.path = "/tmp/x.tachyon";
  fr.offset = 123456789012ULL;
  fr.length = 4096;
  fr.want_sha256 = true;
  fetch.body = fr;
  decoded = decodeRequest(encodeRequest(fetch));
  ASSERT_TRUE(decoded.ok());
  const auto& got = std::get<FetchRequest>(decoded->body);
  EXPECT_EQ(got.path, fr.path);
  EXPECT_EQ(got.offset, fr.offset);
  EXPECT_EQ(got.length, fr.length);
  EXPECT_TRUE(got.want_sha256);

  Request procs;
  procs.id = 5;
  procs.body = ProcessesRequest{40};
  decoded = decodeRequest(encodeRequest(procs));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(std::get<ProcessesRequest>(decoded->body).offset, 40u);

  EXPECT_FALSE(decodeRequest("{\"id\":1,\"cmd\":\"auth\"}").ok()) << "token is required";
  EXPECT_FALSE(decodeRequest("{\"id\":1,\"cmd\":\"fetch\"}").ok()) << "path is required";
  EXPECT_FALSE(decodeRequest("{\"cmd\":\"status\"}").ok()) << "only auth may omit the id";
  EXPECT_TRUE(decodeRequest("{\"cmd\":\"auth\",\"token\":\"t\"}").ok());
}

TEST(ProtocolRemote, FetchAndProcessesResponsesRoundTrip) {
  FetchBody fb;
  fb.data = std::string("\0\x01\xff binary \n", 12);
  fb.offset = 10;
  fb.size = 22;
  fb.eof = true;
  fb.sha256 = std::string(64, 'a');
  const std::string line = encodeResponse(makeOkResponse(9, fb));
  EXPECT_EQ(line.find('\n'), std::string::npos);
  auto msg = decodeServerMessage(line, Cmd::Fetch);
  ASSERT_TRUE(msg.ok());
  const auto& resp = std::get<Response>(*msg);
  const auto* body = std::get_if<FetchBody>(&resp.body);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->data, fb.data);
  EXPECT_EQ(body->offset, 10u);
  EXPECT_EQ(body->size, 22u);
  EXPECT_TRUE(body->eof);
  EXPECT_EQ(body->sha256, fb.sha256);

  // The largest chunk stays under the line limit.
  FetchBody big;
  big.data.assign(kMaxFetchChunkBytes, '\xff');
  big.size = big.data.size();
  EXPECT_LT(encodeResponse(makeOkResponse(1, big)).size(), kMaxLineBytes);

  ProcessesBody pb;
  pb.offset = 2;
  pb.total = 9;
  pb.processes.push_back({42, "game", "dev", "game --level \"x\""});
  auto pmsg = decodeServerMessage(encodeResponse(makeOkResponse(1, pb)), Cmd::Processes);
  ASSERT_TRUE(pmsg.ok());
  const auto* pbody = std::get_if<ProcessesBody>(&std::get<Response>(*pmsg).body);
  ASSERT_NE(pbody, nullptr);
  EXPECT_EQ(pbody->total, 9u);
  ASSERT_EQ(pbody->processes.size(), 1u);
  EXPECT_EQ(pbody->processes[0].cmdline, "game --level \"x\"");
}

}  // namespace
}  // namespace tachyon::control
