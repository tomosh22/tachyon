#include "control/transport.h"
#include "control/pipe_names.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <thread>

namespace tachyon::control {
namespace {

using testing_util::testAddress;

struct Pair {
  std::unique_ptr<IListener> listener;
  std::unique_ptr<IConnection> client;
  std::unique_ptr<IConnection> server;
};

Pair makePair(const std::string& tag) {
  Pair p;
  const std::string addr = testAddress(tag);
  auto l = listenOn(addr);
  EXPECT_TRUE(l.ok()) << (l.ok() ? "" : l.error().toString());
  if (!l.ok()) return p;
  p.listener = std::move(*l);
  std::unique_ptr<IConnection> accepted;
  std::thread t([&] {
    auto a = p.listener->accept();
    if (a.ok()) accepted = std::move(*a);
  });
  auto c = connectTo(addr);
  EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().toString());
  t.join();
  if (c.ok()) p.client = std::move(*c);
  p.server = std::move(accepted);
  return p;
}

TEST(Transport, LineRoundTripBothDirections) {
  Pair p = makePair("rt");
  ASSERT_TRUE(p.client && p.server);
  ASSERT_TRUE(p.client->writeLine(R"({"id":1})").ok());
  std::string line;
  auto r = p.server->readLine(line, 5000);
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_EQ(line, R"({"id":1})");
  ASSERT_TRUE(p.server->writeLine("reply").ok());
  r = p.client->readLine(line, 5000);
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_EQ(line, "reply");
}

TEST(Transport, ManyLinesInOneBurstStaySeparate) {
  Pair p = makePair("burst");
  ASSERT_TRUE(p.client && p.server);
  for (int i = 0; i < 200; ++i) ASSERT_TRUE(p.client->writeLine("line" + std::to_string(i)).ok());
  for (int i = 0; i < 200; ++i) {
    std::string line;
    auto r = p.server->readLine(line, 5000);
    ASSERT_TRUE(r.ok() && *r);
    EXPECT_EQ(line, "line" + std::to_string(i));
  }
}

TEST(Transport, CrLfIsStripped) {
  Pair p = makePair("crlf");
  ASSERT_TRUE(p.client && p.server);
  ASSERT_TRUE(p.client->writeLine("abc\r").ok());
  std::string line;
  auto r = p.server->readLine(line, 5000);
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_EQ(line, "abc");
}

TEST(Transport, ReadTimeoutThenDataStillArrives) {
  Pair p = makePair("timeout");
  ASSERT_TRUE(p.client && p.server);
  std::string line;
  auto r = p.server->readLine(line, 50);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Timeout);
  ASSERT_TRUE(p.client->writeLine("late").ok());
  r = p.server->readLine(line, 5000);
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_EQ(line, "late");
}

TEST(Transport, PeerCloseGivesEof) {
  Pair p = makePair("eof");
  ASSERT_TRUE(p.client && p.server);
  p.client.reset();
  std::string line;
  auto r = p.server->readLine(line, 5000);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(*r);
}

