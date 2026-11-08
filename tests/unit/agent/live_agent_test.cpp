// WP-65a: the live protocol end to end against an in-process Agent with the mock backend's live source:
// subscribe / keyframe / batches / from_seq / unsubscribe / live_end / live_gap, status fields, remote
// clients, and a live model of a whole replay that matches the final trace.
#include "../control/test_util.h"
#include "agent/agent.h"
#include "agent/stub_backend.h"
#include "analysis/query.h"
#include "analysis/thread_states.h"
#include "capture/mock/mock_backend.h"
#include "control/client.h"
#include "control/tcp.h"
#include "model/live_model.h"
#include "model/model.h"
#include "symbols/symbol_service.h"
#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <map>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using control::Client;
using control::testing_util::TempDir;
using namespace control;

constexpr uint64_t kStart = 1'000'000'000ull;  // trace clock 1 GHz, first timestamp
constexpr uint32_t kPid = 4242;

// A trace with one process, a module, two threads and `ms` milliseconds of 1 kHz samples per thread, plus
// context switches and the instrumentation record kinds. Stacks (root first): main > update > physics (50%),
// main > update (30%), main > render (20%).
fs::path writeLiveFixture(const fs::path& path, uint32_t ms) {
  trace::FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = kStart;
  h.end_ts = kStart + static_cast<uint64_t>(ms) * 1'000'000;
  h.platform = trace::kPlatformLinux;
  auto w = trace::TraceWriter::open(path, h);
  EXPECT_TRUE(w.ok());
  if (!w.ok()) return path;
  trace::ProcessRec proc{};
  proc.pid = kPid;
  proc.parent_pid = 1;
  proc.start_ts = kStart;
  proc.name = w->internString("livegame");
  proc.command_line = w->internString("/bin/livegame --live");
  w->append(std::span<const trace::ProcessRec>(&proc, 1));
  trace::ModuleRec mod{};
  mod.pid = kPid;
  mod.base = 0x400000;
  mod.size = 0x100000;
  mod.path = w->internString("/bin/livegame");
  mod.id_kind = trace::kModuleIdGnuBuildId;
  mod.id_len = 4;
  mod.id[0] = 0xAA;
  mod.load_ts = kStart;
  w->append(std::span<const trace::ModuleRec>(&mod, 1));
  trace::ThreadRec threads[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    threads[i].tid = kPid + i;
    threads[i].pid = kPid;
    threads[i].start_ts = kStart;
    threads[i].name = w->internString(i == 0 ? "main" : "worker");
    threads[i].flags = trace::kThreadFlagTargetProcess;
  }
  w->append(std::span<const trace::ThreadRec>(threads, 2));

  const FrameIdx fMain = w->internFrame(ModuleIdx(0), 0x1000), fUpdate = w->internFrame(ModuleIdx(0), 0x2000),
                 fPhysics = w->internFrame(ModuleIdx(0), 0x3000),
                 fRender = w->internFrame(ModuleIdx(0), 0x4000);
  const FrameIdx s0[] = {fPhysics, fUpdate, fMain}, s1[] = {fUpdate, fMain}, s2[] = {fRender, fMain};
  const CallstackIdx stacks[3] = {w->internCallstack(s0), w->internCallstack(s1), w->internCallstack(s2)};

  trace::SampleBatch samples;
  for (uint32_t i = 0; i < ms; ++i) {
    for (uint32_t t = 0; t < 2; ++t) {
      const uint32_t r = (i * 7 + t * 3) % 10;
      const CallstackIdx st = r < 5 ? stacks[0] : (r < 8 ? stacks[1] : stacks[2]);
      samples.push(kStart + static_cast<uint64_t>(i) * 1'000'000 + t * 1000, kPid + t, st,
                   static_cast<uint16_t>(t));
    }
  }
  w->append(samples);

  std::vector<trace::ContextSwitchRec> switches;
  for (uint32_t i = 0; i + 1 < ms; i += 10) {
    trace::ContextSwitchRec cs{};
    cs.ts = kStart + static_cast<uint64_t>(i) * 1'000'000 + 500'000;
    cs.old_tid = kPid;
    cs.new_tid = kPid + 1;
    cs.old_state = 5;
    cs.old_thread_stack = stacks[1];
    switches.push_back(cs);
    cs.ts += 200'000;
    cs.old_tid = kPid + 1;
    cs.new_tid = kPid;
    cs.old_thread_stack = CallstackIdx::invalid();
    switches.push_back(cs);
  }
  w->append(std::span<const trace::ContextSwitchRec>(switches));
  std::vector<trace::ScopeEventRec> scopes;
  for (uint32_t i = 0; i + 1 < ms; i += 20) {
    trace::ScopeEventRec b{}, e{};
    b.ts = kStart + static_cast<uint64_t>(i) * 1'000'000 + 100;
    b.tid = kPid;
    b.kind = trace::kScopeBegin;
    b.id = w->internString("Update");
    e.ts = b.ts + 5'000'000;
    e.tid = kPid;
    e.kind = trace::kScopeEnd;
    scopes.push_back(b);
    scopes.push_back(e);
  }
  w->append(std::span<const trace::ScopeEventRec>(scopes));
  trace::FrameMarkRec fm{};
  fm.ts = kStart + 4'000'000;
  fm.tid = kPid;
  fm.name = w->internString("Frame");
  fm.frame_number = 1;
  w->append(std::span<const trace::FrameMarkRec>(&fm, 1));
  trace::PlotSampleRec plot{};
  plot.ts = kStart + 6'000'000;
  plot.name = w->internString("fps");
  plot.value = 60.0;
  w->append(std::span<const trace::PlotSampleRec>(&plot, 1));
  w->appendCaptureInfo("{\"sampling_hz\":1000,\"capture\":\"live-fixture\"}");
  EXPECT_TRUE(w->finish().ok());
  return path;
}

