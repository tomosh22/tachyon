// WP-66: flight recorder hardening. The instrumentation ring's string prefix near its cap (no rotation after
// every frame, one warning), a segment file that cannot be created (the ring recovers and reports it), and a
// trigger that is coalesced into a manual flush.
#include "../control/test_util.h"
#include "../inst/raw_client.h"
#include "../inst/trace_builder.h"
#include "../inst/wire_builder.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "control/client.h"
#include "inst/decoder.h"
#include "inst/spool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using capture::CaptureState;
using control::Client;
using control::testing_util::TempDir;
using control::testing_util::testAddress;
using inst::testing_util::RawClient;
using inst::testing_util::StreamBuilder;
using namespace control;

constexpr uint32_t kTid = 7;

void feed(inst::Spool& spool, const StreamBuilder& b) {
  inst::FrameAssembler a;
  a.feed(b.bytes().data(), b.bytes().size(), [&](const uint8_t* f, size_t n) { spool.appendFrame(f, n); });
}

size_t countFiles(const fs::path& dir) {
  size_t n = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) n += entry.is_regular_file(ec) ? 1 : 0;
  return n;
}

// ---- (a) the string prefix cap ----

TEST(RingPrefix, APrefixNearTheCapDoesNotMakeSegmentsRotateAtEveryFrame) {
  TempDir dir;
  inst::SpoolOptions options;
  options.ringBytes = 48 << 20;
  options.segmentBytes = 2 << 20;  // smaller than the prefix below
  auto spool = inst::Spool::create(dir.path() / "inst-1.tachyon-inst", 0, nullptr, options);
  ASSERT_TRUE(spool.ok());

  StreamBuilder head;
  head.header();
  // Nine strings of 900 KiB: 8.1 MiB of StringDefs, just under the 8 MiB cap; the tenth no longer fits.
  const std::string big(900 * 1024, 'x');
  uint64_t key = 0;
  for (int i = 0; i < 10; ++i) {
    const uint64_t k = head.string(big + std::to_string(i));
    if (i == 0) key = k;  // a string that is in every prefix
  }
  head.threadName(1000, kTid, "ring-thread");
  feed(**spool, head);
  EXPECT_GE((*spool)->stats().prefixOverflows, 1u) << "the cap was hit and counted";
  const uint64_t startedByStrings = (*spool)->stats().segmentsStarted;  // the strings themselves rotate

  // A few thousand small frames: with a segment of 2 MiB and a prefix of 8 MiB the old rule rotated after
  // every one of them (and wrote 8 MiB each time). Now a segment holds at least half a segment of payload.
  uint64_t ts = 2000;
  for (int round = 0; round < 20; ++round) {
    StreamBuilder b;
    for (int i = 0; i < 200; ++i) {
      b.scopeBegin(ts, kTid, key, "payload-payload-payload");
      b.scopeEnd(ts + 5, kTid);
      ts += 10;
    }
    feed(**spool, b);
  }
  const inst::SpoolStats mid = (*spool)->stats();
  EXPECT_LE(mid.segmentsStarted - startedByStrings, 1u)
      << "about 0.4 MiB of payload fits within a segment's payload room (old rule: one rotation per frame)";

  // Now enough payload for a rotation or two: segments are about prefix + 1 MiB, not one per frame.
  for (int round = 0; round < 120; ++round) {
    StreamBuilder b;
    for (int i = 0; i < 200; ++i) {
      b.scopeBegin(ts, kTid, key, "payload-payload-payload");
      b.scopeEnd(ts + 5, kTid);
      ts += 10;
    }
    feed(**spool, b);
  }
  ASSERT_TRUE((*spool)->close().ok());
  const inst::SpoolStats end = (*spool)->stats();
  EXPECT_LE(end.segmentsStarted - startedByStrings, 5u) << "payload of about 2.6 MiB: a handful of segments";
  EXPECT_GE(end.segmentsStarted - startedByStrings, 1u);
  EXPECT_LE(countFiles(dir.path()), 12u);
  // Every retained segment still decodes on its own (the prefix survived).
  inst::InstDecoder decoder;
  for (const inst::SpoolSegment& seg : (*spool)->segments())
    ASSERT_TRUE(decoder.addFile(seg.file, seg.bytes).ok());
  const inst::DecodedInstrumentation data = decoder.finish();
  EXPECT_FALSE(data.scopes.empty());
  EXPECT_EQ(data.stats.malformedFrames, 0u);
  EXPECT_EQ(data.stats.unresolvedStrings, 0u);
}

// ---- (b) a segment file that cannot be created ----

