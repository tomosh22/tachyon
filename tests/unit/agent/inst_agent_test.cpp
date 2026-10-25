// WP-52: instrumentation ingestion inside the agent (Agent + MockBackend + real control server + raw
// SDK-style clients on the per-process instrumentation socket / pipe).
#include "../control/test_util.h"
#include "../inst/packed.h"
#include "../inst/raw_client.h"
#include "../inst/trace_builder.h"
#include "../inst/wire_builder.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "cli/cli.h"
#include "control/client.h"
#include "inst/decoder.h"
#include "inst/merge.h"
#include "process_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using capture::CaptureState;
using control::Client;
using control::testing_util::TempDir;
using control::testing_util::testAddress;
using inst::testing_util::pv;
using inst::testing_util::RawClient;
using inst::testing_util::StreamBuilder;
using namespace control;

constexpr uint64_t kTraceStart = 1000;
constexpr uint64_t kTraceEnd = 2'000'000;

struct InstHarness {
  TempDir dir;
  fs::path fixture;
  std::string address = testAddress("instagent");
  std::unique_ptr<Agent> agent;
  std::unique_ptr<Server> server;

  explicit InstHarness(AgentConfig tweaks = {}, bool junkFixture = false) {
    fixture = dir.path() / "fixture.tachyon";
    if (junkFixture) {
      std::ofstream(fixture, std::ios::binary) << "this is not a trace file at all";
    } else {
      inst::testing_util::writeBaseTrace(fixture, kTraceStart, kTraceEnd);
    }
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture;
    AgentConfig ac = std::move(tweaks);
    ac.backendName = "mock";
    ac.agentAddress = address;
    ac.instDrainIdleMs = 100;
    ac.instDrainMaxMs = 2000;
    agent = std::make_unique<Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), *agent);
    agent->setEventSink([this](const Event& e) { server->broadcast(e); });
    server->start();
  }
  ~InstHarness() {
    server->stop();
    agent->shutdown();
  }

  Client connect() {
    auto c = Client::connectWithRetry(address, 5000);
    EXPECT_TRUE(c.ok());
    return std::move(*c);
  }
  StartRequest startRequest(const std::string& out, uint32_t circularMb = 0) const {
    StartRequest s;
    s.mode = capture::CaptureMode::System;
    s.out_path = (dir.path() / out).string();
    s.circular_mb = circularMb;
    return s;
  }
  fs::path spoolDir(const std::string& out) const { return dir.path() / (out + ".inst"); }

  // The SDK's handshake: inst_hello on the control channel, returns the data pipe / socket address.
  std::string hello(Client& c, uint32_t pid) {
    auto r = c.call(InstHelloRequest{pid, kProtocolApiVersion}, 10000);
    EXPECT_TRUE(r.ok());
    if (!r.ok() || !r->ok) return {};
    const auto* body = std::get_if<InstHelloBody>(&r->body);
    return body != nullptr ? body->pipe : std::string();
  }

  // stop + wait for capture_done; returns the summary's path.
  std::string stopAndWait(Client& c) {
    auto stop = c.call(StopRequest{}, 10000);
    EXPECT_TRUE(stop.ok());
    EXPECT_TRUE(stop->ok) << stop->error;
    auto done =
        c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 20000);
    EXPECT_TRUE(done.ok());
    return done.ok() ? std::get<CaptureDoneEvent>(*done).summary.path : std::string();
  }
};