// A client that applies the live events it reads to a LiveModel.
struct LiveClient {
  Client client;
  model::LiveModel model;
  std::vector<uint64_t> seqs;  // seq of every live event, in order
  std::vector<bool> keyframes;
  uint64_t lastSeq = 0;
  bool gap = false, end = false;
  uint64_t gapSeq = 0, endSeq = 0;
  std::string applyError;
  bool keyframePending = false;

  explicit LiveClient(Client c, model::LiveModelOptions o = {}) : client(std::move(c)), model(o) {
    client.setLiveHandler([this](const Event& e) { onEvent(e); });
  }

  void onEvent(const Event& e) {
    if (const auto* le = std::get_if<LiveEvent>(&e)) {
      seqs.push_back(le->seq);
      keyframes.push_back(le->keyframe);
      const auto* p = reinterpret_cast<const std::byte*>(le->data.data());
      auto r = model.apply({p, le->data.size()}, le->keyframe);
      if (!r.ok()) applyError = r.error().message;
      lastSeq = le->seq;
    } else if (const auto* g = std::get_if<LiveGapEvent>(&e)) {
      gap = true;
      gapSeq = g->seq;
    } else if (const auto* en = std::get_if<LiveEndEvent>(&e)) {
      end = true;
      endSeq = en->seq;
    }
  }

  // Reads until `pred()` holds (live events are applied by the handler as they arrive).
  template <typename Pred>
  bool until(Pred pred, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      auto ev = client.nextEvent(50);  // non-live events are returned and dropped here
      (void)ev;
    }
    return true;
  }
};

struct LiveHarness {
  TempDir dir;
  fs::path fixture;
  std::string address = testing_util::testAddress("live");
  capture::MockBackend* mock = nullptr;
  std::unique_ptr<Agent> agent;
  std::unique_ptr<Server> server;
  std::string tcpAddress;

