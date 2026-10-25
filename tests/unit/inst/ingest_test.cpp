#include "inst/ingest.h"

#include "../control/test_util.h"
#include "inst/decoder.h"
#include "inst/raw_stream_server.h"
#include "raw_client.h"
#include "wire_builder.h"

#include <gtest/gtest.h>

#include <cstring>

#include "core/platform.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

#if !TACHYON_PLATFORM_WINDOWS
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tachyon::inst {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using control::testing_util::testAddress;
using testing_util::RawClient;
using testing_util::StreamBuilder;

using Clock = std::chrono::steady_clock;

template <typename Pred>
bool waitUntil(Pred pred, int timeoutMs = 5000) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!pred()) {
    if (Clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// Collects what every connection sends.
struct Collector {
  std::mutex mutex;
  std::string bytes;
  std::atomic<int> opened{0}, closed{0};

  class Sink final : public IStreamSink {
   public:
    explicit Sink(Collector& c) : m_c(c) { ++m_c.opened; }
    void onData(const uint8_t* data, size_t size) override {
      std::lock_guard lock(m_c.mutex);
      m_c.bytes.append(reinterpret_cast<const char*>(data), size);
    }
    void onClose() override { ++m_c.closed; }

   private:
    Collector& m_c;
  };

  SinkFactory factory() {
    return [this]() -> std::unique_ptr<IStreamSink> { return std::make_unique<Sink>(*this); };
  }
  size_t size() {
    std::lock_guard lock(mutex);
    return bytes.size();
  }
};

TEST(RawStreamServer, ReceivesRawBytesIncludingNewlinesFromSeveralClients) {
  Collector c;
  const std::string address = testAddress("raw");
  auto server = RawStreamServer::listen(address, c.factory());
  ASSERT_TRUE(server.ok()) << server.error().toString();
  EXPECT_EQ((*server)->address(), address);
  std::string payload;
  for (int i = 0; i < 256; ++i) payload.push_back(static_cast<char>(i));  // \n, \r and \0 included
  {
    RawClient a, b;
    ASSERT_TRUE(a.connect(address));
    ASSERT_TRUE(b.connect(address));
    ASSERT_TRUE(a.send(payload.data(), payload.size()));
    ASSERT_TRUE(b.send(payload.data(), payload.size()));
  }
  ASSERT_TRUE(waitUntil([&] { return c.size() == 2 * payload.size() && c.closed == 2; }));
  EXPECT_EQ((*server)->acceptedCount(), 2u);
  // Each client's bytes arrive in order (the two may interleave only at chunk boundaries; one chunk here).
  std::lock_guard lock(c.mutex);
  EXPECT_EQ(c.bytes, payload + payload);
}

TEST(RawStreamServer, ShutdownEndsASilentConnectionAfterTheIdleTime) {
  Collector c;
  const std::string address = testAddress("idle");
  auto server = RawStreamServer::listen(address, c.factory());
  ASSERT_TRUE(server.ok());
  RawClient client;
  ASSERT_TRUE(client.connect(address));
  ASSERT_TRUE(client.send("hello", 5));
  const auto t0 = Clock::now();
  (*server)->shutdown(150, 5000);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  EXPECT_GE(elapsed, 100);
  EXPECT_LT(elapsed, 3000);
  EXPECT_EQ(c.size(), 5u);
  EXPECT_EQ(c.closed.load(), 1);
  EXPECT_EQ((*server)->openCount(), 0u);
}

TEST(RawStreamServer, ShutdownCapsAConnectionThatNeverGoesQuiet) {
  Collector c;
  const std::string address = testAddress("chatty");
  auto server = RawStreamServer::listen(address, c.factory());
  ASSERT_TRUE(server.ok());
  RawClient client;
  ASSERT_TRUE(client.connect(address));
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    while (!stop.load()) {
      if (!client.send("x", 1)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  ASSERT_TRUE(waitUntil([&] { return c.size() > 5; }));
  const auto t0 = Clock::now();
  (*server)->shutdown(500, 400);  // never idle for 500 ms, so the 400 ms cap ends it
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  stop = true;
  writer.join();
  EXPECT_GE(elapsed, 300);
  EXPECT_LT(elapsed, 3000);
  EXPECT_EQ(c.closed.load(), 1);
}

TEST(RawStreamServer, AClientThatConnectedJustBeforeShutdownIsStillServed) {
  Collector c;
  const std::string address = testAddress("late");
  auto server = RawStreamServer::listen(address, c.factory());
  ASSERT_TRUE(server.ok());
  RawClient client;
  ASSERT_TRUE(client.connect(address));
  ASSERT_TRUE(client.send("late data", 9));
  (*server)->shutdown(100, 2000);
  EXPECT_EQ(c.size(), 9u);
  EXPECT_EQ(c.closed.load(), 1);
}

TEST(RawStreamServer, ASecondListenerOnALiveAddressIsBusy) {
  Collector c;
  const std::string address = testAddress("busy");
  auto first = RawStreamServer::listen(address, c.factory());
  ASSERT_TRUE(first.ok());
  auto second = RawStreamServer::listen(address, c.factory());
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.error().code, ErrorCode::Busy);
}

TEST(RawStreamServer, AddressIsFreeAgainAfterShutdown) {
  Collector c;
  const std::string address = testAddress("again");
  {
    auto first = RawStreamServer::listen(address, c.factory());
    ASSERT_TRUE(first.ok());
    (*first)->shutdown(0, 0);
    (*first)->shutdown(0, 0);  // idempotent
  }
  auto second = RawStreamServer::listen(address, c.factory());
  EXPECT_TRUE(second.ok());
}

RawServerOptions limits(uint32_t maxConnections, uint32_t expectedPeer = 0) {
  RawServerOptions o;
  o.maxConnections = maxConnections;
  o.expectedPeerPid = expectedPeer;
  return o;
}

TEST(RawStreamServer, ConnectionsOverTheCapAreClosedAndCounted) {
  Collector c;
  const std::string address = testAddress("cap");
  auto server = RawStreamServer::listen(address, c.factory(), limits(2));
  ASSERT_TRUE(server.ok());
  std::vector<std::unique_ptr<RawClient>> clients;
  for (int i = 0; i < 5; ++i) {
    clients.push_back(std::make_unique<RawClient>());
    ASSERT_TRUE(clients.back()->connect(address));
    (void)clients.back()->send("x", 1);
    // Let the server decide about this one before the next arrives.
    ASSERT_TRUE(waitUntil([&] { return (*server)->acceptedCount() == static_cast<uint32_t>(i + 1); }));
  }
  EXPECT_EQ((*server)->rejectedCount(), 3u);
  EXPECT_EQ((*server)->openCount(), 2u);
  clients.clear();
  ASSERT_TRUE(waitUntil([&] { return (*server)->openCount() == 0; }));
  // Room again once the open ones are gone.
  RawClient again;
  ASSERT_TRUE(again.connect(address));
  ASSERT_TRUE(again.send("y", 1));
  ASSERT_TRUE(waitUntil([&] { return c.size() >= 3; }));
  EXPECT_EQ((*server)->rejectedCount(), 3u);
}

TEST(RawStreamServer, FinishedReaderThreadsAreReapedSoManyShortConnectionsStayCheap) {
  Collector c;
  const std::string address = testAddress("many");
  auto server = RawStreamServer::listen(address, c.factory(), limits(8));
  ASSERT_TRUE(server.ok());
  constexpr int kConnections = 400;
  for (int i = 0; i < kConnections; ++i) {
    RawClient client;
    ASSERT_TRUE(client.connect(address));
    ASSERT_TRUE(client.send("z", 1));
    client.close();
    ASSERT_TRUE(waitUntil([&] { return (*server)->acceptedCount() == static_cast<uint32_t>(i + 1); })) << i;
    ASSERT_TRUE(waitUntil([&] { return (*server)->openCount() == 0; })) << i;
  }
  ASSERT_TRUE(waitUntil([&] { return c.size() == static_cast<size_t>(kConnections); }));
  EXPECT_EQ((*server)->rejectedCount(), 0u);
#if TACHYON_PLATFORM_LINUX
  // Without reaping this would be ~400 finished-but-unjoined threads.
  std::ifstream status("/proc/self/status");
  std::string line;
  int threads = -1;
  while (std::getline(status, line))
    if (line.rfind("Threads:", 0) == 0) threads = std::stoi(line.substr(8));
  EXPECT_LT(threads, 40);
#endif
}

TEST(RawStreamServer, OnlyTheExpectedPeerProcessIsServed) {
  using control::testing_util::currentPid;
  Collector c;
  const std::string mine = testAddress("peer-ok"), other = testAddress("peer-bad");
  auto ok = RawStreamServer::listen(mine, c.factory(), limits(4, static_cast<uint32_t>(currentPid())));
  auto bad = RawStreamServer::listen(other, c.factory(), limits(4, static_cast<uint32_t>(currentPid()) + 1));
  ASSERT_TRUE(ok.ok());
  ASSERT_TRUE(bad.ok());
  RawClient a, b;
  ASSERT_TRUE(a.connect(mine));
  ASSERT_TRUE(a.send("ok", 2));
  ASSERT_TRUE(b.connect(other));
  (void)b.send("no", 2);
  ASSERT_TRUE(waitUntil([&] { return (*bad)->rejectedCount() == 1; }));
  ASSERT_TRUE(waitUntil([&] { return c.size() == 2; }));
  EXPECT_EQ(c.bytes, "ok");
  EXPECT_EQ((*ok)->rejectedCount(), 0u);
}

#if !TACHYON_PLATFORM_WINDOWS
TEST(RawStreamServer, TheSocketIsOwnerOnlyAndLeavesNoTemporaryDirectoryBehind) {
  TempDir dir;
  const std::string address = (dir.path() / "tachyon-inst-1.sock").string();
  Collector c;
  auto server = RawStreamServer::listen(address, c.factory());
  ASSERT_TRUE(server.ok()) << server.error().toString();
  struct stat st {};
  ASSERT_EQ(::stat(address.c_str(), &st), 0);
  EXPECT_TRUE(S_ISSOCK(st.st_mode));
  EXPECT_EQ(st.st_mode & 0777, 0600u);
  size_t entries = 0;
  for (const auto& e : fs::directory_iterator(dir.path())) {
    (void)e;
    ++entries;
  }
  EXPECT_EQ(entries, 1u);
  RawClient client;
  EXPECT_TRUE(client.connect(address));  // still reachable under its final name
}

// Runs `body` in a forked child and returns its exit code (-1 when it crashed), so resource limits and a
// crash stay out of the test process.
template <typename Body>
int inChild(Body body) {
  const pid_t child = ::fork();
  if (child == 0) ::_exit(body());
  int status = 0;
  ::waitpid(child, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool underSanitizer() {
#if defined(__SANITIZE_ADDRESS__)
  return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
  return true;
#else
  return false;
#endif
#else
  return false;
#endif
}

TEST(RawStreamServer, NoThreadsAvailableRejectsConnectionsInsteadOfTerminating) {
  if (underSanitizer()) GTEST_SKIP() << "address-space limits do not work with the sanitizer runtime";
  const std::string address = testAddress("nothreads");
  const int code = inChild([&]() -> int {
    Collector c;
    auto server = RawStreamServer::listen(address, c.factory(), limits(100));
    if (!server.ok()) return 10;
    // Leave room for about a thread or two: every further 8 MiB stack mapping fails.
    long pages = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
      if (std::fscanf(f, "%ld", &pages) != 1) pages = 0;
      std::fclose(f);
    }
    rlimit lim{};
    lim.rlim_cur = lim.rlim_max =
        static_cast<rlim_t>(pages) * static_cast<rlim_t>(::sysconf(_SC_PAGESIZE)) + (24ull << 20);
    if (::setrlimit(RLIMIT_AS, &lim) != 0) return 11;
    std::vector<std::unique_ptr<RawClient>> clients;
    for (int i = 0; i < 45; ++i) {
      clients.push_back(std::make_unique<RawClient>());
      if (!clients.back()->connect(address, 2000)) return 12;
      (void)clients.back()->send("x", 1);
    }
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while ((*server)->acceptedCount() < 45 && Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if ((*server)->acceptedCount() != 45) return 13;
    if ((*server)->rejectedCount() == 0) return 14;  // some had to be refused for lack of threads
    clients.clear();
    (*server)->shutdown(50, 500);
    return 0;
  });
  EXPECT_EQ(code, 0);
}

TEST(RawStreamServer, OutOfDescriptorsTheAcceptLoopBacksOffAndRecovers) {
  if (underSanitizer()) GTEST_SKIP() << "the sanitizer runtime needs descriptors of its own";
  const std::string address = testAddress("nofd");
  const int code = inChild([&]() -> int {
    Collector c;
    auto server = RawStreamServer::listen(address, c.factory(), limits(100));
    if (!server.ok()) return 10;
    // A handful of descriptors above what is open now.
    int open = 0;
    for (int fd = 0; fd < 1024; ++fd)
      if (::fcntl(fd, F_GETFD) != -1) ++open;
    rlimit lim{};
    lim.rlim_cur = lim.rlim_max = static_cast<rlim_t>(open + 8);
    if (::setrlimit(RLIMIT_NOFILE, &lim) != 0) return 11;
    {
      std::vector<std::unique_ptr<RawClient>> clients;
      for (int i = 0; i < 12; ++i) {
        auto client = std::make_unique<RawClient>();
        if (!client->connect(address, 500)) break;  // our own client descriptors run out too
        (void)client->send("x", 1);
        clients.push_back(std::move(client));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the accept loop meets EMFILE here
    }
    // Everything closed again: a new client must still be served.
    const size_t before = c.size();
    RawClient later;
    if (!later.connect(address, 2000)) return 12;
    if (!later.send("again", 5)) return 13;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (c.size() < before + 5 && Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return c.size() >= before + 5 ? 0 : 14;
  });
  EXPECT_EQ(code, 0);
}
#endif

// ---------------------------------------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------------------------------------

IngestConfig configIn(const TempDir& dir, uint64_t cap = kDefaultMaxSpoolBytes) {
  IngestConfig c;
  c.spoolDir = dir.path() / "spool";
  c.maxSpoolBytes = cap;
  c.drainIdleMs = 100;
  c.drainMaxMs = 3000;
  c.verifyPeerPid = false;  // the test clients live in this process, the pids are made up
  return c;
}

StreamBuilder processStream(uint32_t pid, uint64_t t0) {
  StreamBuilder b;
  b.header(pid, 1'000'000'000, t0);
  const uint64_t k = b.string("work");
  b.threadName(t0, pid, "main");
  for (uint64_t i = 0; i < 20; ++i) {
    b.scopeBegin(t0 + 10 + i * 10, pid, k, "n=" + std::to_string(i));
    b.scopeEnd(t0 + 15 + i * 10, pid);
  }
  return b;
}

TEST(Ingest, SpoolsEachProcessInItsOwnFile) {
  TempDir dir;
  Ingest ingest(configIn(dir));
  const std::string a1 = testAddress("ing-a"), a2 = testAddress("ing-b");
  ASSERT_TRUE(ingest.attach(100, a1).ok());
  ASSERT_TRUE(ingest.attach(200, a2).ok());
  EXPECT_EQ(ingest.processCount(), 2u);
  const StreamBuilder s1 = processStream(100, 1000), s2 = processStream(200, 5000);
  {
    RawClient c1, c2;
    ASSERT_TRUE(c1.connect(a1));
    ASSERT_TRUE(c2.connect(a2));
    ASSERT_TRUE(c1.sendChunked(s1.bytes(), 7));  // tiny writes: frames are reassembled
    ASSERT_TRUE(c2.send(s2.bytes().data(), s2.bytes().size()));
  }
  const std::vector<SpoolInfo> infos = ingest.finish();
  ASSERT_EQ(infos.size(), 2u);
  EXPECT_EQ(infos[0].pid, 100u);
  EXPECT_EQ(infos[1].pid, 200u);
  EXPECT_NE(infos[0].path, infos[1].path);
  EXPECT_EQ(infos[0].stats.framesWritten, s1.frames());
  EXPECT_EQ(infos[0].stats.bytesWritten, s1.bytes().size());
  EXPECT_EQ(infos[0].stats.truncatedTailBytes, 0u);
  EXPECT_EQ(infos[0].stats.connections, 1u);
  EXPECT_FALSE(infos[0].stats.overflowed);
  // The spool is the client's stream, byte for byte.
  EXPECT_EQ(fs::file_size(infos[0].path), s1.bytes().size());

  InstDecoder decoder;
  for (const SpoolInfo& info : infos) ASSERT_TRUE(decoder.addFile(info.path).ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.stats.scopeBegins, 40u);
  EXPECT_EQ(d.stats.scopeEnds, 40u);
  EXPECT_EQ(d.stats.unmatchedScopeEnds, 0u);
  EXPECT_EQ(d.stats.unclosedScopes, 0u);
  EXPECT_EQ(d.stats.streams, 2u);
  EXPECT_EQ(d.stats.threadNames, 2u);
}

TEST(Ingest, AnIncompleteLastFrameAtDisconnectIsNotSpooled) {
  TempDir dir;
  Ingest ingest(configIn(dir));
  const std::string address = testAddress("ing-trunc");
  ASSERT_TRUE(ingest.attach(7, address).ok());
  const StreamBuilder s = processStream(7, 100);
  {
    RawClient c;
    ASSERT_TRUE(c.connect(address));
    ASSERT_TRUE(c.send(s.bytes().data(), s.bytes().size() - 4));  // dies mid-frame
  }
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].stats.framesWritten, s.frames() - 1);
  EXPECT_EQ(infos[0].stats.truncatedTailBytes, sizeof(FrameHeader) + sizeof(ScopeEndPayload) - 4);
}

TEST(Ingest, ReconnectingAppendsANewStreamToTheSameSpool) {
  TempDir dir;
  Ingest ingest(configIn(dir));
  const std::string address = testAddress("ing-re");
  ASSERT_TRUE(ingest.attach(9, address).ok());
  ASSERT_TRUE(ingest.attach(9, address).ok());  // a second hello from the same process is fine
  EXPECT_EQ(ingest.processCount(), 1u);
  const StreamBuilder first = processStream(9, 100), second = processStream(9, 2000);
  {
    RawClient c;
    ASSERT_TRUE(c.connect(address));
    ASSERT_TRUE(c.send(first.bytes().data(), first.bytes().size()));
  }
  ASSERT_TRUE(waitUntil([&] {
    return ingest.snapshot().size() == 1 && ingest.snapshot()[0].stats.framesWritten == first.frames();
  }));
  {
    RawClient c;
    ASSERT_TRUE(c.connect(address));
    ASSERT_TRUE(c.send(second.bytes().data(), second.bytes().size()));
  }
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].stats.connections, 2u);
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(infos[0].path).ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.stats.streams, 2u);
  EXPECT_EQ(d.stats.scopeBegins, 40u);
}

TEST(Ingest, SpoolCapDropsTheRestAndReportsItAsADroppedMarker) {
  TempDir dir;
  const StreamBuilder s = processStream(5, 100);
  Ingest ingest(configIn(dir, s.bytes().size() / 2));
  const std::string address = testAddress("ing-cap");
  ASSERT_TRUE(ingest.attach(5, address).ok());
  {
    RawClient c;
    ASSERT_TRUE(c.connect(address));
    ASSERT_TRUE(c.send(s.bytes().data(), s.bytes().size()));
  }
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_TRUE(infos[0].stats.overflowed);
  EXPECT_GT(infos[0].stats.recordsDropped, 0u);
  EXPECT_LE(infos[0].stats.bytesWritten, s.bytes().size() / 2 + sizeof(FrameHeader) + sizeof(DroppedPayload));
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(infos[0].path).ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.stats.droppedRecords, infos[0].stats.recordsDropped);
  bool marker = false;
  for (const auto& rec : d.markers) {
    const trace::MarkerRec m = rec;
    if (d.strings.get(m.name) == kDroppedMarkerName) marker = true;
  }
  EXPECT_TRUE(marker);
  // What was kept plus what was dropped adds up to the 40 scope events and 1 thread name of the stream.
  EXPECT_EQ(d.stats.scopeBegins + d.stats.scopeEnds + d.stats.threadNames + d.stats.droppedRecords, 41u);
}

TEST(Ingest, AProcessThatNeverSendsLeavesNoSpool) {
  TempDir dir;
  Ingest ingest(configIn(dir));
  ASSERT_TRUE(ingest.attach(3, testAddress("ing-none")).ok());
  EXPECT_TRUE(ingest.finish().empty());
  size_t files = 0;
  for (const auto& e : fs::directory_iterator(ingest.spoolDir())) {
    (void)e;
    ++files;
  }
  EXPECT_EQ(files, 0u);
}

TEST(Ingest, SnapshotWhileStreamsAreLiveReturnsAFrameBoundary) {
  TempDir dir;
  Ingest ingest(configIn(dir));
  const std::string address = testAddress("ing-snap");
  ASSERT_TRUE(ingest.attach(11, address).ok());
  RawClient c;
  ASSERT_TRUE(c.connect(address));
  const StreamBuilder first = processStream(11, 100);
  ASSERT_TRUE(c.send(first.bytes().data(), first.bytes().size()));
  std::vector<SpoolInfo> snap;
  ASSERT_TRUE(waitUntil([&] {
    snap = ingest.snapshot();
    return snap.size() == 1 && snap[0].flushedBytes == first.bytes().size();
  }));
  // The live connection keeps spooling after the snapshot; decoding the snapshot prefix sees only the first
  // part.
  StreamBuilder more;
  more.scopeEnd(9999, 11);
  ASSERT_TRUE(c.send(more.bytes().data(), more.bytes().size()));
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(snap[0].path, snap[0].flushedBytes).ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.stats.scopeEnds, 20u);
  EXPECT_EQ(d.stats.truncatedTailBytes, 0u);
  c.close();
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].stats.framesWritten, first.frames() + 1);
}