TEST(RingSegments, AFailedSegmentOpenDoesNotLeaveTheRingInError) {
  TempDir dir;
  inst::SpoolOptions options;
  options.ringBytes = 2 << 20;
  options.segmentBytes = 64 << 10;
  const fs::path base = dir.path() / "inst-2.tachyon-inst";
  auto spool = inst::Spool::create(base, 0, nullptr, options);
  ASSERT_TRUE(spool.ok());
  // Make the second segment's name unusable: a directory is in the way (injected failure).
  const fs::path blocked = dir.path() / "inst-2.ring-000002.tachyon-inst";
  fs::create_directory(blocked);

  StreamBuilder head;
  head.header();
  const uint64_t key = head.string("ring_scope");
  head.threadName(1000, kTid, "ring-thread");
  feed(**spool, head);
  uint64_t ts = 2000;
  uint64_t pairs = 0;
  auto feedPairs = [&](int rounds) {
    for (int round = 0; round < rounds; ++round) {
      StreamBuilder b;
      for (int i = 0; i < 100; ++i) {
        b.scopeBegin(ts, kTid, key, "payload-payload-payload");
        b.scopeEnd(ts + 5, kTid);
        ts += 10;
        ++pairs;
      }
      feed(**spool, b);
    }
  };
  feedPairs(60);  // about 0.9 MiB: many segments' worth
  inst::SpoolStats st = (*spool)->stats();
  EXPECT_GE(st.segmentOpenFailures, 1u);
  EXPECT_FALSE(st.lastOpenError.empty());
  EXPECT_LE(st.segmentOpenFailures, 60u) << "retries are paced, not attempted at every frame";
  EXPECT_EQ(st.framesDropped, 0u) << "nothing is dropped: the current segment keeps growing";
  EXPECT_EQ(st.segmentsStarted, 1u);
  EXPECT_FALSE(st.overflowed);

  // The obstacle goes away: the next rotation succeeds and the ring works as usual again.
  fs::remove(blocked);
  feedPairs(60);
  st = (*spool)->stats();
  EXPECT_GE(st.segmentsStarted, 2u) << "rotation resumed";
  EXPECT_EQ(st.framesDropped, 0u);
  ASSERT_TRUE((*spool)->close().ok());

  // Everything that was fed is in the retained segments (the ring is large enough to keep all of it).
  inst::InstDecoder decoder;
  for (const inst::SpoolSegment& seg : (*spool)->segments())
    ASSERT_TRUE(decoder.addFile(seg.file, seg.bytes).ok());
  const inst::DecodedInstrumentation data = decoder.finish();
  EXPECT_EQ(data.scopes.size(), pairs * 2);
  EXPECT_EQ(data.stats.unresolvedStrings, 0u);
  EXPECT_EQ(data.stats.malformedFrames, 0u);
}

// ---- agent level: the warnings reach CaptureSummary ----

struct Harness {
  TempDir dir;
  fs::path fixture;
  std::string address = testAddress("hardening");
  std::unique_ptr<Agent> agent;
  std::unique_ptr<Server> server;

  explicit Harness(std::unique_ptr<capture::ICaptureBackend> backend, AgentConfig tweaks = {}) {
    AgentConfig ac = std::move(tweaks);
    ac.backendName = "mock";
    ac.agentAddress = address;
    ac.verifyInstPeerPid = false;
    ac.instDrainIdleMs = 100;
    ac.instDrainMaxMs = 2000;
    agent = std::make_unique<Agent>(std::move(backend), ac);
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), *agent);
    agent->setEventSink([this](const Event& e) { server->broadcast(e); });
    server->start();
  }
  ~Harness() {
    server->stop();
    agent->shutdown();
  }
  Client connect() {
    auto c = Client::connectWithRetry(address, 5000);
    EXPECT_TRUE(c.ok());
    return std::move(*c);
  }
  StartRequest startRequest(const std::string& out, uint32_t circularMb) const {
    StartRequest s;
    s.mode = capture::CaptureMode::System;
    s.out_path = (dir.path() / out).string();
    s.circular_mb = circularMb;
    return s;
  }
  std::string hello(Client& c, uint32_t pid) {
    auto r = c.call(InstHelloRequest{pid, kProtocolApiVersion}, 10000);
    EXPECT_TRUE(r.ok());
    if (!r.ok() || !r->ok) return {};
    const auto* body = std::get_if<InstHelloBody>(&r->body);
    return body != nullptr ? body->pipe : std::string();
  }
};

capture::MockBackendConfig mockConfig(const fs::path& fixture) {
  capture::MockBackendConfig cfg;
  cfg.fixture = fixture;
  return cfg;
}