  explicit LiveHarness(uint32_t ms = 400, double speed = 0.0, AgentConfig ac = {}, bool tcp = false) {
    fixture = writeLiveFixture(dir.path() / "fixture.tachyon", ms);
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture;
    cfg.live.speed = speed;
    auto backend = std::make_unique<capture::MockBackend>(cfg);
    mock = backend.get();
    ac.backendName = "mock";
    ac.agentAddress = address;
    ac.verifyInstPeerPid = false;
    ac.remoteOutDir = dir.path();
    agent = std::make_unique<Agent>(std::move(backend), ac);
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), *agent);
    if (tcp) {
      auto t = listenTcp(TcpEndpoint{"127.0.0.1", 0});
      EXPECT_TRUE(t.ok());
      tcpAddress = "tcp://" + (*t)->address();
      AuthPolicy policy;
      policy.token = "live-test-token-0123456789-abcdefghijkl";
      server->addRemoteListener(std::move(*t), std::move(policy));
    }
    agent->setEventSink([this](const Event& e) { server->broadcast(e); });
    server->start();
  }
  ~LiveHarness() {
    server->stop();
    agent->shutdown();
  }

  Client connect() {
    auto c = Client::connectWithRetry(address, 5000);
    EXPECT_TRUE(c.ok());
    return std::move(*c);
  }
  Client connectRemote() {
    auto c = Client::connectWithRetry(tcpAddress, 5000, "live-test-token-0123456789-abcdefghijkl");
    EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().toString());
    return std::move(*c);
  }
  StartRequest liveStart(const std::string& out = "live.tachyon", uint32_t circularMb = 0) const {
    StartRequest s;
    s.mode = capture::CaptureMode::System;
    s.out_path = (dir.path() / out).string();
    s.live = true;
    s.circular_mb = circularMb;
    return s;
  }
  // Pumps the whole replay (the fixture covers [kStart, kStart + ms]).
  void pumpAll() { mock->pumpLive(mock->liveEndTs() + 1'000'000); }
};

bool callOk(Client& c, RequestBody body) {
  auto r = c.call(std::move(body), 10000);
  return r.ok() && r->ok;
}

uint64_t sampleCount(const model::TraceModel& m) {
  uint64_t n = 0;
  for (const auto& c : m.samplesByThread) n += c.ts.size();
  return n;
}

// Exclusive share of each function (module path + rva), in percent, over all samples.
std::map<std::string, double> functionShares(const model::TraceModel& source) {
  model::TraceModel m = source;
  EXPECT_TRUE(analysis::buildThreadStates(m).ok());
  symbols::SymbolService svc(std::vector<std::string>{}, fs::path{});
  analysis::QueryContext ctx(m, svc);
  auto rows = analysis::bottomUpFunctions(ctx, analysis::Filter{});
  EXPECT_TRUE(rows.ok());
  std::map<std::string, double> out;
  double total = 0;
  for (const auto& r : *rows) total += static_cast<double>(r.exclusiveTicks);
  for (const auto& r : *rows) {
    std::string name = r.key.module.valid() ? m.modules[r.key.module.get()].path : "<abs>";
    name += "+" + std::to_string(r.key.value);
    out[name] += total > 0 ? 100.0 * static_cast<double>(r.exclusiveTicks) / total : 0.0;
  }
  return out;
}

TEST(LiveAgent, SubscribeWithoutALiveCaptureIsNotFound) {
  LiveHarness h;
  Client c = h.connect();
  auto r = c.call(LiveSubscribeRequest{}, 5000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->code, ErrorCode::NotFound);
  // A capture that is not live has nothing to subscribe to either.
  StartRequest plain = h.liveStart("plain.tachyon");
  plain.live = false;
  ASSERT_TRUE(callOk(c, plain));
  r = c.call(LiveSubscribeRequest{}, 5000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->code, ErrorCode::NotFound);
  auto status = c.call(StatusRequest{}, 5000);
  ASSERT_TRUE(status.ok());
  EXPECT_FALSE(std::get<StatusBody>(status->body).live);
  EXPECT_TRUE(callOk(c, CancelRequest{}));
}

