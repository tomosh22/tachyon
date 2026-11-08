// WP-61: remote capture against an in-process Agent (mock backend) behind a real control Server with a TCP
// listener on loopback: restrictions for remote clients, `fetch` (resume, checksum, path confinement, large
// files with bounded memory), `processes`.
#include "../control/test_util.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "control/auth.h"
#include "control/client.h"
#include "control/fetch.h"
#include "control/server.h"
#include "control/sha256.h"
#include "control/tcp.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#if TACHYON_PLATFORM_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TACHYON_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define TACHYON_TEST_ASAN 1
#endif
#ifndef TACHYON_TEST_ASAN
#define TACHYON_TEST_ASAN 0
#endif

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using control::Client;
using control::testing_util::TempDir;
using namespace control;

const std::string kToken = "remote-test-token-0123456789-abcdefghij";

// Writes `bytes` of reproducible pseudo-random data without holding them in memory.
void writeBlob(const fs::path& path, uint64_t bytes, uint32_t seed = 1) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  std::string block(1 << 16, '\0');
  uint32_t x = seed * 2654435761u + 12345u;
  uint64_t left = bytes;
  while (left > 0) {
    for (char& c : block) {
      x = x * 1664525u + 1013904223u;
      c = static_cast<char>(x >> 24);
    }
    const size_t n = static_cast<size_t>(std::min<uint64_t>(left, block.size()));
    out.write(block.data(), static_cast<std::streamsize>(n));
    left -= n;
  }
}

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct RemoteHarness {
  TempDir dir;
  fs::path fixture;
  fs::path outDir;    // remoteOutDir of the agent
  fs::path localDir;  // where the client keeps fetched files
  std::string localAddress = testing_util::testAddress("remote");
  std::string tcpAddress;
  std::unique_ptr<Agent> agent;
  std::unique_ptr<Server> server;

  explicit RemoteHarness(uint64_t fixtureBytes = 1'500'000, bool allowLaunch = false,
                         AuthPolicy policy = {}) {
    fixture = dir.path() / "fixture.tachyon";
    writeBlob(fixture, fixtureBytes);
    outDir = dir.path() / "out";
    localDir = dir.path() / "local";
    fs::create_directories(outDir);
    fs::create_directories(localDir);
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture;
    AgentConfig ac;
    ac.backendName = "mock";
    ac.agentAddress = localAddress;
    ac.verifyInstPeerPid = false;
    ac.allowRemoteLaunch = allowLaunch;
    ac.remoteOutDir = outDir;
    agent = std::make_unique<Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto local = listenOn(localAddress);
    EXPECT_TRUE(local.ok());
    server = std::make_unique<Server>(std::move(*local), *agent);
    auto tcp = listenTcp(TcpEndpoint{"127.0.0.1", 0});
    EXPECT_TRUE(tcp.ok());
    tcpAddress = "tcp://" + (*tcp)->address();
    policy.token = kToken;
    server->addRemoteListener(std::move(*tcp), std::move(policy));
    agent->setEventSink([this](const Event& e) { server->broadcast(e); });
    server->start();
  }
  ~RemoteHarness() {
    server->stop();
    agent->shutdown();
  }

  Client remote() {
    auto c = Client::connectWithRetry(tcpAddress, 5000, kToken);
    EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().toString());
    return std::move(*c);
  }
  Client local() {
    auto c = Client::connectWithRetry(localAddress, 5000);
    EXPECT_TRUE(c.ok());
    return std::move(*c);
  }
};

bool isDone(const Event& e) {
  return std::holds_alternative<CaptureDoneEvent>(e);
}

StartRequest attachStart(const std::string& outPath = {}) {
  StartRequest s;
  s.mode = capture::CaptureMode::Attach;
  s.pid = 1;
  s.out_path = outPath;
  return s;
}

// Runs a whole capture through `client` and returns its summary.
CaptureSummary captureOnce(Client& client, const StartRequest& start) {
  auto started = client.call(start, 10000);
  EXPECT_TRUE(started.ok());
  EXPECT_TRUE(started->ok) << started->error;
  auto stop = client.call(StopRequest{}, 10000);
  EXPECT_TRUE(stop.ok());
  EXPECT_TRUE(stop->ok) << stop->error;
  auto done = client.waitForEvent(isDone, 60000);
  EXPECT_TRUE(done.ok());
  return std::get<CaptureDoneEvent>(*done).summary;
}