TEST(Ingest, CorruptStreamIsCutOffAndCounted) {
  TempDir dir;
  Ingest ingest(configIn(dir));
  const std::string address = testAddress("ing-bad");
  ASSERT_TRUE(ingest.attach(13, address).ok());
  StreamBuilder s = processStream(13, 100);
  std::vector<uint8_t> bytes = s.bytes();
  const size_t good = bytes.size();
  FrameHeader bad{3, 0x7FFFFFF0};
  bytes.resize(good + sizeof(bad) + 64);
  std::memcpy(bytes.data() + good, &bad, sizeof(bad));
  {
    RawClient c;
    ASSERT_TRUE(c.connect(address));
    ASSERT_TRUE(c.send(bytes.data(), bytes.size()));
  }
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].stats.framesWritten, s.frames());
  EXPECT_GT(infos[0].stats.corruptBytes, 0u);
}

TEST(Ingest, ProcessCapGivesBusyForTheOneBeyondIt) {
  TempDir dir;
  IngestConfig cfg = configIn(dir);
  cfg.maxProcesses = 2;
  Ingest ingest(cfg);
  ASSERT_TRUE(ingest.attach(1, testAddress("pc1")).ok());
  ASSERT_TRUE(ingest.attach(2, testAddress("pc2")).ok());
  ASSERT_TRUE(ingest.attach(2, testAddress("pc2")).ok());  // the same pid again is not a new process
  const auto third = ingest.attach(3, testAddress("pc3"));
  ASSERT_FALSE(third.ok());
  EXPECT_EQ(third.error().code, ErrorCode::Busy);
  EXPECT_EQ(ingest.processCount(), 2u);
}

