// WP-60: the flight recorder in the agent (Agent + MockBackend + real control server + raw SDK-style
// clients): flushed events, file names, SDK triggers, coalescing, and many flushes of a running circular
// capture without growth of file descriptors or disk.
#include "../control/test_util.h"
#include "../inst/raw_client.h"
#include "../inst/trace_builder.h"
#include "../inst/wire_builder.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "control/client.h"
#include "inst/merge.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <regex>
#include <set>
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

constexpr uint64_t kTraceStart = 1000;
constexpr uint64_t kTraceEnd = 2'000'000;  // the window of every (mock) flushed trace

struct FlightHarness {
  TempDir dir;
  fs::path fixture;
  std::string address = testAddress("flight");
  std::unique_ptr<Agent> agent;
  std::unique_ptr<Server> server;

  explicit FlightHarness(AgentConfig tweaks = {}) {
    fixture = dir.path() / "fixture.tachyon";
    inst::testing_util::writeBaseTrace(fixture, kTraceStart, kTraceEnd);
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture;
    AgentConfig ac = std::move(tweaks);
    ac.backendName = "mock";
    ac.agentAddress = address;
    ac.verifyInstPeerPid = false;
    ac.instDrainIdleMs = 100;
    ac.instDrainMaxMs = 2000;
    agent = std::make_unique<Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), *agent);
    agent->setEventSink([this](const Event& e) { server->broadcast(e); });
    server->start();
  }
  ~FlightHarness() {
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
  void stopAndWait(Client& c) {
    auto stop = c.call(StopRequest{}, 10000);
    ASSERT_TRUE(stop.ok());
    EXPECT_TRUE(stop->ok) << stop->error;
    auto done =
        c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 30000);
    EXPECT_TRUE(done.ok());
  }
};

Result<FlushedEvent> nextFlushed(Client& c, int timeoutMs = 15000) {
  auto e =
      c.waitForEvent([](const Event& ev) { return std::holds_alternative<FlushedEvent>(ev); }, timeoutMs);
  if (!e.ok()) return e.error();
  return std::get<FlushedEvent>(*e);
}

std::string captureInfoOf(const std::string& path) {
  auto reader = trace::TraceReader::open(path);
  EXPECT_TRUE(reader.ok());
  if (!reader.ok()) return {};
  return reader->readCaptureInfo().value();
}

FlushRequest flushRequest(const std::string& name, uint32_t seconds = 10) {
  FlushRequest f;
  f.name = name;
  f.seconds = seconds;
  return f;
}

FlushRequest triggerRequest(const std::string& reason, uint32_t pid = 4242) {
  FlushRequest f;
  f.name = reason;
  f.seconds = 10;  // what the SDK sends; the agent uses its own window
  f.reason = reason;
  f.trigger = true;
  f.pid = pid;
  return f;
}

TEST(FlightRecorder, AFlushEmitsFlushedToEveryClientAndTagsTheTrace) {
  FlightHarness h;
  Client a = h.connect();
  Client observer = h.connect();
  ASSERT_TRUE(a.call(h.startRequest("fr.tachyon", 64), 10000)->ok);

  FlushRequest f = flushRequest("hitch-12345", 7);
  f.reason = "console";
  auto r = a.call(f, 10000);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r->ok) << r->error;
  const auto* summary = std::get_if<CaptureSummary>(&r->body);
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(fs::path(summary->path), h.dir.path() / "hitch-12345.tachyon");

  for (Client* c : {&a, &observer}) {
    auto flushed = nextFlushed(*c);
    ASSERT_TRUE(flushed.ok());
    EXPECT_EQ(flushed->path, summary->path);
    EXPECT_EQ(flushed->name, "hitch-12345");
    EXPECT_EQ(flushed->reason, "console");
    EXPECT_EQ(flushed->coalesced, 0u);
  }
  const std::string info = captureInfoOf(summary->path);
  EXPECT_NE(info.find("\"flush_reason\":\"console\""), std::string::npos) << info;
  EXPECT_NE(info.find("\"flush_name\":\"hitch-12345\""), std::string::npos) << info;
  EXPECT_NE(info.find("\"flush_window_s\":7"), std::string::npos) << info;
  EXPECT_NE(info.find("\"flush_trigger\":false"), std::string::npos) << info;
  EXPECT_EQ(h.agent->state(), CaptureState::Capturing);
}