// ---- restrictions for remote clients ----

TEST(RemoteAgent, AttachAndSystemAreAllowedRemotely) {
  RemoteHarness h;
  auto c = h.remote();
  const CaptureSummary a = captureOnce(c, attachStart("a.tachyon"));
  EXPECT_FALSE(a.path.empty());
  StartRequest system;
  system.mode = capture::CaptureMode::System;
  const CaptureSummary s = captureOnce(c, system);
  EXPECT_FALSE(s.path.empty());
}

TEST(RemoteAgent, LaunchIsRefusedUnlessTheAgentAllowsIt) {
  {
    RemoteHarness h;
    auto c = h.remote();
    StartRequest launch;
    launch.mode = capture::CaptureMode::Launch;
    launch.exe = "/bin/true";
    auto r = c.call(launch, 10000);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r->ok);
    EXPECT_EQ(r->code, ErrorCode::NoPrivilege);
    EXPECT_NE(r->error.find("--allow-remote-launch"), std::string::npos) << r->error;
    // The refusal changed nothing: still idle, and a local client may launch.
    auto status = c.call(StatusRequest{}, 5000);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(std::get<StatusBody>(status->body).state, capture::CaptureState::Idle);
    auto local = h.local();
    launch.out_path = (h.dir.path() / "local-launch.tachyon").string();
    auto lr = local.call(launch, 10000);
    ASSERT_TRUE(lr.ok());
    EXPECT_TRUE(lr->ok) << lr->error;
    (void)local.call(CancelRequest{}, 10000);
  }
  {
    RemoteHarness h(1000, /*allowLaunch=*/true);
    auto c = h.remote();
    StartRequest launch;
    launch.mode = capture::CaptureMode::Launch;
    launch.exe = "/bin/true";
    auto r = c.call(launch, 10000);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r->ok) << r->error;
    (void)c.call(CancelRequest{}, 10000);
  }
}

TEST(RemoteAgent, RemoteClientsCannotChooseWhereTracesAreWritten) {
  RemoteHarness h;
  auto c = h.remote();
  const fs::path elsewhere = h.dir.path() / "elsewhere";
  fs::create_directories(elsewhere);
  const CaptureSummary s = captureOnce(c, attachStart((elsewhere / "evil.tachyon").string()));
  EXPECT_EQ(fs::path(s.path).parent_path(), h.outDir);
  EXPECT_TRUE(fs::exists(s.path));
  EXPECT_FALSE(fs::exists(elsewhere / "evil.tachyon"));
  EXPECT_EQ(fs::path(s.path).filename(), "evil.tachyon");
  // A second capture with the same name does not overwrite the first.
  const CaptureSummary again = captureOnce(c, attachStart((elsewhere / "evil.tachyon").string()));
  EXPECT_NE(again.path, s.path);
  // Traversal in the name cannot escape either.
  const CaptureSummary t = captureOnce(c, attachStart("../../../tmp/up.tachyon"));
  EXPECT_EQ(fs::path(t.path).parent_path(), h.outDir);
  // A local client still decides for itself.
  auto local = h.local();
  const fs::path mine = h.dir.path() / "mine.tachyon";
  EXPECT_EQ(fs::path(captureOnce(local, attachStart(mine.string())).path), mine);
}

TEST(RemoteAgent, InstHelloIsLocalOnly) {
  RemoteHarness h;
  auto c = h.remote();
  auto started = c.call(attachStart(), 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok);
  InstHelloRequest hello;
  hello.pid = 1;
  hello.api_version = kProtocolApiVersion;
  auto r = c.call(hello, 5000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->code, ErrorCode::NoPrivilege);
  (void)c.call(CancelRequest{}, 10000);
}

TEST(RemoteAgent, EventsReachRemoteAndLocalClients) {
  RemoteHarness h;
  auto remote = h.remote();
  auto local = h.local();
  auto started = local.call(attachStart((h.dir.path() / "events.tachyon").string()), 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok);
  auto stop = local.call(StopRequest{}, 10000);
  ASSERT_TRUE(stop.ok());
  EXPECT_TRUE(remote.waitForEvent(isDone, 30000).ok());
  EXPECT_TRUE(local.waitForEvent(isDone, 30000).ok());
}

// ---- fetch ----