TEST(Ingest, ConnectionsPerProcessAreCappedAndCounted) {
  TempDir dir;
  IngestConfig cfg = configIn(dir);
  cfg.maxConnectionsPerProcess = 1;
  Ingest ingest(cfg);
  const std::string address = testAddress("cpp");
  ASSERT_TRUE(ingest.attach(4, address).ok());
  RawClient first, second;
  ASSERT_TRUE(first.connect(address));
  const StreamBuilder s = processStream(4, 100);
  ASSERT_TRUE(first.send(s.bytes().data(), s.bytes().size()));
  ASSERT_TRUE(waitUntil([&] { return !ingest.snapshot().empty(); }));
  ASSERT_TRUE(second.connect(address));
  (void)second.send(s.bytes().data(), s.bytes().size());
  ASSERT_TRUE(waitUntil([&] { return ingest.rejectedConnections() == 1; }));
  first.close();
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].stats.framesWritten, s.frames());  // only the first connection was spooled
  EXPECT_EQ(infos[0].stats.connections, 1u);
}

TEST(Ingest, TotalSpoolBudgetIsSharedByAllProcesses) {
  TempDir dir;
  const StreamBuilder s1 = processStream(1, 100), s2 = processStream(2, 100);
  IngestConfig cfg = configIn(dir);  // per process: 1 GiB
  cfg.maxTotalSpoolBytes = s1.bytes().size() + s2.bytes().size() / 2;
  Ingest ingest(cfg);
  const std::string a1 = testAddress("tb1"), a2 = testAddress("tb2");
  ASSERT_TRUE(ingest.attach(1, a1).ok());
  ASSERT_TRUE(ingest.attach(2, a2).ok());
  {
    RawClient c1;
    ASSERT_TRUE(c1.connect(a1));
    ASSERT_TRUE(c1.send(s1.bytes().data(), s1.bytes().size()));
    ASSERT_TRUE(waitUntil([&] {
      const auto snap = ingest.snapshot();
      return snap.size() == 1 && snap[0].stats.framesWritten == s1.frames();
    }));
    RawClient c2;
    ASSERT_TRUE(c2.connect(a2));
    ASSERT_TRUE(c2.send(s2.bytes().data(), s2.bytes().size()));
  }
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 2u);
  EXPECT_FALSE(infos[0].stats.overflowed);
  EXPECT_TRUE(infos[1].stats.overflowed);
  EXPECT_GT(infos[1].stats.recordsDropped, 0u);
  EXPECT_LE(infos[0].stats.bytesWritten + infos[1].stats.bytesWritten,
            cfg.maxTotalSpoolBytes + sizeof(FrameHeader) + sizeof(DroppedPayload));
}

