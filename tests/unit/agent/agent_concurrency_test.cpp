// WP-10 follow-ups: the agent mutex is never held across a backend call, and a stalled client cannot wedge
// state changes made by another client.
#include "../control/test_util.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "control/client.h"
#include "fixture_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace tachyon::agent {
namespace {

namespace fs = std::filesystem;
using capture::CaptureState;
using control::Client;
using control::testing_util::TempDir;
using control::testing_util::testAddress;
using namespace control;
using Clock = std::chrono::steady_clock;

// A backend whose flush() blocks until released and reports progress from inside the call.
class BlockingFlushBackend final : public capture::ICaptureBackend {
 public:
  Result<void> checkPrivileges() override { return ok(); }
  Result<void> start(const capture::CaptureOptions&) override {
    m_state = CaptureState::Capturing;
    return ok();
  }
  Result<capture::CaptureResult> stop() override {
    ++stops;
    m_state = CaptureState::Done;
    capture::CaptureResult r;
    r.tracePath = "stopped.tachyon";
    return r;
  }
  Result<capture::CaptureResult> flush(std::string name, uint32_t) override {
    {
      std::unique_lock lock(m_mutex);
      ++flushesEntered;
      m_entered.notify_all();
    }
    if (m_callback) m_callback(CaptureState::Converting, 0.5f);  // progress from inside the call
    std::unique_lock lock(m_mutex);
    m_released.wait(lock, [&] { return m_open; });
    capture::CaptureResult r;
    r.tracePath = name + ".tachyon";
    return r;
  }
  CaptureState state() const override { return m_state; }
  void setProgressCallback(std::function<void(CaptureState, float)> cb) override {
    m_callback = std::move(cb);
  }

  bool waitEntered(int ms) {
    std::unique_lock lock(m_mutex);
    return m_entered.wait_for(lock, std::chrono::milliseconds(ms), [&] { return flushesEntered.load() > 0; });
  }
  void release() {
    {
      std::lock_guard lock(m_mutex);
      m_open = true;
    }
    m_released.notify_all();
  }

  std::atomic<int> flushesEntered{0};
  std::atomic<int> stops{0};

 private:
  std::mutex m_mutex;
  std::condition_variable m_entered, m_released;
  bool m_open = false;
  std::atomic<CaptureState> m_state{CaptureState::Idle};
  std::function<void(CaptureState, float)> m_callback;
};

struct Harness {
  TempDir dir;
  std::string address = testAddress("agentc");
  std::unique_ptr<Agent> agent;
  std::unique_ptr<Server> server;
  BlockingFlushBackend* blocking = nullptr;