TEST(RemoteFetch, TransfersAFileAndVerifiesIt) {
  RemoteHarness h(1'700'000);  // several chunks
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("cap.tachyon"));
  const fs::path dest = h.localDir / "fetched.tachyon";
  uint64_t lastDone = 0;
  FetchOptions options;
  options.progress = [&](uint64_t done, uint64_t total) {
    EXPECT_LE(done, total == 0 ? done : total);
    EXPECT_GE(done, lastDone);
    lastDone = done;
    return true;
  };
  auto r = fetchFile(c, s.path, dest, options);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->size, 1'700'000u);
  EXPECT_EQ(r->resumedFrom, 0u);
  EXPECT_FALSE(r->restarted);
  EXPECT_EQ(lastDone, 1'700'000u);
  EXPECT_EQ(slurp(dest), slurp(h.fixture));
  EXPECT_FALSE(fs::exists(dest.string() + ".part"));
  EXPECT_EQ(r->sha256, *sha256OfFile(h.fixture));
}

TEST(RemoteFetch, EmptyAndTinyFiles) {
  for (const uint64_t size : {uint64_t{0}, uint64_t{1}, kMaxFetchChunkBytes, kMaxFetchChunkBytes + 1}) {
    RemoteHarness h(size);
    auto c = h.remote();
    const CaptureSummary s = captureOnce(c, attachStart("edge.tachyon"));
    auto r = fetchFile(c, s.path, h.localDir / "edge.tachyon");
    ASSERT_TRUE(r.ok()) << size << ": " << r.error().toString();
    EXPECT_EQ(slurp(r->path), slurp(h.fixture)) << size;
  }
}

TEST(RemoteFetch, AnInterruptedTransferResumesFromThePartFile) {
  RemoteHarness h(3'000'000);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("resume.tachyon"));
  const fs::path dest = h.localDir / "resume.tachyon";

  int chunks = 0;
  FetchOptions cancelAfterTwo;
  cancelAfterTwo.progress = [&](uint64_t, uint64_t) { return ++chunks <= 2; };
  auto first = fetchFile(c, s.path, dest, cancelAfterTwo);
  ASSERT_FALSE(first.ok());
  EXPECT_EQ(first.error().code, ErrorCode::Cancelled);
  EXPECT_FALSE(fs::exists(dest));
  ASSERT_TRUE(fs::exists(dest.string() + ".part"));
  const uint64_t partial = fs::file_size(dest.string() + ".part");
  EXPECT_GT(partial, 0u);
  EXPECT_LT(partial, 3'000'000u);

  // A new connection, as after a network drop.
  auto c2 = h.remote();
  uint64_t firstDone = ~uint64_t{0};
  FetchOptions options;
  options.progress = [&](uint64_t done, uint64_t) {
    if (firstDone == ~uint64_t{0}) firstDone = done;
    return true;
  };
  auto second = fetchFile(c2, s.path, dest, options);
  ASSERT_TRUE(second.ok()) << second.error().toString();
  EXPECT_EQ(second->resumedFrom, partial);
  EXPECT_EQ(firstDone, partial) << "the transfer continued where it stopped";
  EXPECT_FALSE(second->restarted);
  EXPECT_EQ(slurp(dest), slurp(h.fixture));
}

TEST(RemoteFetch, ADroppedConnectionLeavesAResumablePartFile) {
  RemoteHarness h(2'200'000);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("drop.tachyon"));
  const fs::path dest = h.localDir / "drop.tachyon";
  int chunks = 0;
  FetchOptions options;
  options.callTimeoutMs = 2000;
  options.progress = [&](uint64_t, uint64_t) {
    if (++chunks == 3) c.close();  // the network drops in the middle of the transfer
    return true;
  };
  auto broken = fetchFile(c, s.path, dest, options);
  ASSERT_FALSE(broken.ok());
  ASSERT_TRUE(fs::exists(dest.string() + ".part"));
  auto c2 = h.remote();
  auto done = fetchFile(c2, s.path, dest);
  ASSERT_TRUE(done.ok()) << done.error().toString();
  EXPECT_GT(done->resumedFrom, 0u);
  EXPECT_EQ(slurp(dest), slurp(h.fixture));
}

