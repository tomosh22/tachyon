// In-process tests of the agent state machine: Agent + Server + Clients over a real transport.
#include "agent/agent.h"
#include "../control/test_util.h"
#include "capture/mock/mock_backend.h"
#include "control/client.h"
#include "fixture_util.h"
#include "process_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <thread>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using capture::CaptureState;
using control::Client;
using control::testing_util::TempDir;
using control::testing_util::testAddress;
using testing_util::readTextFile;
using testing_util::writeFixtureTrace;
using namespace control;

struct Harness {
  TempDir dir;
  fs::path fixture;
  std::string address = testAddress("agent");
  std::unique_ptr<Agent> agent;
  std::unique_ptr<Server> server;

  explicit Harness(int mockDurationMs = 0, uint32_t steps = 4) {
    fixture = writeFixtureTrace(dir.path() / "fixture.tachyon");
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture;
    cfg.captureDuration = std::chrono::milliseconds(mockDurationMs);
    cfg.progressSteps = steps;
    AgentConfig ac;
    ac.backendName = "mock";
    ac.agentAddress = address;
    ac.elevated = false;
    agent = std::make_unique<Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
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
  StartRequest startRequest(const std::string& out, uint32_t circularMb = 0) const {
    StartRequest s;
    s.mode = capture::CaptureMode::System;
    s.out_path = (dir.path() / out).string();
    s.circular_mb = circularMb;
    return s;
  }
};

Result<Response> callOk(Client& c, RequestBody body) {
  auto r = c.call(std::move(body), 10000);
  EXPECT_TRUE(r.ok());
  return r;
}

TEST(AgentStateMachine, FullLifecycleEmitsOrderedStateEventsAndCaptureDone) {
  Harness h(/*mockDurationMs=*/40, /*steps=*/4);
  Client c = h.connect();

  auto st = callOk(c, StatusRequest{});
  ASSERT_TRUE(st.ok());
  const auto* sb = std::get_if<StatusBody>(&st->body);
  ASSERT_NE(sb, nullptr);
  EXPECT_EQ(sb->state, CaptureState::Idle);
  EXPECT_EQ(sb->backend, "mock");

  auto start = callOk(c, h.startRequest("x.tachyon"));
  ASSERT_TRUE(start.ok());
  EXPECT_TRUE(start->ok) << start->error;
  EXPECT_EQ(h.agent->state(), CaptureState::Capturing);

  auto stop = callOk(c, StopRequest{});
  ASSERT_TRUE(stop.ok());
  EXPECT_TRUE(stop->ok) << stop->error;

  auto done =
      c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000);
  ASSERT_TRUE(done.ok());
  const auto& summary = std::get<CaptureDoneEvent>(*done).summary;
  EXPECT_EQ(fs::path(summary.path), h.dir.path() / "x.tachyon");
  EXPECT_EQ(summary.events_lost, 0u);
  EXPECT_GE(summary.duration_s, 0.0);

  // File is byte-identical to the fixture and opens with TraceReader.
  EXPECT_EQ(readTextFile(summary.path), readTextFile(h.fixture));
  EXPECT_TRUE(trace::TraceReader::open(summary.path).ok());

  // After the final state(idle) event the status is idle again.
  auto idle = c.waitForEvent(
      [](const Event& e) {
        const auto* s = std::get_if<StateEvent>(&e);
        return s != nullptr && s->state == CaptureState::Idle;
      },
      10000);
  ASSERT_TRUE(idle.ok());
  auto st2 = callOk(c, StatusRequest{});
  EXPECT_EQ(std::get<StatusBody>(st2->body).state, CaptureState::Idle);
}

TEST(AgentStateMachine, StateEventSequence) {
  Harness h(/*mockDurationMs=*/20, /*steps=*/2);
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.startRequest("seq.tachyon"))->ok);
  ASSERT_TRUE(callOk(c, StopRequest{})->ok);

  std::vector<CaptureState> states;
  bool sawDone = false;
  for (;;) {
    auto e = c.nextEvent(10000);
    ASSERT_TRUE(e.ok());
    if (std::holds_alternative<CaptureDoneEvent>(*e)) sawDone = true;
    if (const auto* s = std::get_if<StateEvent>(&*e)) {
      if (states.empty() || states.back() != s->state) states.push_back(s->state);
      if (s->state == CaptureState::Idle && sawDone) break;
    }
  }
  const std::vector<CaptureState> expected = {CaptureState::Starting, CaptureState::Capturing,
                                              CaptureState::Converting, CaptureState::Done,
                                              CaptureState::Idle};
  EXPECT_EQ(states, expected);
}