TEST(FlightRecorder, NamesAreSanitizedAndCollisionsGetASuffix) {
  FlightHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("names.tachyon", 64), 10000)->ok);
  auto flush = [&](const std::string& name) {
    auto r = c.call(flushRequest(name), 10000);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r->ok) << r->error;
    const auto* s = std::get_if<CaptureSummary>(&r->body);
    return s != nullptr ? fs::path(s->path).filename().string() : std::string();
  };
  EXPECT_EQ(flush("plain"), "plain.tachyon");
  EXPECT_EQ(flush("plain"), "plain-1.tachyon");
  EXPECT_EQ(flush("plain"), "plain-2.tachyon");
  EXPECT_EQ(flush("a b/c\\d:e*f"), "a_b_c_d_e_f.tachyon");
  EXPECT_EQ(flush("../escape"), "_escape.tachyon");
  EXPECT_EQ(flush(".."), "flush.tachyon");
  EXPECT_EQ(flush(""), "flush-1.tachyon");
  EXPECT_EQ(flush(std::string(300, 'x')), std::string(120, 'x') + ".tachyon");
  EXPECT_EQ(flush("h\xC3\xA9llo"), "h__llo.tachyon");
  EXPECT_EQ(flush("CON"), "_CON.tachyon");  // Windows device names
  EXPECT_EQ(flush("nul.x"), "_nul.x.tachyon");
  EXPECT_EQ(flush("com7"), "_com7.tachyon");
  EXPECT_EQ(flush("COM0"), "COM0.tachyon");
  EXPECT_EQ(flush("console"), "console.tachyon");
  for (const auto& entry : fs::directory_iterator(h.dir.path()))
    EXPECT_EQ(entry.path().parent_path(), h.dir.path()) << "nothing escaped the output directory";
}

TEST(FlightRecorder, FlushNeedsACircularCapture) {
  FlightHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("plain.tachyon", 0), 10000)->ok);
  for (const FlushRequest& f : {flushRequest("nope"), triggerRequest("hitch")}) {
    auto r = c.call(f, 10000);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r->ok);
    EXPECT_EQ(r->code, ErrorCode::Internal);  // EINTERNAL on the wire
  }
  std::error_code ec;
  size_t traces = 0;
  for (const auto& entry : fs::directory_iterator(h.dir.path(), ec))
    traces += entry.path().extension() == ".tachyon" ? 1 : 0;
  EXPECT_EQ(traces, 1u) << "only the fixture";
  h.stopAndWait(c);
}

TEST(FlightRecorder, ATriggerAnswersAtOnceAndFlushesInTheBackground) {
  AgentConfig cfg;
  cfg.flushSeconds = 6;
  cfg.flushSettleMs = 0;
  FlightHarness h(cfg);
  Client c = h.connect();
  StartRequest start = h.startRequest("trig.tachyon", 64);
  start.mode = capture::CaptureMode::Launch;
  start.exe = "/usr/games/My Game.exe";  // the mock backend does not launch anything
  ASSERT_TRUE(c.call(start, 10000)->ok);

  auto r = c.call(triggerRequest("hitch"), 10000);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r->ok) << r->error;
  const auto* planned = std::get_if<CaptureSummary>(&r->body);
  ASSERT_NE(planned, nullptr);
  // <process>-<reason>-<YYYYMMDD-HHMMSS>
  const std::string file = fs::path(planned->path).filename().string();
  EXPECT_TRUE(std::regex_match(file, std::regex("My_Game-hitch-[0-9]{8}-[0-9]{6}\\.tachyon"))) << file;
  EXPECT_EQ(fs::path(planned->path).parent_path(), h.dir.path());

  auto flushed = nextFlushed(c);
  ASSERT_TRUE(flushed.ok());
  EXPECT_EQ(flushed->path, planned->path);
  EXPECT_EQ(flushed->reason, "hitch");
  ASSERT_TRUE(fs::exists(flushed->path));
  const std::string info = captureInfoOf(flushed->path);
  EXPECT_NE(info.find("\"flush_reason\":\"hitch\""), std::string::npos) << info;
  EXPECT_NE(info.find("\"flush_window_s\":6"), std::string::npos) << "the agent's window, not the request's";
  EXPECT_NE(info.find("\"flush_trigger\":true"), std::string::npos) << info;
  EXPECT_EQ(h.agent->state(), CaptureState::Capturing);
}