TEST(RemoteFetch, ACorruptPartFileIsDetectedAndFetchedAgain) {
  RemoteHarness h(2'000'000);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("corrupt.tachyon"));
  const fs::path dest = h.localDir / "corrupt.tachyon";
  int chunks = 0;
  FetchOptions cancelEarly;
  cancelEarly.progress = [&](uint64_t, uint64_t) { return ++chunks <= 2; };
  ASSERT_FALSE(fetchFile(c, s.path, dest, cancelEarly).ok());
  const fs::path part = dest.string() + ".part";
  ASSERT_TRUE(fs::exists(part));
  {  // flip a byte in the middle of what was already received
    std::fstream f(part, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(1000);
    f.put('\x5a');
  }
  FetchOptions strict;
  strict.restartOnMismatch = false;
  auto mismatch = fetchFile(c, s.path, dest, strict);
  ASSERT_FALSE(mismatch.ok());
  EXPECT_EQ(mismatch.error().code, ErrorCode::CorruptData);
  EXPECT_NE(mismatch.error().message.find("SHA-256"), std::string::npos);
  EXPECT_FALSE(fs::exists(dest)) << "a file that failed its checksum never appears under its final name";
  EXPECT_FALSE(fs::exists(part)) << "and the bad partial file is discarded";

  // The same damage with the default policy: the transfer starts over by itself.
  chunks = 0;
  ASSERT_FALSE(fetchFile(c, s.path, dest, cancelEarly).ok());
  {
    std::fstream f(part, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(1000);
    f.put('\x5a');
  }
  auto healed = fetchFile(c, s.path, dest);
  ASSERT_TRUE(healed.ok()) << healed.error().toString();
  EXPECT_TRUE(healed->restarted);
  EXPECT_EQ(healed->resumedFrom, 0u);
  EXPECT_EQ(slurp(dest), slurp(h.fixture));
}

// An "agent" whose fetch answers carry the wrong checksum (bit rot on the wire, a buggy peer).
class LyingHandler final : public IRequestHandler {
 public:
  Response handle(ClientId, const Request& request) override {
    const auto* fetch = std::get_if<FetchRequest>(&request.body);
    if (fetch == nullptr) return makeOkResponse(std::nullopt);
    FetchBody body;
    body.offset = fetch->offset;
    body.size = 5;
    if (fetch->offset == 0) body.data = "hello";
    body.eof = true;
    body.sha256 = std::string(64, '0');
    ++answers;
    return makeOkResponse(std::nullopt, std::move(body));
  }
  std::atomic<int> answers{0};
};

TEST(RemoteFetch, AChecksumMismatchOfAFreshTransferIsAnError) {
  TempDir dir;
  LyingHandler handler;
  const std::string address = testing_util::testAddress("liar");
  auto listener = listenOn(address);
  ASSERT_TRUE(listener.ok());
  Server server(std::move(*listener), handler);
  server.start();
  auto c = Client::connectWithRetry(address, 5000);
  ASSERT_TRUE(c.ok());
  const fs::path dest = dir.path() / "liar.tachyon";
  auto r = fetchFile(*c, "/anything", dest);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::CorruptData);
  EXPECT_NE(r.error().message.find("SHA-256 mismatch"), std::string::npos) << r.error().message;
  EXPECT_FALSE(fs::exists(dest));
  EXPECT_FALSE(fs::exists(dest.string() + ".part"));
  EXPECT_EQ(handler.answers.load(), 1) << "a fresh transfer is not retried";
  server.stop();
}

