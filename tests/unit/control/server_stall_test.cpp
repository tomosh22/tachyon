// A client that stops reading must never block the producer of an event or another client (WP-10 follow-up).
#include "control/client.h"
#include "control/server.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace tachyon::control {
namespace {

using testing_util::testAddress;
using Clock = std::chrono::steady_clock;

class StatusHandler final : public IRequestHandler {
 public:
  Response handle(ClientId, const Request&) override {
    StatusBody s;
    s.backend = "stall-test";
    return makeOkResponse(std::nullopt, s);
  }
};

bool waitUntil(const std::function<bool()>& pred, int ms = 10000) {
  const auto end = Clock::now() + std::chrono::milliseconds(ms);
  while (Clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

struct Fixture {
  StatusHandler handler;
  std::string address = testAddress("stall");
  std::unique_ptr<Server> server;

  Fixture() {
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), handler);
    server->start();
  }
};

Event bigEvent(int i) {
  return ErrorEvent{std::to_string(i) + std::string(1000, 'x')};
}

TEST(ServerStall, BroadcastDoesNotBlockOnAClientThatNeverReads) {
  Fixture f;
  auto stalled = connectTo(f.address);  // never reads
  ASSERT_TRUE(stalled.ok());
  auto healthy = Client::connect(f.address);
  ASSERT_TRUE(healthy.ok());
  ASSERT_TRUE(waitUntil([&] { return f.server->clientCount() == 2; }));

  constexpr int kEvents = 6000;  // ~6 MB: far more than the queue cap plus any socket / pipe buffer
  std::atomic<int> received{0};
  std::thread reader([&] {
    while (received.load() < kEvents) {
      auto e = healthy->nextEvent(10000);
      if (!e.ok()) break;
      ++received;
    }
  });

  const auto begin = Clock::now();
  for (int i = 0; i < kEvents; ++i) {
    f.server->broadcast(bigEvent(i));
    // Keep the healthy client's backlog small so only the stalled one can overflow.
    if (i % 100 == 99) waitUntil([&] { return received.load() >= i - 300; }, 5000);
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
  reader.join();

  EXPECT_EQ(received.load(), kEvents) << "the healthy client must get every event";
  EXPECT_LT(seconds, 20.0) << "broadcast was held up by the stalled client";
  // The stalled client was dropped once its backlog passed the cap; the healthy one is still connected.
  EXPECT_TRUE(waitUntil([&] { return f.server->clientCount() == 1; }));
  auto r = healthy->call(StatusRequest{});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->ok);
}

TEST(ServerStall, FloodingRequestsWithoutReadingDoesNotBlockOthers) {
  Fixture f;
  auto flooder = connectTo(f.address);
  ASSERT_TRUE(flooder.ok());
  std::atomic<bool> stop{false};
  std::thread flood([&] {
    const std::string line = encodeRequest(Request{1, StatusRequest{}});
    for (int i = 0; i < 400000 && !stop.load(); ++i)
      if (!(*flooder)->writeLine(line).ok()) break;
  });

  auto other = Client::connect(f.address);
  ASSERT_TRUE(other.ok());
  for (int i = 0; i < 50; ++i) {
    auto r = other->call(StatusRequest{}, 5000);
    ASSERT_TRUE(r.ok()) << "request " << i << " stalled behind the flooding client";
    EXPECT_TRUE(r->ok);
    f.server->broadcast(bigEvent(i));
  }
  stop = true;
  (*flooder)->close();
  flood.join();
}

TEST(ServerStall, ResponsesAndEventsKeepTheirOrderPerClient) {
  Fixture f;
  auto c = Client::connect(f.address);
  ASSERT_TRUE(c.ok());
  ASSERT_TRUE(waitUntil([&] { return f.server->clientCount() == 1; }));
  for (int i = 0; i < 20; ++i) f.server->broadcast(ErrorEvent{"e" + std::to_string(i)});
  for (int i = 0; i < 20; ++i) {
    auto e = c->nextEvent(5000);
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(std::get<ErrorEvent>(*e).message, "e" + std::to_string(i));
  }
}

}  // namespace
}  // namespace tachyon::control