bool hasWarning(const std::vector<std::string>& warnings, const std::string& needle) {
  return std::any_of(warnings.begin(), warnings.end(),
                     [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

TEST(FlightHardening, TheStringTableCapIsReportedAsAWarning) {
  TempDir fixtureDir;
  const fs::path fixture = fixtureDir.path() / "fixture.tachyon";
  inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
  AgentConfig cfg;
  cfg.minRingBytes = 48 << 20;
  cfg.ringSegmentBytes = 4 << 20;
  Harness h(std::make_unique<capture::MockBackend>(mockConfig(fixture)), cfg);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("cap.tachyon", 1), 10000)->ok);
  constexpr uint32_t kPid = 88;
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(h.hello(c, kPid)));
  StreamBuilder head;
  head.header(kPid, 1'000'000'000, 1);
  const std::string big(900 * 1024, 'y');
  uint64_t key = 0;
  for (int i = 0; i < 10; ++i) key = head.string(big + std::to_string(i));
  ASSERT_TRUE(sdk.send(head.bytes().data(), head.bytes().size()));
  StreamBuilder b;
  for (int i = 0; i < 500; ++i) {
    b.scopeBegin(10'000 + 10 * i, kPid, key, "x");
    b.scopeEnd(10'005 + 10 * i, kPid);
  }
  ASSERT_TRUE(sdk.send(b.bytes().data(), b.bytes().size()));
  sdk.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the agent reads what was sent

  FlushRequest flush;
  flush.name = "capped";
  flush.seconds = 10;
  auto flushed = c.call(flush, 30000);
  ASSERT_TRUE(flushed.ok());
  ASSERT_TRUE(flushed->ok) << flushed->error;
  const CaptureSummary summary = std::get<CaptureSummary>(flushed->body);
  EXPECT_TRUE(hasWarning(summary.warnings, "string table cap")) << "warnings: " << summary.warnings.size();
  const auto count =
      std::count_if(summary.warnings.begin(), summary.warnings.end(),
                    [](const std::string& w) { return w.find("string table cap") != std::string::npos; });
  EXPECT_EQ(count, 1) << "one warning per process";
  (void)c.call(CancelRequest{}, 10000);
}

TEST(FlightHardening, AFailedSegmentFileIsReportedAndTheCaptureCompletes) {
  TempDir fixtureDir;
  const fs::path fixture = fixtureDir.path() / "fixture.tachyon";
  inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
  AgentConfig cfg;
  cfg.minRingBytes = 1 << 20;
  Harness h(std::make_unique<capture::MockBackend>(mockConfig(fixture)), cfg);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("seg.tachyon", 1), 10000)->ok);
  constexpr uint32_t kPid = 89;
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(h.hello(c, kPid)));
  // The spool directory is <out>.inst; the second segment's name is blocked by a directory.
  const fs::path spoolDir = h.dir.path() / "seg.tachyon.inst";
  ASSERT_TRUE(fs::is_directory(spoolDir));
  fs::create_directory(spoolDir / "inst-89.ring-000002.tachyon-inst");

  StreamBuilder head;
  head.header(kPid, 1'000'000'000, 1);
  const uint64_t key = head.string("seg_scope");
  ASSERT_TRUE(sdk.send(head.bytes().data(), head.bytes().size()));
  uint64_t ts = 10'000;
  for (int round = 0; round < 10; ++round) {  // ~0.7 MiB: several 128 KiB segments' worth
    StreamBuilder b;
    for (int i = 0; i < 1500; ++i) {
      b.scopeBegin(ts, kPid, key, "payload-payload");
      b.scopeEnd(ts + 3, kPid);
      ts += 8;
    }
    ASSERT_TRUE(sdk.send(b.bytes().data(), b.bytes().size()));
  }
  sdk.close();
  auto stop = c.call(StopRequest{}, 10000);
  ASSERT_TRUE(stop.ok());
  EXPECT_TRUE(stop->ok) << stop->error;
  auto done =
      c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 30000);
  ASSERT_TRUE(done.ok());
  const CaptureSummary summary = std::get<CaptureDoneEvent>(*done).summary;
  EXPECT_TRUE(hasWarning(summary.warnings, "segment files could not be created")) << "no warning";
  EXPECT_FALSE(summary.path.empty());
  EXPECT_TRUE(fs::exists(summary.path));
}

// ---- (d) a trigger coalesced into a manual flush ----

// The mock backend with a gate in flush(): it stays inside until open() is called.
class GatedBackend final : public capture::ICaptureBackend {
 public:
  explicit GatedBackend(capture::MockBackendConfig cfg) : m_inner(std::move(cfg)) {}
  Result<void> checkPrivileges() override { return m_inner.checkPrivileges(); }
  Result<void> start(const capture::CaptureOptions& o) override { return m_inner.start(o); }
  Result<capture::CaptureResult> stop() override { return m_inner.stop(); }
  Result<capture::CaptureResult> flush(std::string name, uint32_t seconds) override {
    {
      std::unique_lock lock(m_mutex);
      m_entered = true;
      m_cv.notify_all();
      m_cv.wait(lock, [&] { return m_open; });
    }
    return m_inner.flush(std::move(name), seconds);
  }
  CaptureState state() const override { return m_inner.state(); }
  void setProgressCallback(std::function<void(CaptureState, float)> cb) override {
    m_inner.setProgressCallback(std::move(cb));
  }
  bool waitEntered(int ms) {
    std::unique_lock lock(m_mutex);
    return m_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return m_entered; });
  }
  void open() {
    std::lock_guard lock(m_mutex);
    m_open = true;
    m_cv.notify_all();
  }

 private:
  capture::MockBackend m_inner;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_entered = false;
  bool m_open = false;
};

