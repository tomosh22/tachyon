#include "control/client.h"
#include "control/server.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace tachyon::control {
namespace {

using testing_util::testAddress;

// Echoes the request id back as the status progress; counts connects/disconnects.
class EchoHandler final : public IRequestHandler {
 public:
  void onConnect(ClientId) override { ++connects; }
  void onDisconnect(ClientId) override { ++disconnects; }
  Response handle(ClientId, const Request& request) override {
    ++handled;
    if (std::holds_alternative<StopRequest>(request.body))
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy, "nope"));
    StatusBody s;
    s.progress = static_cast<double>(request.id);
    s.backend = "echo";
    return makeOkResponse(std::nullopt, s);
  }
  std::atomic<int> connects{0}, disconnects{0}, handled{0};
};

struct Fixture {
  EchoHandler handler;
  std::string address = testAddress("srv");
  std::unique_ptr<Server> server;

  Fixture() {
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), handler);
    server->start();
  }
};

bool waitUntil(const std::function<bool()>& pred, int ms = 5000) {
  for (int i = 0; i < ms / 5; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

TEST(ServerClient, RequestResponse) {
  Fixture f;
  auto c = Client::connect(f.address);
  ASSERT_TRUE(c.ok());
  auto r = c->call(StatusRequest{});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->ok);
  EXPECT_EQ(r->id, 1);
  const auto* s = std::get_if<StatusBody>(&r->body);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->backend, "echo");
  EXPECT_DOUBLE_EQ(s->progress, 1.0);
}

TEST(ServerClient, ErrorResponseCarriesCode) {
  Fixture f;
  auto c = Client::connect(f.address);
  ASSERT_TRUE(c.ok());
  auto r = c->call(StopRequest{});
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->ok);
  EXPECT_EQ(r->code, ErrorCode::Busy);
  EXPECT_EQ(r->error, "nope");
}