TEST(RemoteFetch, AStalePartFileLongerThanTheRemoteFileStartsOver) {
  RemoteHarness h(100'000);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("stale.tachyon"));
  const fs::path dest = h.localDir / "stale.tachyon";
  writeBlob(dest.string() + ".part", 500'000, 99);
  auto r = fetchFile(c, s.path, dest);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_TRUE(r->restarted);
  EXPECT_EQ(slurp(dest), slurp(h.fixture));
}

TEST(RemoteFetch, OnlyFilesTheAgentProducedAreServed) {
  RemoteHarness h(50'000);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("good.tachyon"));
  const fs::path produced = s.path;

  auto fetchStatus = [&](const std::string& path) {
    FetchRequest req;
    req.path = path;
    req.length = 16;
    auto r = c.call(req, 5000);
    EXPECT_TRUE(r.ok());
    return std::move(*r);
  };
  // The real thing works, also spelled with "..".
  EXPECT_TRUE(fetchStatus(produced.string()).ok);
  EXPECT_TRUE(fetchStatus((produced.parent_path() / ".." / "out" / "good.tachyon").string()).ok);

  // A file that exists but was not produced by this agent.
  const fs::path other = h.dir.path() / "other.tachyon";
  writeBlob(other, 1000, 7);
  for (const std::string& bad :
       {other.string(), h.fixture.string(), std::string("/etc/passwd"), std::string("/etc/passwd/../passwd"),
        (produced.parent_path() / ".." / "fixture.tachyon").string(),
        (produced.parent_path() / ".." / ".." / "etc" / "passwd").string(),
        (h.outDir / "missing.tachyon").string(), std::string("good.tachyon"),
        std::string("../out/good.tachyon"), std::string("/proc/self/environ"), std::string(".")}) {
    const Response r = fetchStatus(bad);
    EXPECT_FALSE(r.ok) << bad;
    EXPECT_EQ(r.code, ErrorCode::NotFound) << bad;
  }
  // The refusal is the same for a file that does not exist and one that is not served: nothing to probe.
  EXPECT_EQ(fetchStatus(other.string()).error, fetchStatus((h.dir.path() / "nope").string()).error);
  FetchRequest nul;
  nul.path = std::string("a\0b", 3);
  auto r = c.call(nul, 5000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
}

TEST(RemoteFetch, SymlinksCannotLeadOutOfTheServedSet) {
  RemoteHarness h(50'000);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("real.tachyon"));
  const fs::path secret = h.dir.path() / "secret.txt";
  std::ofstream(secret) << "top secret";

  std::error_code ec;
  fs::create_symlink(secret, h.outDir / "link.tachyon", ec);
  if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();
  fs::create_symlink(h.dir.path(), h.outDir / "dirlink", ec);

  auto status = [&](const std::string& path) {
    FetchRequest req;
    req.path = path;
    auto r = c.call(req, 5000);
    EXPECT_TRUE(r.ok());
    return std::move(*r);
  };
  // A symlink inside the output directory that points at a file nobody produced.
  EXPECT_EQ(status((h.outDir / "link.tachyon").string()).code, ErrorCode::NotFound);
  // Through a symlinked directory.
  EXPECT_EQ(status((h.outDir / "dirlink" / "secret.txt").string()).code, ErrorCode::NotFound);
  EXPECT_EQ(status((h.outDir / "dirlink" / "fixture.tachyon").string()).code, ErrorCode::NotFound);
  // A symlink that points at a produced file is just another spelling of it.
  fs::create_symlink(s.path, h.localDir / "alias.tachyon", ec);
  EXPECT_TRUE(status((h.localDir / "alias.tachyon").string()).ok);

  // The produced file replaced by a symlink to something else afterwards is refused.
  fs::remove(s.path);
  fs::create_symlink(secret, s.path, ec);
  EXPECT_EQ(status(s.path).code, ErrorCode::NotFound);
}

TEST(RemoteFetch, FlushedTracesAreServedToo) {
  RemoteHarness h(40'000);
  auto c = h.remote();
  StartRequest start = attachStart("ring.tachyon");
  start.circular_mb = 8;
  auto started = c.call(start, 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;
  FlushRequest flush;
  flush.name = "hitch";
  flush.seconds = 5;
  auto flushed = c.call(flush, 10000);
  ASSERT_TRUE(flushed.ok());
  ASSERT_TRUE(flushed->ok) << flushed->error;
  const auto* summary = std::get_if<CaptureSummary>(&flushed->body);
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(fs::path(summary->path).parent_path(), h.outDir);
  auto r = fetchFile(c, summary->path, h.localDir / "hitch.tachyon");
  ASSERT_TRUE(r.ok()) << r.error().toString();
  (void)c.call(CancelRequest{}, 10000);
}

#if TACHYON_PLATFORM_LINUX
size_t residentBytes() {
  std::ifstream in("/proc/self/statm");
  size_t total = 0, resident = 0;
  in >> total >> resident;
  return resident * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}
#endif

TEST(RemoteFetch, LargeFilesStreamWithBoundedMemory) {
  constexpr uint64_t kSize = 72ull * 1024 * 1024;  // more than 64 MiB
  RemoteHarness h(kSize);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("big.tachyon"));

#if TACHYON_PLATFORM_LINUX
  const size_t before = residentBytes();
  std::atomic<size_t> peak{before};
  std::atomic<bool> running{true};
  std::thread sampler([&] {
    while (running.load()) {
      size_t now = residentBytes();
      size_t seen = peak.load();
      while (now > seen && !peak.compare_exchange_weak(seen, now)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
#endif
  auto r = fetchFile(c, s.path, h.localDir / "big.tachyon");
#if TACHYON_PLATFORM_LINUX
  running.store(false);
  sampler.join();
#endif
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->size, kSize);
  EXPECT_EQ(fs::file_size(h.localDir / "big.tachyon"), kSize);
  EXPECT_EQ(r->sha256, *sha256OfFile(h.fixture));
#if TACHYON_PLATFORM_LINUX
  // Client and agent share this process: a chunk is 512 KiB raw, ~700 KiB base64, a few copies in flight.
  // Streaming 72 MiB must not grow the process by anything close to that (ASan's quarantine inflates RSS, so
  // the bound is not checked there).
  const size_t growth = peak.load() > before ? peak.load() - before : 0;
  std::printf("[ measured ] resident growth while fetching %llu MiB: %.1f MiB\n",
              static_cast<unsigned long long>(kSize >> 20), static_cast<double>(growth) / (1024.0 * 1024.0));
  if (!TACHYON_TEST_ASAN) EXPECT_LT(growth, 24u * 1024 * 1024);
#endif
}

// ---- processes ----

TEST(RemoteProcesses, ListsTheAgentsProcesses) {
  RemoteHarness h;
  auto c = h.remote();
  auto list = fetchProcessList(c);
  ASSERT_TRUE(list.ok()) << list.error().toString();
  ASSERT_FALSE(list->empty());
#if TACHYON_PLATFORM_LINUX
  bool foundSelf = false;
  for (const ProcessEntry& p : *list) {
    EXPECT_NE(p.pid, 0u);
    EXPECT_LE(p.cmdline.size(), 256u);
    foundSelf |= p.pid == static_cast<uint32_t>(::getpid()) && !p.name.empty();
  }
  EXPECT_TRUE(foundSelf);
#endif
  for (size_t i = 1; i < list->size(); ++i) EXPECT_LT((*list)[i - 1].pid, (*list)[i].pid);
  // The first page alone says how many there are. Processes come and go between the two listings (other tests
  // run in parallel), so only check the page against its own total.
  ProcessesRequest first;
  auto page = c.call(first, 10000);
  ASSERT_TRUE(page.ok());
  const auto* body = std::get_if<ProcessesBody>(&page->body);
  ASSERT_NE(body, nullptr);
  EXPECT_GT(body->total, 0u);
  EXPECT_LE(body->processes.size(), body->total);
  ProcessesRequest past;
  past.offset = 100000000;
  auto empty = c.call(past, 10000);
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty->ok);
  EXPECT_TRUE(std::get<ProcessesBody>(empty->body).processes.empty());
}

// ---- WP-66: output files and fetch handles ----

#if TACHYON_PLATFORM_LINUX
TEST(RemoteOutput, ASymlinkPlantedAtThePlannedNameIsNeverWrittenThrough) {
  RemoteHarness h(30'000);
  const fs::path victimDir = h.dir.path() / "victim";
  fs::create_directories(victimDir);
  const fs::path existing = victimDir / "existing.txt";
  std::ofstream(existing) << "KEEP ME";
  const fs::path dangling = victimDir / "created-by-attack.txt";  // does not exist

  std::error_code ec;
  fs::create_symlink(dangling, h.outDir / "evil.tachyon", ec);
  if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();
  fs::create_symlink(existing, h.outDir / "evil-1.tachyon", ec);
  ASSERT_FALSE(ec);

  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("evil.tachyon"));
  EXPECT_EQ(fs::path(s.path), h.outDir / "evil-2.tachyon") << "both planted names count as taken";
  EXPECT_FALSE(fs::is_symlink(s.path));
  EXPECT_EQ(slurp(s.path), slurp(h.fixture));
  EXPECT_EQ(slurp(existing), "KEEP ME") << "the file behind the symlink is untouched";
  EXPECT_FALSE(fs::exists(dangling)) << "the dangling target was not created";
  EXPECT_TRUE(fs::is_symlink(h.outDir / "evil.tachyon"));
  EXPECT_TRUE(fs::is_symlink(h.outDir / "evil-1.tachyon"));
  // The produced file is still served.
  auto fetched = fetchFile(c, s.path, h.localDir / "evil.tachyon");
  ASSERT_TRUE(fetched.ok()) << fetched.error().toString();
}

TEST(RemoteOutput, ASymlinkPlantedAtAFlushNameIsNeverWrittenThrough) {
  RemoteHarness h(30'000);
  const fs::path existing = h.dir.path() / "existing-flush-victim.txt";
  std::ofstream(existing) << "KEEP ME";
  const fs::path dangling = h.dir.path() / "flush-victim-created.txt";
  std::error_code ec;
  fs::create_symlink(existing, h.outDir / "hitch.tachyon", ec);
  if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();
  fs::create_symlink(dangling, h.outDir / "hitch-1.tachyon", ec);

  auto c = h.remote();
  StartRequest start = attachStart("ring.tachyon");
  start.circular_mb = 8;
  auto started = c.call(start, 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;
  FlushRequest flush;
  flush.name = "hitch";
  flush.seconds = 5;
  auto flushed = c.call(flush, 10000);
  ASSERT_TRUE(flushed.ok());
  ASSERT_TRUE(flushed->ok) << flushed->error;
  const std::string path = std::get<CaptureSummary>(flushed->body).path;
  EXPECT_EQ(fs::path(path), h.outDir / "hitch-2.tachyon");
  EXPECT_FALSE(fs::is_symlink(path));
  EXPECT_EQ(slurp(path), slurp(h.fixture));
  EXPECT_EQ(slurp(existing), "KEEP ME");
  EXPECT_FALSE(fs::exists(dangling));
  (void)c.call(CancelRequest{}, 10000);
}

TEST(RemoteOutput, AFailedStartLeavesNoReservedFileBehind) {
  RemoteHarness h(30'000);
  auto c = h.remote();
  auto first = c.call(attachStart("busy.tachyon"), 10000);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(first->ok);
  // A second start is refused (a capture is running): the name it picked must not stay behind.
  auto second = c.call(attachStart("other.tachyon"), 10000);
  ASSERT_TRUE(second.ok());
  EXPECT_FALSE(second->ok);
  EXPECT_EQ(second->code, ErrorCode::Busy);
  EXPECT_FALSE(fs::exists(h.outDir / "other.tachyon"));
  (void)c.call(CancelRequest{}, 10000);
}
#endif

#if TACHYON_PLATFORM_LINUX
TEST(RemoteOutput, ASharedWritableDirectoryWarnsAndOnlyAForeignOwnerIsRefused) {
  TempDir dir;
  const fs::path d = dir.path() / "out";
  fs::create_directories(d);
  ::chmod(d.c_str(), 0700);
  EXPECT_TRUE(remoteOutDirWarning(d).empty());
  ::chmod(d.c_str(), 0755);
  EXPECT_TRUE(remoteOutDirWarning(d).empty()) << "readable by others is fine";
  ::chmod(d.c_str(), 0775);
  EXPECT_NE(remoteOutDirWarning(d).find("chmod go-w"), std::string::npos);
  ::chmod(d.c_str(), 0777);
  EXPECT_FALSE(remoteOutDirWarning(d).empty());
  ::chmod(d.c_str(), 01777);
  EXPECT_TRUE(remoteOutDirWarning(d).empty()) << "a sticky directory like /tmp cannot have files replaced";
  EXPECT_TRUE(remoteOutDirWarning(d / "missing").empty());
  // A shared directory is no longer a reason to refuse: the writer cannot be redirected through a link.
  for (const mode_t mode : {0700, 0775, 0777, 01777}) {
    ::chmod(d.c_str(), mode);
    EXPECT_TRUE(remoteOutDirProblem(d).empty()) << std::oct << mode;
  }
  EXPECT_TRUE(remoteOutDirProblem(d / "missing").empty());
  if (::geteuid() == 0) {
    ASSERT_EQ(::chown(d.c_str(), 12345, 12345), 0);
    ::chmod(d.c_str(), 0700);
    EXPECT_NE(remoteOutDirProblem(d).find("owned by uid 12345"), std::string::npos);
    ::chmod(d.c_str(), 01777);
    EXPECT_FALSE(remoteOutDirProblem(d).empty()) << "a foreign owner is refused even with the sticky bit";
  }
}

TEST(RemoteOutput, NoteProducedRecordsTheReservedFileAndNeverALinkToIt) {
  RemoteHarness h(1000);
  Agent& agent = *h.agent;
  const fs::path file = h.outDir / "real.tachyon";
  auto id = createExclusive(file);
  ASSERT_TRUE(id.ok());
  EXPECT_FALSE(agent.isProduced(file));
  agent.noteProduced(file, *id);
  EXPECT_TRUE(agent.isProduced(file));
  EXPECT_TRUE(agent.isProduced(h.outDir / "." / "real.tachyon")) << "same key however the path is spelled";

  // A symlink to a produced file is not itself recorded (the old code canonicalised it onto the target).
  const fs::path link = h.outDir / "link.tachyon";
  fs::create_symlink(file, link);
  agent.noteProduced(link);
  EXPECT_FALSE(agent.isProduced(link));

  // Reserved, then swapped for a symlink to a file that is produced anyway: refused.
  const fs::path swapped = h.outDir / "swapped.tachyon";
  auto swappedId = createExclusive(swapped);
  ASSERT_TRUE(swappedId.ok());
  fs::remove(swapped);
  fs::create_symlink(file, swapped);
  agent.noteProduced(swapped, *swappedId);
  EXPECT_FALSE(agent.isProduced(swapped));

  // Reserved, then replaced by another regular file: the identity differs, so it is not recorded.
  const fs::path replaced = h.outDir / "replaced.tachyon";
  auto replacedId = createExclusive(replaced);
  ASSERT_TRUE(replacedId.ok());
  writeBlob(h.outDir / "other.tmp", 10, 3);  // made first, so the freed inode is not reused
  fs::rename(h.outDir / "other.tmp", replaced);
  agent.noteProduced(replaced, *replacedId);
  EXPECT_FALSE(agent.isProduced(replaced));
  agent.noteProduced(replaced);  // without a reservation to compare (after an instrumentation merge)
  EXPECT_TRUE(agent.isProduced(replaced));
}

#endif

TEST(RemoteFetch, TheFileOpenedMustBeTheFileThatWasProduced) {
  RemoteHarness h(60'000);
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("swap.tachyon"));
  auto ask = [&](const std::string& path) {
    FetchRequest req;
    req.path = path;
    req.length = 16;
    auto r = c.call(req, 5000);
    EXPECT_TRUE(r.ok());
    return std::move(*r);
  };
  EXPECT_TRUE(ask(s.path).ok);
  // A local user replaces the produced file by another regular file of the same name (rename over it).
  const fs::path other = h.outDir / "swap.tmp";
  writeBlob(other, 60'000, 5);
  fs::rename(other, s.path);
  const Response r = ask(s.path);
  EXPECT_FALSE(r.ok) << "a different file is not served just because the path is the produced one";
  EXPECT_EQ(r.code, ErrorCode::NotFound);
}

TEST(RemoteFetch, TheWholeFileHashIsComputedOncePerFileVersion) {
  RemoteHarness h(1'900'000);  // four chunks
  auto c = h.remote();
  const CaptureSummary s = captureOnce(c, attachStart("hash.tachyon"));
  const std::string expected = *sha256OfFile(h.fixture);
  ASSERT_EQ(h.agent->hashComputations(), 0u);
  for (uint64_t offset = 0; offset < 1'900'000; offset += kMaxFetchChunkBytes) {
    FetchRequest req;
    req.path = s.path;
    req.offset = offset;
    req.want_sha256 = true;
    auto r = c.call(req, 10000);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r->ok) << r->error;
    EXPECT_EQ(std::get<FetchBody>(r->body).sha256, expected);
  }
  EXPECT_EQ(h.agent->hashComputations(), 1u) << "sha256:true on every chunk hashes the file once";
  // A full fetch (resumable client) afterwards still does not rehash.
  auto fetched = fetchFile(c, s.path, h.localDir / "hash.tachyon");
  ASSERT_TRUE(fetched.ok()) << fetched.error().toString();
  EXPECT_EQ(fetched->sha256, expected);
  EXPECT_EQ(h.agent->hashComputations(), 1u);
  // A changed file (new size or mtime) is hashed again, and the answer follows the content.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::ofstream out(s.path, std::ios::binary | std::ios::app);
    out << "more";
  }
  FetchRequest again;
  again.path = s.path;
  again.want_sha256 = true;
  auto changed = c.call(again, 10000);
  ASSERT_TRUE(changed.ok());
  ASSERT_TRUE(changed->ok) << changed->error;
  EXPECT_EQ(h.agent->hashComputations(), 2u);
  EXPECT_EQ(std::get<FetchBody>(changed->body).sha256, *sha256OfFile(s.path));
}

}  // namespace
}  // namespace tachyon::agent