TEST(FlightRecorder, TriggersInsideTheCoalescingWindowAreCountedNotFlushed) {
  AgentConfig cfg;
  cfg.flushCoalesceMs = 60'000;
  cfg.flushSettleMs = 0;
  FlightHarness h(cfg);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("coal.tachyon", 64), 10000)->ok);

  auto first = c.call(triggerRequest("hitch"), 10000);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(first->ok) << first->error;
  const std::string path = std::get<CaptureSummary>(first->body).path;
  auto flushed = nextFlushed(c);
  ASSERT_TRUE(flushed.ok());
  ASSERT_EQ(flushed->path, path);

  for (int i = 0; i < 3; ++i) {
    auto again = c.call(triggerRequest("hitch"), 10000);
    ASSERT_TRUE(again.ok());
    EXPECT_TRUE(again->ok) << "a coalesced trigger is still an accepted one";
    EXPECT_EQ(std::get<CaptureSummary>(again->body).path, path) << "it points at the flush it joined";
  }
  // A flush asked for by a person is not a trigger; its event reports what was folded away.
  auto manual = c.call(flushRequest("manual"), 10000);
  ASSERT_TRUE(manual.ok());
  ASSERT_TRUE(manual->ok) << manual->error;
  auto second = nextFlushed(c);
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(second->name, "manual");
  EXPECT_EQ(second->coalesced, 3u);
  size_t traces = 0;
  for (const auto& entry : fs::directory_iterator(h.dir.path()))
    traces += entry.path().extension() == ".tachyon" ? 1 : 0;
  EXPECT_EQ(traces, 3u) << "fixture, the first trigger, the manual flush";
}

TEST(FlightRecorder, StopWaitsForABackgroundFlushAndTheCaptureCompletes) {
  AgentConfig cfg;
  cfg.flushSettleMs = 300;  // the flush is still pending when stop arrives
  FlightHarness h(cfg);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("stopwait.tachyon", 64), 10000)->ok);
  auto r = c.call(triggerRequest("late"), 10000);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r->ok);
  const std::string path = std::get<CaptureSummary>(r->body).path;
  h.stopAndWait(c);
  EXPECT_TRUE(fs::exists(path)) << "stop did not run ahead of the flush";
}