TEST(Ingest, PeerPidVerificationTiesTheConnectionToTheHelloPid) {
  using control::testing_util::currentPid;
  TempDir dir;
  IngestConfig cfg = configIn(dir);
  cfg.verifyPeerPid = true;
  Ingest ingest(cfg);
  const uint32_t me = static_cast<uint32_t>(currentPid());
  const std::string good = testAddress("pv-good"), bad = testAddress("pv-bad");
  ASSERT_TRUE(ingest.attach(me, good).ok());
  ASSERT_TRUE(ingest.attach(me + 1, bad).ok());  // a hello that lied about its pid
  {
    RawClient c1, c2;
    ASSERT_TRUE(c1.connect(good));
    ASSERT_TRUE(c1.send("", 0));
    const StreamBuilder s = processStream(me, 100);
    ASSERT_TRUE(c1.send(s.bytes().data(), s.bytes().size()));
    ASSERT_TRUE(c2.connect(bad));
    (void)c2.send(s.bytes().data(), s.bytes().size());
    ASSERT_TRUE(waitUntil([&] { return ingest.rejectedConnections() == 1; }));
  }
  const auto infos = ingest.finish();
  ASSERT_EQ(infos.size(), 1u);  // the liar's spool never received a byte and was removed
  EXPECT_EQ(infos[0].pid, me);
}