StreamBuilder processStream(uint32_t pid, uint64_t t0, uint32_t pairs = 10) {
  StreamBuilder b;
  b.header(pid, 1'000'000'000, t0);
  const uint64_t frame = b.string("frame");
  const uint64_t update = b.string("update");
  b.threadName(t0, pid, "main-" + std::to_string(pid));
  for (uint32_t i = 0; i < pairs; ++i) {
    const uint64_t base = t0 + 100 + static_cast<uint64_t>(i) * 100;
    b.scopeBegin(base, pid, frame);
    b.scopeBegin(base + 10, pid, update, "i=" + std::to_string(i));
    b.scopeEnd(base + 40, pid);
    b.scopeEnd(base + 90, pid);
  }
  return b;
}

std::vector<char> bytesOf(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(AgentInst, HelloOutsideACaptureIsNotFound) {
  InstHarness h;
  Client c = h.connect();
  auto r = c.call(InstHelloRequest{1234, kProtocolApiVersion}, 10000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->code, ErrorCode::NotFound);

  // During a capture it works; after the capture it is NotFound again.
  ASSERT_TRUE(c.call(h.startRequest("a.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 1234);
  EXPECT_NE(address.find("tachyon-inst-1234"), std::string::npos);
  (void)h.stopAndWait(c);
  auto after = c.call(InstHelloRequest{1234, kProtocolApiVersion}, 10000);
  ASSERT_TRUE(after.ok());
  EXPECT_FALSE(after->ok);
}

TEST(AgentInst, ScopesFromAnInstrumentedProcessEndUpInTheTrace) {
  InstHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("cap.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 4242);
  ASSERT_FALSE(address.empty());
  const StreamBuilder s = processStream(4242, 5000);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(
      sdk.send(s.bytes().data(), s.bytes().size()));  // the connection stays open, as in a live process
  EXPECT_TRUE(fs::exists(h.spoolDir("cap.tachyon")));

  const std::string path = h.stopAndWait(c);
  ASSERT_EQ(fs::path(path), h.dir.path() / "cap.tachyon");
  auto reader = trace::TraceReader::open(path);
  ASSERT_TRUE(reader.ok());
  EXPECT_NE(reader->header().flags & trace::kFlagHasInstrumentation, 0u);
  const auto scopes = reader->readScopeEvents().value();
  EXPECT_EQ(scopes.size(), 40u);
  const auto balance = inst::checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);
  const trace::StringTable strings = reader->readStrings().value();
  const trace::ScopeEventRec first = scopes[0];
  EXPECT_EQ(strings.get(first.id), "frame");
  EXPECT_EQ(pv(first.tid), 4242u);
  const auto names = reader->readThreadNames().value();
  ASSERT_EQ(names.size(), 1u);
  const trace::ThreadNameRec tn = names[0];
  EXPECT_EQ(strings.get(tn.name), "main-4242");
  // The sampling data of the backend's trace is still there.
  EXPECT_EQ(reader->readSamples().value().size(), 50u);
  EXPECT_FALSE(reader->isPartial());
  // The spool directory is cleaned up after a successful merge, and nothing else is left in the output dir.
  EXPECT_FALSE(fs::exists(h.spoolDir("cap.tachyon")));
  EXPECT_FALSE(fs::exists(address));
}

TEST(AgentInst, DataSentJustBeforeStopIsNotLost) {
  InstHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("late.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 77);
  const StreamBuilder s = processStream(77, 5000, 200);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(sdk.sendChunked(s.bytes(), 1000));
  const std::string path = h.stopAndWait(c);  // stop right away: the agent drains what is in flight
  auto reader = trace::TraceReader::open(path);
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(reader->readScopeEvents().value().size(), 800u);
}

TEST(AgentInst, TwoProcessesAreSpooledSeparatelyAndMergedTogether) {
  InstHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("two.tachyon"), 10000)->ok);
  const std::string a1 = h.hello(c, 100), a2 = h.hello(c, 200);
  ASSERT_NE(a1, a2);
  const StreamBuilder s1 = processStream(100, 5000), s2 = processStream(200, 5050);
  RawClient c1, c2;
  ASSERT_TRUE(c1.connect(a1));
  ASSERT_TRUE(c2.connect(a2));
  ASSERT_TRUE(c1.send(s1.bytes().data(), s1.bytes().size()));
  ASSERT_TRUE(c2.send(s2.bytes().data(), s2.bytes().size()));
  const std::string path = h.stopAndWait(c);
  auto reader = trace::TraceReader::open(path);
  ASSERT_TRUE(reader.ok());
  const auto scopes = reader->readScopeEvents().value();
  EXPECT_EQ(scopes.size(), 80u);
  uint64_t last = 0;
  size_t onA = 0, onB = 0;
  for (const auto& rec : scopes) {
    const trace::ScopeEventRec r = rec;
    EXPECT_GE(pv(r.ts), last);
    last = r.ts;
    (r.tid == 100 ? onA : onB)++;
  }
  EXPECT_EQ(onA, 40u);
  EXPECT_EQ(onB, 40u);
  EXPECT_EQ(reader->readThreadNames().value().size(), 2u);
}

TEST(AgentInst, ACaptureWithoutInstrumentedDataLeavesTheBackendsTraceUntouched) {
  InstHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("none.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 55);  // says hello, never sends a byte
  ASSERT_FALSE(address.empty());
  const std::string path = h.stopAndWait(c);
  EXPECT_EQ(bytesOf(path), bytesOf(h.fixture));
  EXPECT_FALSE(fs::exists(h.spoolDir("none.tachyon")));
}

TEST(AgentInst, SpoolCapIsAnAgentSettingAndOverflowBecomesADroppedMarker) {
  AgentConfig tweaks;
  tweaks.maxInstSpoolBytes = 700;
  InstHarness h(tweaks);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("cap.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 9);
  const StreamBuilder s = processStream(9, 5000, 100);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(sdk.send(s.bytes().data(), s.bytes().size()));
  const std::string path = h.stopAndWait(c);
  auto reader = trace::TraceReader::open(path);
  ASSERT_TRUE(reader.ok());
  const trace::StringTable strings = reader->readStrings().value();
  const auto markers = reader->readMarkers().value();
  uint64_t dropped = 0;
  for (const auto& rec : markers) {
    const trace::MarkerRec m = rec;
    if (strings.get(m.name) == inst::kDroppedMarkerName)
      dropped += std::stoull(std::string(strings.get(m.data)));
  }
  const size_t kept = reader->readScopeEvents().value().size() + reader->readThreadNames().value().size();
  EXPECT_GT(dropped, 0u);
  EXPECT_EQ(kept + dropped, 401u);  // 400 scope events and the thread name
}

TEST(AgentInst, KeepInstSpoolLeavesTheFilesAndTheCliCanMergeThemAgain) {
  AgentConfig tweaks;
  tweaks.keepInstSpool = true;
  InstHarness h(tweaks);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("keep.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 31);
  const StreamBuilder s = processStream(31, 5000);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(sdk.send(s.bytes().data(), s.bytes().size()));
  const std::string path = h.stopAndWait(c);
  const fs::path spool = h.spoolDir("keep.tachyon") / "inst-31.tachyon-inst";
  ASSERT_TRUE(fs::exists(spool));
  // The spool is exactly what the client sent: a valid sidecar stream.
  EXPECT_EQ(fs::file_size(spool), s.bytes().size());

  // `tachyon-cli merge` redoes the merge on the backend's original output.
  const fs::path again = h.dir.path() / "again.tachyon";
  fs::copy_file(h.fixture, again);
  std::ostringstream out, err;
  const int code = cli::run({"merge", again.string(), spool.string()}, out, err);
  EXPECT_EQ(code, cli::kExitOk) << err.str();
  auto a = trace::TraceReader::open(path);
  auto b = trace::TraceReader::open(again);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a->readScopeEvents().value(), b->readScopeEvents().value());
}

TEST(AgentInst, CancelDiscardsTheSpoolAndTheTrace) {
  InstHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("cancel.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 21);
  const StreamBuilder s = processStream(21, 5000);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(sdk.send(s.bytes().data(), s.bytes().size()));
  auto r = c.call(CancelRequest{}, 10000);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->ok) << r->error;
  EXPECT_FALSE(fs::exists(h.spoolDir("cancel.tachyon")));
  EXPECT_FALSE(fs::exists(h.dir.path() / "cancel.tachyon"));
  EXPECT_EQ(h.agent->state(), CaptureState::Idle);
}

TEST(AgentInst, FlushMergesASnapshotClippedToTheFlushedWindowAndTheCaptureGoesOn) {
  InstHarness h;
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("fr.tachyon", /*circularMb=*/8), 10000)->ok);
  const std::string address = h.hello(c, 61);
  // Window of the (mock) flushed trace is [1000, 2'000'000]; events before and after it are not part of it.
  StreamBuilder s;
  s.header(61, 1'000'000'000, 1);
  const uint64_t k = s.string("hitch");
  s.scopeBegin(500, 61, k);  // before the window
  s.scopeEnd(600, 61);
  s.scopeBegin(100'000, 61, k);  // inside
  s.scopeEnd(150'000, 61);
  s.scopeBegin(3'000'000, 61, k);  // after
  s.scopeEnd(3'000'100, 61);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(sdk.send(s.bytes().data(), s.bytes().size()));
  // The reader thread spools asynchronously: flush until the snapshot has seen the frames.
  FlushRequest flush;
  flush.name = "hitch-1";
  flush.seconds = 5;
  std::string flushedPath;
  std::vector<trace::ScopeEventRec> flushedScopes;
  std::optional<trace::TraceReader> flushed;
  for (int attempt = 0; attempt < 500 && flushedScopes.size() != 2; ++attempt) {
    auto fr = c.call(flush, 10000);
    ASSERT_TRUE(fr.ok());
    ASSERT_TRUE(fr->ok) << fr->error;
    const auto* summary = std::get_if<CaptureSummary>(&fr->body);
    ASSERT_NE(summary, nullptr);
    auto opened = trace::TraceReader::open(summary->path);
    ASSERT_TRUE(opened.ok());
    flushed.emplace(std::move(*opened));
    flushedScopes = flushed->readScopeEvents().value();
    if (flushedScopes.size() != 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(flushedScopes.size(), 2u);  // the 'inside' pair only
  const trace::ScopeEventRec fb = flushedScopes[0];
  const uint64_t fbTs = fb.ts;
  EXPECT_EQ(fbTs, 100'000u);
  EXPECT_EQ(flushed->header().start_ts, kTraceStart);  // clipped, not extended
  EXPECT_EQ(flushed->header().end_ts, kTraceEnd);
  EXPECT_EQ(h.agent->state(), CaptureState::Capturing);

  // The capture continues; the stop merge of this circular capture is clipped to its window too.
  const std::string path = h.stopAndWait(c);
  auto final = trace::TraceReader::open(path);
  ASSERT_TRUE(final.ok());
  EXPECT_EQ(final->readScopeEvents().value().size(), 2u);
}

TEST(AgentInst, AFailedMergeStillCompletesTheCaptureAndKeepsTheSpool) {
  InstHarness h({}, /*junkFixture=*/true);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("junk.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 8);
  const StreamBuilder s = processStream(8, 5000);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(sdk.send(s.bytes().data(), s.bytes().size()));
  const std::string path = h.stopAndWait(c);  // capture_done arrives anyway
  EXPECT_TRUE(fs::exists(path));
  EXPECT_TRUE(fs::exists(h.spoolDir("junk.tachyon") / "inst-8.tachyon-inst"));
  EXPECT_EQ(h.agent->state(), CaptureState::Idle);
}

TEST(AgentInst, SpoolRootSettingMovesTheSpoolDirectory) {
  TempDir root;
  AgentConfig tweaks;
  tweaks.instSpoolRoot = root.path();
  tweaks.keepInstSpool = true;
  InstHarness h(tweaks);
  Client c = h.connect();
  ASSERT_TRUE(c.call(h.startRequest("moved.tachyon"), 10000)->ok);
  const std::string address = h.hello(c, 3);
  const StreamBuilder s = processStream(3, 5000);
  RawClient sdk;
  ASSERT_TRUE(sdk.connect(address));
  ASSERT_TRUE(sdk.send(s.bytes().data(), s.bytes().size()));
  (void)h.stopAndWait(c);
  EXPECT_TRUE(fs::exists(root.path() / "moved.tachyon.inst" / "inst-3.tachyon-inst"));
  EXPECT_FALSE(fs::exists(h.spoolDir("moved.tachyon")));
}

TEST(AgentInst, ASecondCaptureStartsWithAFreshIngest) {
  InstHarness h;
  Client c = h.connect();
  for (int round = 0; round < 2; ++round) {
    const std::string out = "round" + std::to_string(round) + ".tachyon";
    ASSERT_TRUE(c.call(h.startRequest(out), 10000)->ok);
    const std::string address = h.hello(c, 500);
    const StreamBuilder s = processStream(500, 5000, 5);
    RawClient sdk;
    ASSERT_TRUE(sdk.connect(address));
    ASSERT_TRUE(sdk.send(s.bytes().data(), s.bytes().size()));
    const std::string path = h.stopAndWait(c);
    auto reader = trace::TraceReader::open(path);
    ASSERT_TRUE(reader.ok());
    EXPECT_EQ(reader->readScopeEvents().value().size(), 20u) << "round " << round;
    // Wait for the state to return to idle before starting the next one.
    auto idle = c.waitForEvent(
        [](const Event& e) {
          const auto* st = std::get_if<StateEvent>(&e);
          return st != nullptr && st->state == CaptureState::Idle;
        },
        10000);
    ASSERT_TRUE(idle.ok());
  }
}

}  // namespace
}  // namespace tachyon::agent