TEST(AgentStateMachine, SecondStartWhileBusyIsEbusy) {
  Harness h;
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.startRequest("a.tachyon"))->ok);
  auto second = callOk(c, h.startRequest("b.tachyon"));
  ASSERT_TRUE(second.ok());
  EXPECT_FALSE(second->ok);
  EXPECT_EQ(second->code, ErrorCode::Busy);
  EXPECT_EQ(protocolCode(second->code), "EBUSY");

  // A different client is also rejected.
  Client c2 = h.connect();
  auto third = callOk(c2, h.startRequest("c.tachyon"));
  EXPECT_EQ(third->code, ErrorCode::Busy);

  ASSERT_TRUE(callOk(c, StopRequest{})->ok);
  ASSERT_TRUE(
      c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000).ok());
  EXPECT_FALSE(fs::exists(h.dir.path() / "b.tachyon"));
}

TEST(AgentStateMachine, StartAgainAfterDone) {
  Harness h;
  Client c = h.connect();
  for (int i = 0; i < 2; ++i) {
    const std::string name = "run" + std::to_string(i) + ".tachyon";
    ASSERT_TRUE(callOk(c, h.startRequest(name))->ok);
    ASSERT_TRUE(callOk(c, StopRequest{})->ok);
    auto done =
        c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000);
    ASSERT_TRUE(done.ok());
    // Wait for idle before the next start (done -> idle is emitted right after capture_done).
    ASSERT_TRUE(c.waitForEvent(
                     [](const Event& e) {
                       const auto* s = std::get_if<StateEvent>(&e);
                       return s != nullptr && s->state == CaptureState::Idle;
                     },
                     10000)
                    .ok());
    EXPECT_TRUE(fs::exists(h.dir.path() / name));
  }
}

TEST(AgentStateMachine, StopWhileConvertingIsBusy) {
  Harness h(/*mockDurationMs=*/300, /*steps=*/3);
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.startRequest("slow.tachyon"))->ok);
  ASSERT_TRUE(callOk(c, StopRequest{})->ok);
  auto st = callOk(c, StatusRequest{});
  EXPECT_EQ(std::get<StatusBody>(st->body).state, CaptureState::Converting);
  auto again = callOk(c, StopRequest{});
  EXPECT_EQ(again->code, ErrorCode::Busy);
  auto startAgain = callOk(c, h.startRequest("other.tachyon"));
  EXPECT_EQ(startAgain->code, ErrorCode::Busy);
  ASSERT_TRUE(
      c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000).ok());
}

TEST(AgentStateMachine, ConvertingProgressEventsAreMonotonic) {
  Harness h(/*mockDurationMs=*/40, /*steps=*/4);
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.startRequest("p.tachyon"))->ok);
  ASSERT_TRUE(callOk(c, StopRequest{})->ok);
  double last = -1.0;
  int convertingEvents = 0;
  for (;;) {
    auto e = c.nextEvent(10000);
    ASSERT_TRUE(e.ok());
    const auto* s = std::get_if<StateEvent>(&*e);
    if (s == nullptr) continue;
    if (s->state == CaptureState::Converting) {
      EXPECT_GE(s->progress, last);
      last = s->progress;
      ++convertingEvents;
    }
    if (s->state == CaptureState::Idle) break;
  }
  EXPECT_GE(convertingEvents, 3);
}

TEST(AgentStateMachine, StopFlushCancelWhenIdleAreNotFound) {
  Harness h;
  Client c = h.connect();
  EXPECT_EQ(callOk(c, StopRequest{})->code, ErrorCode::NotFound);
  EXPECT_EQ(callOk(c, FlushRequest{"n", 1})->code, ErrorCode::NotFound);
  EXPECT_EQ(callOk(c, CancelRequest{})->code, ErrorCode::NotFound);
}

TEST(AgentStateMachine, FailedStartReturnsToIdle) {
  Harness h;
  Client c = h.connect();
  fs::remove(h.fixture);  // the mock backend cannot start without its fixture
  auto r = callOk(c, h.startRequest("f.tachyon"));
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->code, ErrorCode::NotFound);
  EXPECT_EQ(h.agent->state(), CaptureState::Idle);
  writeFixtureTrace(h.fixture);
  EXPECT_TRUE(callOk(c, h.startRequest("f.tachyon"))->ok);
}