TEST(Ingest, AttachAfterFinishFailsAndDiscardRemovesTheDirectory) {
  TempDir dir;
  Ingest ingest(configIn(dir));
  const std::string address = testAddress("ing-fin");
  ASSERT_TRUE(ingest.attach(1, address).ok());
  {
    RawClient c;
    ASSERT_TRUE(c.connect(address));
    const StreamBuilder s = processStream(1, 10);
    ASSERT_TRUE(c.send(s.bytes().data(), s.bytes().size()));
  }
  ASSERT_EQ(ingest.finish().size(), 1u);
  EXPECT_EQ(ingest.finish().size(), 1u);  // idempotent
  EXPECT_FALSE(ingest.attach(2, testAddress("ing-fin2")).ok());
  EXPECT_TRUE(fs::exists(ingest.spoolDir()));
  ingest.discard();
  EXPECT_FALSE(fs::exists(ingest.spoolDir()));
}

TEST(Ingest, TheListenerIsGoneAfterFinish) {
  TempDir dir;
  const std::string address = testAddress("ing-gone");
  {
    Ingest ingest(configIn(dir));
    ASSERT_TRUE(ingest.attach(1, address).ok());
    (void)ingest.finish();
  }
  RawClient c;
  EXPECT_FALSE(c.connect(address, 100));
}

}  // namespace
}  // namespace tachyon::inst