TEST(ServerClient, MalformedInputGetsInternalErrorAndConnectionSurvives) {
  Fixture f;
  auto raw = connectTo(f.address);
  ASSERT_TRUE(raw.ok());
  std::string line;

  ASSERT_TRUE((*raw)->writeLine("this is not json").ok());
  auto r = (*raw)->readLine(line, 5000);
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_NE(line.find("\"id\":null"), std::string::npos) << line;
  EXPECT_NE(line.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(line.find("\"code\":\"EINTERNAL\""), std::string::npos);

  ASSERT_TRUE((*raw)->writeLine(R"({"id":42,"cmd":"start"})").ok());
  r = (*raw)->readLine(line, 5000);
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_NE(line.find("\"id\":42"), std::string::npos) << line;
  EXPECT_NE(line.find("EINTERNAL"), std::string::npos);

  ASSERT_TRUE((*raw)->writeLine(R"({"id":43,"cmd":"status","extra":1})").ok());
  r = (*raw)->readLine(line, 5000);
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_NE(line.find("\"ok\":true"), std::string::npos) << line;
  EXPECT_EQ(f.handler.handled.load(), 1);
}

TEST(ServerClient, OverlongLineDisconnectsClient) {
  Fixture f;
  auto raw = connectTo(f.address);
  ASSERT_TRUE(raw.ok());
  std::thread writer([&] { (void)(*raw)->writeLine(std::string(kMaxLineBytes + 1, 'z')); });
  std::string line;
  bool sawError = false, sawEof = false;
  for (int i = 0; i < 4 && !sawEof; ++i) {
    auto r = (*raw)->readLine(line, 10000);
    if (!r.ok()) break;
    if (!*r) {
      sawEof = true;
    } else if (line.find("EINTERNAL") != std::string::npos) {
      sawError = true;
    }
  }
  (*raw)->close();
  writer.join();
  EXPECT_TRUE(sawError || sawEof);
  EXPECT_TRUE(waitUntil([&] { return f.handler.disconnects.load() == 1; }));
}

TEST(ServerClient, ConcurrentClients) {
  Fixture f;
  constexpr int kClients = 8, kRequests = 100;
  std::atomic<int> okCount{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; ++t) {
    threads.emplace_back([&] {
      auto c = Client::connect(f.address);
      ASSERT_TRUE(c.ok());
      for (int i = 1; i <= kRequests; ++i) {
        auto r = c->call(StatusRequest{});
        ASSERT_TRUE(r.ok());
        const auto* s = std::get_if<StatusBody>(&r->body);
        ASSERT_NE(s, nullptr);
        if (r->ok && r->id == i && s->progress == static_cast<double>(i)) ++okCount;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(okCount.load(), kClients * kRequests);
  EXPECT_EQ(f.handler.handled.load(), kClients * kRequests);
  EXPECT_TRUE(waitUntil([&] { return f.handler.disconnects.load() == kClients; }));
}

TEST(ServerClient, BroadcastReachesAllClients) {
  Fixture f;
  std::vector<Client> clients;
  for (int i = 0; i < 3; ++i) {
    auto c = Client::connect(f.address);
    ASSERT_TRUE(c.ok());
    clients.push_back(std::move(*c));
  }
  ASSERT_TRUE(waitUntil([&] { return f.server->clientCount() == 3; }));
  f.server->broadcast(StateEvent{capture::CaptureState::Capturing, 0.0});
  f.server->broadcast(CaptureDoneEvent{{"a.tachyon", 0, 1.0}});
  for (auto& c : clients) {
    auto e1 = c.nextEvent(5000);
    ASSERT_TRUE(e1.ok());
    EXPECT_TRUE(std::holds_alternative<StateEvent>(*e1));
    auto e2 =
        c.waitForEvent([](const Event& e) { return std::holds_alternative<CaptureDoneEvent>(e); }, 5000);
    ASSERT_TRUE(e2.ok());
  }
}

TEST(ServerClient, EventsInterleavedWithResponsesAreQueued) {
  Fixture f;
  auto c = Client::connect(f.address);
  ASSERT_TRUE(c.ok());
  ASSERT_TRUE(waitUntil([&] { return f.server->clientCount() == 1; }));
  f.server->broadcast(ErrorEvent{"before"});
  auto r = c->call(StatusRequest{});
  ASSERT_TRUE(r.ok());
  auto e = c->nextEvent(5000);
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(std::get<ErrorEvent>(*e).message, "before");
}

TEST(ServerClient, ClientsConnectAndDisconnectFreely) {
  Fixture f;
  for (int i = 0; i < 5; ++i) {
    auto c = Client::connect(f.address);
    ASSERT_TRUE(c.ok());
    ASSERT_TRUE(c->call(StatusRequest{}).ok());
    c->close();
  }
  EXPECT_TRUE(waitUntil([&] { return f.handler.disconnects.load() == 5; }));
  // Broadcast with a dead client must not crash or block.
  f.server->broadcast(ErrorEvent{"x"});
  auto c = Client::connect(f.address);
  ASSERT_TRUE(c.ok());
  EXPECT_TRUE(c->call(StatusRequest{}).ok());
}

TEST(ServerClient, StopWithConnectedClientsReturns) {
  Fixture f;
  auto c = Client::connect(f.address);
  ASSERT_TRUE(c.ok());
  ASSERT_TRUE(c->call(StatusRequest{}).ok());
  f.server->stop();
  auto r = c->call(StatusRequest{}, 2000);
  EXPECT_FALSE(r.ok());
}

TEST(ServerClient, CallTimesOutWhenNoReply) {
  // A listener nobody serves: accept the connection manually and never answer.
  auto l = listenOn(testAddress("silent"));
  ASSERT_TRUE(l.ok());
  std::unique_ptr<IConnection> held;
  std::thread t([&] {
    auto a = (*l)->accept();
    if (a.ok()) held = std::move(*a);
  });
  auto c = Client::connect((*l)->address());
  ASSERT_TRUE(c.ok());
  t.join();
  auto r = c->call(StatusRequest{}, 100);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Timeout);
}

TEST(ServerClient, ConnectWithRetryWaitsForListener) {
  const std::string addr = testAddress("late");
  EchoHandler handler;
  std::unique_ptr<Server> server;
  std::thread starter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto l = listenOn(addr);
    server = std::make_unique<Server>(std::move(*l), handler);
    server->start();
  });
  auto c = Client::connectWithRetry(addr, 5000);
  starter.join();
  ASSERT_TRUE(c.ok());
  EXPECT_TRUE(c->call(StatusRequest{}).ok());
}

}  // namespace
}  // namespace tachyon::control