TEST(FlightRecorder, TwentyFlushesOfARunningCaptureDoNotGrowDescriptorsOrDisk) {
  AgentConfig cfg;
  cfg.minRingBytes = 1 << 20;  // 1 MiB ring per process, 128 KiB segments
  FlightHarness h(cfg);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("many.tachyon", 1), 10000)->ok);
  const fs::path spoolDir = h.dir.path() / "many.tachyon.inst";

  constexpr uint32_t kPid = 77;
  const std::string address = h.hello(c, kPid);
  ASSERT_FALSE(address.empty());
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  StreamBuilder head;
  head.header(kPid, 1'000'000'000, 1);
  const uint64_t key = head.string("flight_scope");
  head.threadName(5000, kPid, "flight-thread");
  ASSERT_TRUE(sdk.send(head.bytes().data(), head.bytes().size()));

  uint64_t ts = 10'000;
  const auto sendBatch = [&](int pairs) {
    StreamBuilder b;
    for (int i = 0; i < pairs; ++i) {
      b.scopeBegin(ts, kPid, key, "some-payload");
      b.scopeEnd(ts + 3, kPid);
      ts += 10;
    }
    ASSERT_TRUE(sdk.send(b.bytes().data(), b.bytes().size()));
  };
  const auto spoolBytes = [&] {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(spoolDir, ec))
      if (entry.is_regular_file(ec)) total += entry.file_size(ec);
    return total;
  };
  const auto openFds = [] {
    size_t n = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc/self/fd", ec)) {
      (void)entry;
      ++n;
    }
    return n;
  };

  size_t fdsAfterFirst = 0;
  uint64_t maxSpool = 0;
  constexpr int kFlushes = 24;
  for (int i = 0; i < kFlushes; ++i) {
    sendBatch(8000);  // about 600 KiB per round: the 1 MiB ring rotates every other round
    auto r = c.call(flushRequest("many"), 20000);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r->ok) << r->error;
    auto flushed = nextFlushed(c);
    ASSERT_TRUE(flushed.ok());
    const std::string path = flushed->path;
    {
      auto reader = trace::TraceReader::open(path);
      ASSERT_TRUE(reader.ok()) << path;
      const auto scopes = reader->readScopeEvents().value();
      if (i > 2) {
        EXPECT_FALSE(scopes.empty()) << "flush " << i;
        const inst::ScopeBalance balance = inst::checkScopeBalance(scopes);
        EXPECT_EQ(balance.unmatchedEnds, 0u) << "flush " << i;
        EXPECT_EQ(balance.unclosedBegins, 0u) << "flush " << i;
      }
    }
    std::error_code ec;
    fs::remove(path, ec);  // a person would move them away; the test checks the agent's own footprint
    if (i == 1) fdsAfterFirst = openFds();
    maxSpool = std::max(maxSpool, spoolBytes());
  }
#if TACHYON_PLATFORM_LINUX
  EXPECT_LE(openFds(), fdsAfterFirst + 2) << "descriptors grew over " << kFlushes << " flushes";
#else
  (void)fdsAfterFirst;
#endif
  EXPECT_LE(maxSpool, (1u << 20) + (1u << 20) / 8 + 4096) << "the ring is bounded";
  h.stopAndWait(c);
}

TEST(FlightRecorder, TheRingKeepsTheNewestInstrumentationForTheFinalTraceToo) {
  AgentConfig cfg;
  cfg.minRingBytes = 1 << 20;
  FlightHarness h(cfg);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("final.tachyon", 1), 10000)->ok);
  constexpr uint32_t kPid = 78;
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(h.hello(c, kPid)));
  StreamBuilder head;
  head.header(kPid, 1'000'000'000, 1);
  const uint64_t key = head.string("final_scope");
  ASSERT_TRUE(sdk.send(head.bytes().data(), head.bytes().size()));
  uint64_t ts = 10'000;
  for (int round = 0; round < 20; ++round) {  // 12 MB: the ring (1 MiB) keeps the newest tenth
    StreamBuilder b;
    for (int i = 0; i < 6000; ++i) {
      b.scopeBegin(ts, kPid, key, "payload-payload");
      b.scopeEnd(ts + 3, kPid);
      ts += 8;  // stays below the window end (2'000'000)
    }
    ASSERT_TRUE(sdk.send(b.bytes().data(), b.bytes().size()));
  }
  const uint64_t lastBegin = ts - 8;
  sdk.close();
  h.stopAndWait(c);
  auto reader = trace::TraceReader::open(h.dir.path() / "final.tachyon");
  ASSERT_TRUE(reader.ok());
  const auto scopes = reader->readScopeEvents().value();
  ASSERT_FALSE(scopes.empty());
  EXPECT_LT(scopes.size(), 120'000u * 2) << "older records fell out of the ring";
  EXPECT_EQ(trace::ScopeEventRec(scopes.back()).ts, lastBegin + 3);
  const inst::ScopeBalance balance = inst::checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
}

}  // namespace
}  // namespace tachyon::agent