  explicit Harness(std::unique_ptr<capture::ICaptureBackend> backend) {
    AgentConfig ac;
    ac.backendName = "test";
    ac.agentAddress = address;
    agent = std::make_unique<Agent>(std::move(backend), ac);
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), *agent);
    agent->setEventSink([this](const Event& e) { server->broadcast(e); });
    server->start();
  }
  ~Harness() {
    if (blocking != nullptr) blocking->release();
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

double secondsSince(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

TEST(AgentConcurrency, OtherClientsAreServedWhileAFlushIsInFlight) {
  auto backend = std::make_unique<BlockingFlushBackend>();
  BlockingFlushBackend* blocking = backend.get();
  Harness h(std::move(backend));
  h.blocking = blocking;

  Client a = h.connect();
  Client b = h.connect();
  auto start = a.call(h.startRequest("flush.tachyon", 64));
  ASSERT_TRUE(start.ok() && start->ok);

  std::atomic<bool> flushOk{false};
  std::thread flusher([&] {
    FlushRequest f;
    f.name = "hitch";
    f.seconds = 5;
    auto r = a.call(f, 20000);
    flushOk = r.ok() && r->ok;
  });
  ASSERT_TRUE(blocking->waitEntered(5000));

  // With the mutex held across flush() every one of these would hang until the flush returned.
  const auto t0 = Clock::now();
  auto status = b.call(StatusRequest{}, 3000);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(std::get<StatusBody>(status->body).state, CaptureState::Capturing);
  auto second = b.call(h.startRequest("again.tachyon"), 3000);
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(second->code, ErrorCode::Busy);
  FlushRequest other;
  other.name = "second";
  auto secondFlush = b.call(other, 3000);
  ASSERT_TRUE(secondFlush.ok());
  EXPECT_EQ(secondFlush->code, ErrorCode::Busy);  // only one flush at a time
  EXPECT_LT(secondsSince(t0), 2.0);
  EXPECT_FALSE(flushOk.load());  // still blocked

  blocking->release();
  flusher.join();
  EXPECT_TRUE(flushOk.load());
  auto stop = b.call(StopRequest{}, 5000);
  ASSERT_TRUE(stop.ok());
  EXPECT_TRUE(stop->ok);
}

TEST(AgentConcurrency, StopWaitsForAFlushInsteadOfInterleavingWithIt) {
  auto backend = std::make_unique<BlockingFlushBackend>();
  BlockingFlushBackend* blocking = backend.get();
  Harness h(std::move(backend));
  h.blocking = blocking;

  Client a = h.connect();
  Client b = h.connect();
  ASSERT_TRUE(a.call(h.startRequest("flush2.tachyon", 64))->ok);
  std::thread flusher([&] {
    FlushRequest f;
    f.name = "hitch";
    (void)a.call(f, 20000);
  });
  ASSERT_TRUE(blocking->waitEntered(5000));

  std::atomic<bool> stopped{false};
  std::thread stopper([&] {
    auto r = b.call(StopRequest{}, 20000);
    stopped = r.ok() && r->ok;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(blocking->stops.load(), 0) << "stop() reached the backend while a flush was running";
  blocking->release();
  flusher.join();
  stopper.join();
  EXPECT_TRUE(stopped.load());
  // The backend stop runs on the conversion worker after the request was acknowledged.
  for (int i = 0; i < 500 && blocking->stops.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(blocking->stops.load(), 1);
}

// ---- stalled client ---------------------------------------------------------------------------------------

TEST(AgentConcurrency, AClientThatFloodsRequestsAndNeverReadsDoesNotBlockStartAndStop) {
  TempDir fixtureDir;
  const fs::path fixture = testing_util::writeFixtureTrace(fixtureDir.path() / "fixture.tachyon");
  capture::MockBackendConfig cfg;
  cfg.fixture = fixture;
  cfg.captureDuration = std::chrono::milliseconds(60);
  cfg.progressSteps = 20;  // lots of state events for the stalled client's backlog
  Harness h(std::make_unique<capture::MockBackend>(cfg));

  // The stalled client: floods status requests and never reads a byte.
  auto flooder = connectTo(h.address);
  ASSERT_TRUE(flooder.ok());
  std::atomic<bool> stopFlood{false};
  std::thread flood([&] {
    const std::string line = encodeRequest(Request{1, StatusRequest{}});
    for (int i = 0; i < 1'000'000 && !stopFlood.load(); ++i)
      if (!(*flooder)->writeLine(line).ok()) break;
  });

  Client c = h.connect();
  for (int round = 0; round < 3; ++round) {
    const auto t0 = Clock::now();
    auto start = c.call(h.startRequest("stall" + std::to_string(round) + ".tachyon"), 8000);
    ASSERT_TRUE(start.ok()) << "start stalled in round " << round;
    ASSERT_TRUE(start->ok) << start->error;
    auto stop = c.call(StopRequest{}, 8000);
    ASSERT_TRUE(stop.ok()) << "stop stalled in round " << round;
    ASSERT_TRUE(stop->ok);
    ASSERT_TRUE(
        c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 8000)
            .ok());
    ASSERT_TRUE(c.waitForEvent(
                     [](const Event& e) {
                       const auto* s = std::get_if<StateEvent>(&e);
                       return s != nullptr && s->state == CaptureState::Idle;
                     },
                     8000)
                    .ok());
    EXPECT_LT(secondsSince(t0), 6.0);
    EXPECT_TRUE(fs::exists(h.dir.path() / ("stall" + std::to_string(round) + ".tachyon")));
  }
  stopFlood = true;
  (*flooder)->close();
  flood.join();
}

}  // namespace
}  // namespace tachyon::agent