TEST(AgentStateMachine, FlushRequiresCircularMode) {
  Harness h;
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.startRequest("plain.tachyon", 0))->ok);
  auto bad = callOk(c, FlushRequest{"hitch", 5});
  EXPECT_FALSE(bad->ok);
  EXPECT_EQ(bad->code, ErrorCode::Internal);
  ASSERT_TRUE(callOk(c, StopRequest{})->ok);
  ASSERT_TRUE(
      c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000).ok());
  ASSERT_TRUE(c.waitForEvent(
                   [](const Event& e) {
                     const auto* s = std::get_if<StateEvent>(&e);
                     return s != nullptr && s->state == CaptureState::Idle;
                   },
                   10000)
                  .ok());

  ASSERT_TRUE(callOk(c, h.startRequest("circ.tachyon", 128))->ok);
  auto good = callOk(c, FlushRequest{"hitch-frame-12345", 10});
  ASSERT_TRUE(good->ok) << good->error;
  const auto* summary = std::get_if<CaptureSummary>(&good->body);
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(fs::path(summary->path), h.dir.path() / "hitch-frame-12345.tachyon");
  EXPECT_EQ(readTextFile(summary->path), readTextFile(h.fixture));
  EXPECT_EQ(h.agent->state(), CaptureState::Capturing);
}

TEST(AgentStateMachine, CancelDiscardsOutput) {
  Harness h;
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.startRequest("cancelled.tachyon"))->ok);
  auto r = callOk(c, CancelRequest{});
  EXPECT_TRUE(r->ok) << r->error;
  EXPECT_EQ(h.agent->state(), CaptureState::Idle);
  EXPECT_FALSE(fs::exists(h.dir.path() / "cancelled.tachyon"));
  EXPECT_TRUE(callOk(c, h.startRequest("again.tachyon"))->ok);
}

TEST(AgentStateMachine, EventsBroadcastToAllClients) {
  Harness h;
  Client a = h.connect();
  Client b = h.connect();
  Client observer = h.connect();
  // The observer never sends a request; it must still see everything.
  ASSERT_TRUE(callOk(a, h.startRequest("bc.tachyon"))->ok);
  ASSERT_TRUE(callOk(b, StopRequest{})->ok);
  for (Client* c : {&a, &b, &observer}) {
    auto done =
        c->waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000);
    EXPECT_TRUE(done.ok());
  }
}

TEST(AgentStateMachine, ClientDisconnectDuringCaptureDoesNotStopIt) {
  Harness h;
  {
    Client a = h.connect();
    ASSERT_TRUE(callOk(a, h.startRequest("persist.tachyon"))->ok);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(h.agent->state(), CaptureState::Capturing);
  Client b = h.connect();
  ASSERT_TRUE(callOk(b, StopRequest{})->ok);
  EXPECT_TRUE(
      b.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 10000).ok());
}

TEST(AgentStateMachine, ShutdownStopsRunningCapture) {
  Harness h;
  Client c = h.connect();
  ASSERT_TRUE(callOk(c, h.startRequest("shutdown.tachyon"))->ok);
  h.server->stop();
  h.agent->shutdown();
  EXPECT_EQ(h.agent->state(), CaptureState::Idle);
  EXPECT_TRUE(fs::exists(h.dir.path() / "shutdown.tachyon"));
}

TEST(AgentStateMachine, InstHelloReturnsPipeName) {
  Harness h;
  Client c = h.connect();
  // WP-52: outside a capture nothing is recorded, so the SDK is told no (and uses its sidecar file).
  auto idle = callOk(c, InstHelloRequest{1234, kProtocolApiVersion});
  EXPECT_FALSE(idle->ok);
  EXPECT_EQ(idle->code, ErrorCode::NotFound);

  ASSERT_TRUE(callOk(c, h.startRequest("hello.tachyon"))->ok);
  auto r = callOk(c, InstHelloRequest{1234, kProtocolApiVersion});
  ASSERT_TRUE(r->ok) << r->error;
  const auto* body = std::get_if<InstHelloBody>(&r->body);
  ASSERT_NE(body, nullptr);
  EXPECT_NE(body->pipe.find("tachyon-inst-1234"), std::string::npos);
  auto bad = callOk(c, InstHelloRequest{1234, 7});
  EXPECT_FALSE(bad->ok);
}

TEST(AgentStateMachine, ConcurrentClientsRaceToStartExactlyOneWins) {
  Harness h;
  constexpr int kClients = 6;
  std::atomic<int> winners{0}, busy{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&, i] {
      Client c = h.connect();
      auto r = c.call(h.startRequest("race" + std::to_string(i) + ".tachyon"), 10000);
      ASSERT_TRUE(r.ok());
      if (r->ok) ++winners;
      if (!r->ok && r->code == ErrorCode::Busy) ++busy;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(busy.load(), kClients - 1);
}

}  // namespace
}  // namespace tachyon::agent
