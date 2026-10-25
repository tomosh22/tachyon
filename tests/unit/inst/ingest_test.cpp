#include "inst/ingest.h"

#include "../control/test_util.h"
#include "inst/decoder.h"
#include "inst/raw_stream_server.h"
#include "raw_client.h"
#include "wire_builder.h"

#include <gtest/gtest.h>

#include <cstring>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

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

// ---------------------------------------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------------------------------------

IngestConfig configIn(const TempDir& dir, uint64_t cap = kDefaultMaxSpoolBytes) {
  IngestConfig c;
  c.spoolDir = dir.path() / "spool";
  c.maxSpoolBytes = cap;
  c.drainIdleMs = 100;
  c.drainMaxMs = 3000;
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