TEST(LiveAgent, BackendWithoutALiveSourceRefusesALiveStart) {
  TempDir dir;
  AgentConfig ac;
  ac.backendName = "stub";
  const std::string address = testing_util::testAddress("livestub");
  Agent agent(std::make_unique<StubBackend>("stub"), ac);
  auto l = listenOn(address);
  ASSERT_TRUE(l.ok());
  Server server(std::move(*l), agent);
  server.start();
  auto c = Client::connectWithRetry(address, 5000);
  ASSERT_TRUE(c.ok());
  StartRequest s;
  s.live = true;
  s.out_path = (dir.path() / "x.tachyon").string();
  auto r = c->call(s, 5000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_NE(r->error.find("no live source"), std::string::npos) << r->error;
  server.stop();
}

TEST(LiveAgent, StatusReportsLiveWhileALiveCaptureRuns) {
  LiveHarness h;
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.liveStart()));
  h.mock->pumpLive(kStart + 100'000'000);
  auto status = c.call(StatusRequest{}, 5000);
  ASSERT_TRUE(status.ok());
  const auto& sb = std::get<StatusBody>(status->body);
  EXPECT_TRUE(sb.live);
  ASSERT_TRUE(callOk(c, StopRequest{}));
  auto done =
      c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000);
  ASSERT_TRUE(done.ok());
  status = c.call(StatusRequest{}, 5000);
  ASSERT_TRUE(status.ok());
  EXPECT_FALSE(std::get<StatusBody>(status->body).live);
}