TEST(Transport, LocalCloseUnblocksReader) {
  Pair p = makePair("unblock");
  ASSERT_TRUE(p.client && p.server);
  std::atomic<bool> returned{false};
  std::thread t([&] {
    std::string line;
    auto r = p.server->readLine(line, -1);
    returned = true;
    EXPECT_TRUE(!r.ok() || !*r);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  p.server->close();
  t.join();
  EXPECT_TRUE(returned);
}

TEST(Transport, OverlongLineIsRejected) {
  Pair p = makePair("long");
  ASSERT_TRUE(p.client && p.server);
  // One line longer than the limit; the reader must reject it (the write may fail once it disconnects).
  std::thread writer([&] { (void)p.client->writeLine(std::string(kMaxLineBytes + 1, 'x')); });
  std::string line;
  auto r = p.server->readLine(line, 10000);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  p.server->close();
  p.client->close();
  writer.join();
}

TEST(Transport, MaxSizeLineIsAccepted) {
  Pair p = makePair("max");
  ASSERT_TRUE(p.client && p.server);
  const std::string big(kMaxLineBytes, 'y');
  std::thread writer([&] { EXPECT_TRUE(p.client->writeLine(big).ok()); });
  std::string line;
  auto r = p.server->readLine(line, 10000);
  writer.join();
  ASSERT_TRUE(r.ok() && *r);
  EXPECT_EQ(line.size(), kMaxLineBytes);
}

TEST(Transport, ListenerCloseCancelsAccept) {
  auto l = listenOn(testAddress("cancel"));
  ASSERT_TRUE(l.ok());
  std::thread t([&] {
    auto a = (*l)->accept();
    ASSERT_FALSE(a.ok());
    EXPECT_EQ(a.error().code, ErrorCode::Cancelled);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  (*l)->close();
  t.join();
}

TEST(Transport, SecondListenerOnSameAddressIsBusy) {
  const std::string addr = testAddress("busy");
  auto first = listenOn(addr);
  ASSERT_TRUE(first.ok());
  auto second = listenOn(addr);
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.error().code, ErrorCode::Busy);
}

TEST(Transport, ConnectToMissingFails) {
  auto c = connectTo(testAddress("missing"));
  EXPECT_FALSE(c.ok());
}

TEST(Transport, InstrumentationAddressIncludesPid) {
  EXPECT_NE(instrumentationAddress(testAddress("inst"), 4321).find("tachyon-inst-4321"), std::string::npos);
}

// WP-66b: pure string function, so it is checked on every platform (it is what Windows uses).
TEST(PipeNames, TheDefaultAgentPipeKeepsTheMachineWideInstrumentationName) {
  EXPECT_EQ(windowsInstrumentationPipeName(R"(\\.\pipe\tachyon-agent)", 4321),
            R"(\\.\pipe\tachyon-inst-4321)");
  EXPECT_EQ(windowsInstrumentationPipeName(R"(\\.\PIPE\Tachyon-Agent)", 7), R"(\\.\pipe\tachyon-inst-7)")
      << "pipe names are case-insensitive";
  EXPECT_EQ(windowsInstrumentationPipeName("", 7), R"(\\.\pipe\tachyon-inst-7)");
}

TEST(PipeNames, APrivateAgentPipeGetsItsOwnInstrumentationNamespace) {
  const std::string a = windowsInstrumentationPipeName(R"(\\.\pipe\tachyon-test-111-a)", 1234);
  const std::string b = windowsInstrumentationPipeName(R"(\\.\pipe\tachyon-test-222-b)", 1234);
  EXPECT_EQ(a, R"(\\.\pipe\tachyon-test-111-a-tachyon-inst-1234)");
  EXPECT_NE(a, b) << "two test processes with the same pid in the hello do not collide";
  EXPECT_NE(a, windowsInstrumentationPipeName(R"(\\.\pipe\tachyon-agent)", 1234));
  EXPECT_NE(windowsInstrumentationPipeName(R"(\\.\pipe\x)", 1),
            windowsInstrumentationPipeName(R"(\\.\pipe\x)", 2));
  // Longer or shorter than the default name, sharing its prefix, is not the default.
  EXPECT_NE(windowsInstrumentationPipeName(R"(\\.\pipe\tachyon-agent2)", 5), R"(\\.\pipe\tachyon-inst-5)");
  EXPECT_NE(windowsInstrumentationPipeName(R"(\\.\pipe\tachyon-agen)", 5), R"(\\.\pipe\tachyon-inst-5)");
}

TEST(PipeNames, ALongAgentPipeFallsBackToAHashOfTheAddress) {
  const std::string longPipe = R"(\\.\pipe\)" + std::string(300, 'a');
  const std::string name = windowsInstrumentationPipeName(longPipe, 99);
  EXPECT_LE(name.size(), kMaxWindowsPipeNameChars);
  EXPECT_EQ(name.rfind(R"(\\.\pipe\tachyon-inst-)", 0), 0u);
  EXPECT_EQ(name.substr(name.size() - 3), "-99");
  EXPECT_EQ(name, windowsInstrumentationPipeName(longPipe, 99)) << "deterministic";
  EXPECT_NE(name, windowsInstrumentationPipeName(longPipe, 100));
  EXPECT_NE(name, windowsInstrumentationPipeName(R"(\\.\pipe\)" + std::string(301, 'a'), 99));
  // Case-folded like the pipe namespace.
  EXPECT_EQ(name, windowsInstrumentationPipeName(R"(\\.\PIPE\)" + std::string(300, 'A'), 99));
  // Just under the limit keeps the readable form.
  const std::string fits = R"(\\.\pipe\)" + std::string(200, 'a');
  EXPECT_EQ(windowsInstrumentationPipeName(fits, 5), fits + "-tachyon-inst-5");
}

}  // namespace
}  // namespace tachyon::control