TEST(FlightHardening, ATriggerCoalescedIntoAManualFlushAnswersWithThatFlushsPath) {
  TempDir fixtureDir;
  const fs::path fixture = fixtureDir.path() / "fixture.tachyon";
  inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
  auto backend = std::make_unique<GatedBackend>(mockConfig(fixture));
  GatedBackend* gate = backend.get();
  AgentConfig cfg;
  cfg.flushSettleMs = 0;
  cfg.flushCoalesceMs = 60'000;
  Harness h(std::move(backend), cfg);
  Client manual = h.connect();
  Client sdk = h.connect();
  ASSERT_TRUE(manual.call(h.startRequest("m.tachyon", 64), 10000)->ok);

  std::atomic<bool> manualOk{false};
  std::string manualPath;
  std::thread flusher([&] {
    FlushRequest f;
    f.name = "slow-manual";
    f.seconds = 10;
    auto r = manual.call(f, 30000);
    if (r.ok() && r->ok) {
      manualPath = std::get<CaptureSummary>(r->body).path;
      manualOk = true;
    }
  });
  ASSERT_TRUE(gate->waitEntered(10000)) << "the manual flush is running inside the backend";

  FlushRequest trigger;
  trigger.name = "hitch";
  trigger.seconds = 10;
  trigger.reason = "hitch";
  trigger.trigger = true;
  trigger.pid = 4242;
  auto r = sdk.call(trigger, 10000);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r->ok) << "a coalesced trigger is an accepted one: " << r->error;
  const std::string answered = std::get<CaptureSummary>(r->body).path;
  EXPECT_FALSE(answered.empty()) << "no empty path before any trigger flushed";
  EXPECT_EQ(answered, (h.dir.path() / "slow-manual.tachyon").string()) << "it points at the manual flush";

  gate->open();
  flusher.join();
  ASSERT_TRUE(manualOk.load());
  EXPECT_EQ(manualPath, answered) << "the same file the manual flush then produced";
  EXPECT_TRUE(fs::exists(manualPath));
  (void)manual.call(CancelRequest{}, 10000);
}

// ---- WP-66: an output file that was swapped while the backend wrote it is refused ----

TEST(FlightHardening, AFlushWhoseReservedFileWasReplacedIsRefusedAndNotServed) {
  TempDir fixtureDir;
  const fs::path fixture = fixtureDir.path() / "fixture.tachyon";
  inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
  auto backend = std::make_unique<GatedBackend>(mockConfig(fixture));
  GatedBackend* gate = backend.get();
  AgentConfig cfg;
  cfg.flushSettleMs = 0;
  Harness h(std::move(backend), cfg);
  Client c = h.connect();
  Client other = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("swap.tachyon", 64), 10000)->ok);

  std::atomic<bool> answered{false};
  Response response;
  std::thread flusher([&] {
    FlushRequest f;
    f.name = "swapped";
    f.seconds = 10;
    auto r = c.call(f, 30000);
    if (r.ok()) {
      response = std::move(*r);
      answered = true;
    }
  });
  ASSERT_TRUE(gate->waitEntered(10000));
  const fs::path reserved = h.dir.path() / "swapped.tachyon";
  ASSERT_TRUE(fs::exists(reserved)) << "the name was reserved before the backend started writing";
  EXPECT_EQ(fs::file_size(reserved), 0u);
  // Another local user replaces the reserved file by a different one (rename over it).
  const fs::path planted = h.dir.path() / "planted.tmp";
  std::ofstream(planted) << "planted";
  fs::rename(planted, reserved);
  gate->open();
  flusher.join();
  ASSERT_TRUE(answered.load());
  EXPECT_FALSE(response.ok) << "the flush is refused";
  EXPECT_NE(response.error.find("replaced"), std::string::npos) << response.error;
  FetchRequest fetch;
  fetch.path = reserved.string();
  auto served = other.call(fetch, 5000);
  ASSERT_TRUE(served.ok());
  EXPECT_FALSE(served->ok) << "and the file is never served";
  EXPECT_EQ(served->code, ErrorCode::NotFound);
  (void)c.call(CancelRequest{}, 10000);
}

}  // namespace
}  // namespace tachyon::agent