TEST(LiveAgent, KeyframeThenBatchesThenEndAndTheLiveModelMatchesTheFinalTrace) {
  LiveHarness h(/*ms=*/400);
  LiveClient lc(h.connect());
  ASSERT_TRUE(callOk(lc.client, h.liveStart()));
  h.mock->pumpLive(kStart + 100'000'000);  // the first quarter, before anyone subscribes
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  auto sub = lc.client.call(LiveSubscribeRequest{}, 5000);
  ASSERT_TRUE(sub.ok());
  ASSERT_TRUE(sub->ok) << sub->error;
  const uint64_t keyframeSeq = std::get<LiveSubscribeBody>(sub->body).seq;
  EXPECT_GT(keyframeSeq, 0u);
  ASSERT_TRUE(lc.until([&] { return !lc.seqs.empty(); }));
  EXPECT_TRUE(lc.keyframes.front());        // the first event is a keyframe
  EXPECT_EQ(lc.seqs.front(), keyframeSeq);  // carrying the seq of the subscribe response

  // The rest of the replay arrives as batches.
  h.pumpAll();
  ASSERT_TRUE(lc.until([&] { return sampleCount(*lc.model.snapshot()) >= 800; }));
  EXPECT_TRUE(lc.applyError.empty()) << lc.applyError;
  for (size_t i = 1; i < lc.seqs.size(); ++i) {
    if (!lc.keyframes[i]) EXPECT_TRUE(lc.seqs[i] == lc.seqs[i - 1] || lc.seqs[i] == lc.seqs[i - 1] + 1) << i;
  }

  // Stop: live_end follows the last batch, then the capture completes.
  ASSERT_TRUE(callOk(lc.client, StopRequest{}));
  ASSERT_TRUE(lc.until([&] { return lc.end; }));
  EXPECT_EQ(lc.endSeq, lc.lastSeq);
  EXPECT_FALSE(lc.gap);
  auto done = lc.client.waitForEvent(
      [](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000);
  ASSERT_TRUE(done.ok());

  // The live model of the whole replay equals the final trace: same samples, same shares (+-1 point).
  auto finalModel = model::TraceModel::load(std::get<CaptureDoneEvent>(*done).summary.path);
  ASSERT_TRUE(finalModel.ok());
  auto live = lc.model.snapshot();
  EXPECT_EQ(sampleCount(*live), sampleCount(*finalModel));
  EXPECT_EQ(live->contextSwitches.size(), finalModel->contextSwitches.size());
  EXPECT_EQ(live->scopeEvents.size(), finalModel->scopeEvents.size());
  EXPECT_EQ(live->frameMarks.size(), finalModel->frameMarks.size());
  EXPECT_EQ(live->plots.size(), finalModel->plots.size());
  const auto liveShares = functionShares(*live);
  const auto finalShares = functionShares(*finalModel);
  ASSERT_FALSE(finalShares.empty());
  for (const auto& [name, share] : finalShares) {
    ASSERT_TRUE(liveShares.count(name)) << name;
    EXPECT_NEAR(liveShares.at(name), share, 1.0) << name;
  }
}

TEST(LiveAgent, LateSubscriberGetsTheWholeWindowInAKeyframe) {
  LiveHarness h(/*ms=*/300);
  Client driver = h.connect();
  ASSERT_TRUE(callOk(driver, h.liveStart()));
  h.pumpAll();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  LiveClient lc(h.connect());
  ASSERT_TRUE(callOk(lc.client, LiveSubscribeRequest{}));
  ASSERT_TRUE(lc.until([&] { return sampleCount(*lc.model.snapshot()) >= 600; }));
  EXPECT_EQ(sampleCount(*lc.model.snapshot()), 600u);
  EXPECT_TRUE(lc.keyframes.front());
  // Only the first piece of a keyframe is flagged.
  for (size_t i = 1; i < lc.keyframes.size(); ++i) EXPECT_FALSE(lc.keyframes[i]);
  EXPECT_TRUE(callOk(driver, CancelRequest{}));
}

TEST(LiveAgent, FromSeqContinuesWithoutAKeyframeAndAStaleSeqGetsOne) {
  LiveHarness h(/*ms=*/300);
  Client driver = h.connect();
  ASSERT_TRUE(callOk(driver, h.liveStart()));
  h.mock->pumpLive(kStart + 100'000'000);
  LiveClient lc(h.connect());
  ASSERT_TRUE(callOk(lc.client, LiveSubscribeRequest{}));
  ASSERT_TRUE(lc.until([&] { return lc.lastSeq > 0 && sampleCount(*lc.model.snapshot()) >= 200; }));
  ASSERT_TRUE(callOk(lc.client, LiveUnsubscribeRequest{}));
  const uint64_t had = lc.lastSeq;
  const size_t eventsBefore = lc.seqs.size();

  h.mock->pumpLive(kStart + 200'000'000);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  // After the unsubscribe nothing more arrives.
  (void)lc.client.nextEvent(100);
  EXPECT_EQ(lc.seqs.size(), eventsBefore);

  auto sub = lc.client.call(LiveSubscribeRequest{true, had + 1}, 5000);
  ASSERT_TRUE(sub.ok());
  ASSERT_TRUE(sub->ok) << sub->error;
  EXPECT_EQ(std::get<LiveSubscribeBody>(sub->body).seq, had + 1);
  ASSERT_TRUE(lc.until([&] { return lc.seqs.size() > eventsBefore; }));
  EXPECT_EQ(lc.seqs[eventsBefore], had + 1);
  EXPECT_FALSE(lc.keyframes[eventsBefore]);  // a continuation
  ASSERT_TRUE(lc.until([&] { return sampleCount(*lc.model.snapshot()) >= 400; }));

  // A seq from the future cannot be continued: keyframe.
  ASSERT_TRUE(callOk(lc.client, LiveUnsubscribeRequest{}));
  const size_t n = lc.seqs.size();
  sub = lc.client.call(LiveSubscribeRequest{true, 1'000'000}, 5000);
  ASSERT_TRUE(sub.ok());
  ASSERT_TRUE(sub->ok);
  ASSERT_TRUE(lc.until([&] { return lc.seqs.size() > n; }));
  EXPECT_TRUE(lc.keyframes[n]);
  EXPECT_TRUE(callOk(driver, CancelRequest{}));
}

TEST(LiveAgent, DisconnectAndAgentShutdownDoNotLeakSubscribers) {
  LiveHarness h(/*ms=*/100, /*speed=*/1.0);
  Client driver = h.connect();
  ASSERT_TRUE(callOk(driver, h.liveStart()));
  {
    LiveClient a(h.connect());
    ASSERT_TRUE(callOk(a.client, LiveSubscribeRequest{}));
    LiveClient b(h.connect());
    ASSERT_TRUE(callOk(b.client, LiveSubscribeRequest{}));
    ASSERT_TRUE(a.until([&] { return !a.seqs.empty(); }));
    ASSERT_TRUE(b.until([&] { return !b.seqs.empty(); }));
    a.client.close();  // a disconnects while b keeps streaming
    const size_t before = b.seqs.size();
    ASSERT_TRUE(b.until([&] { return b.seqs.size() > before; }));
  }
  // The harness destructor stops the server and the agent while a capture is still live (joins everything).
}

TEST(LiveAgent, TooManySubscribersAreRefused) {
  AgentConfig ac;
  ac.maxLiveSubscribers = 2;
  LiveHarness h(/*ms=*/100, 0.0, ac);
  Client driver = h.connect();
  ASSERT_TRUE(callOk(driver, h.liveStart()));
  Client a = h.connect(), b = h.connect(), c = h.connect();
  EXPECT_TRUE(callOk(a, LiveSubscribeRequest{}));
  EXPECT_TRUE(callOk(b, LiveSubscribeRequest{}));
  auto r = c.call(LiveSubscribeRequest{}, 5000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->code, ErrorCode::Busy);
  EXPECT_TRUE(callOk(driver, CancelRequest{}));
}

TEST(LiveAgent, AClientThatDoesNotReadGetsALiveGapAndCanResubscribe) {
  AgentConfig ac;
  ac.liveStallMs = 300;
  // 60 s of 1 kHz x 2 threads: 2.4 MB of samples, well over the 1.5 MiB queue limit plus the socket buffers.
  LiveHarness h(/*ms=*/60000, 0.0, ac);
  Client driver = h.connect();
  ASSERT_TRUE(callOk(driver, h.liveStart()));
  LiveClient lc(h.connect());
  ASSERT_TRUE(callOk(lc.client, LiveSubscribeRequest{}));
  // Do not read: push the replay through in slices of 3 s (120 KB each).
  for (uint64_t ms = 0; ms <= 60000; ms += 3000) {
    h.mock->pumpLive(kStart + ms * 1'000'000);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  // Even with unread data the agent keeps serving others (status from another client).
  Client other = h.connect();
  EXPECT_TRUE(callOk(other, StatusRequest{}));
  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // the stall limit passes
  // Now read: events up to the gap, then live_gap.
  ASSERT_TRUE(lc.until([&] { return lc.gap; }, 20000)) << "no live_gap after " << lc.seqs.size() << " events";
  EXPECT_FALSE(lc.end);
  const size_t before = lc.seqs.size();
  // Resubscribe: the next event is a keyframe.
  lc.gap = false;
  ASSERT_TRUE(callOk(lc.client, LiveSubscribeRequest{}));
  ASSERT_TRUE(lc.until([&] { return lc.seqs.size() > before; }));
  EXPECT_TRUE(lc.keyframes[before]);
  EXPECT_TRUE(callOk(driver, CancelRequest{}));
}

TEST(LiveAgent, AuthenticatedRemoteClientsCanSubscribe) {
  LiveHarness h(/*ms=*/200, 0.0, {}, /*tcp=*/true);
  Client driver = h.connect();
  ASSERT_TRUE(callOk(driver, h.liveStart()));
  h.pumpAll();
  LiveClient remote(h.connectRemote());
  ASSERT_TRUE(callOk(remote.client, LiveSubscribeRequest{}));
  ASSERT_TRUE(remote.until([&] { return sampleCount(*remote.model.snapshot()) >= 400; }));
  EXPECT_TRUE(callOk(driver, StopRequest{}));
  ASSERT_TRUE(remote.until([&] { return remote.end; }));
}

TEST(LiveAgent, TheCaptureStillProducesItsNormalTraceAtStop) {
  LiveHarness h(/*ms=*/100);
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.liveStart("normal.tachyon")));
  h.pumpAll();
  ASSERT_TRUE(callOk(c, StopRequest{}));
  auto done =
      c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000);
  ASSERT_TRUE(done.ok());
  const auto& summary = std::get<CaptureDoneEvent>(*done).summary;
  EXPECT_EQ(fs::path(summary.path), h.dir.path() / "normal.tachyon");
  EXPECT_TRUE(fs::exists(summary.path));
  EXPECT_EQ(summary.events_lost, 0u);
}

}  // namespace
}  // namespace tachyon::agent
