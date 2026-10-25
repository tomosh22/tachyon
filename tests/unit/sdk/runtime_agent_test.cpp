// Runtime behaviour against an in-test fake agent: control-channel handshake, inst stream, flush request,
// fallback to the sidecar and reconnection.
#include "runtime_fixture.h"

#include <tachyon/tachyon.h>

#include <chrono>
#include <functional>
#include <thread>

namespace tachyon {
namespace {

using inst::FrameType;
using inst::test_support::ParsedStream;
using sdk_testing::FakeAgent;
using sdk_testing::RuntimeTest;

class AgentTest : public RuntimeTest {
 protected:
  // Points the SDK at `address` with the sidecar in the temp dir; returns the sidecar path.
  std::filesystem::path configure(const std::string& address) {
    sdk_testing::setEnv("TACHYON_AGENT_PIPE", address);
    const auto sidecar = m_dir / "fallback.tachyon-inst";
    sdk_testing::setEnv("TACHYON_SIDECAR", sidecar.string());
    return sidecar;
  }
};

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

TEST_F(AgentTest, HandshakeThenStreamsToTheInstPipe) {
  FakeAgent agent(sdk_testing::uniqueAgentAddress("hs"));
  ASSERT_TRUE(agent.ok());
  const auto sidecar = configure(agent.address());
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 1);
  EXPECT_EQ(Tachyon_IsConnected(), 1);
  EXPECT_EQ(agent.helloCount(), 1);
  EXPECT_EQ(agent.helloPid(), sdk_testing::currentPidValue());
  EXPECT_EQ(agent.helloApiVersion(), static_cast<uint32_t>(TACHYON_API_VERSION));

  Tachyon_SetCurrentThreadName("agent-main");
  for (int i = 0; i < 100; ++i) {
    Tachyon_BeginEvent("agent-scope", "d", 0);
    (void)Tachyon_EndEvent();
  }
  Tachyon_FrameMark("agent-frame", 1);
  Tachyon_Internal_Flush();
  ASSERT_TRUE(agent.waitForBytes(100 * 60, 5000));
  Tachyon_Internal_Restart();  // exit-time shutdown: everything still buffered is drained first
  ASSERT_TRUE(agent.waitForInstClosed(1, 5000));

  const ParsedStream ps = ParsedStream::parse(agent.bytes());
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  EXPECT_EQ(+ps.header.pid, sdk_testing::currentPidValue());
  EXPECT_EQ(ps.select(FrameType::ScopeBegin).size(), 100u);
  EXPECT_EQ(ps.select(FrameType::ScopeEnd).size(), 100u);
  EXPECT_EQ(ps.select(FrameType::FrameMark).size(), 1u);
  EXPECT_EQ(Tachyon_IsConnected(), 0);             // runtime is shut down
  EXPECT_FALSE(std::filesystem::exists(sidecar));  // the sidecar is only a fallback
}

TEST_F(AgentTest, RequestFlushIsForwardedAndSynchronous) {
  FakeAgent agent(sdk_testing::uniqueAgentAddress("fl"));
  ASSERT_TRUE(agent.ok());
  configure(agent.address());
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 1);

  EXPECT_EQ(Tachyon_RequestFlush("hitch-frame-12345"), 1);
  EXPECT_EQ(agent.flushCount(), 1);  // synchronous: the agent has already handled it
  EXPECT_EQ(agent.lastFlushName(), "hitch-frame-12345");
  EXPECT_EQ(agent.lastFlushSeconds(), 10u);

  // Special characters survive JSON escaping.
  EXPECT_EQ(Tachyon_RequestFlush("quote\" back\\slash\ttab"), 1);
  EXPECT_EQ(agent.lastFlushName(), "quote\" back\\slash\ttab");

  // Unsolicited events on the control channel do not confuse the reply matching.
  agent.broadcastEvent();
  agent.broadcastEvent();
  EXPECT_EQ(Tachyon_RequestFlush("after-events"), 1);

  // The agent answering with an error means "not flushed".
  agent.setFlushOk(false);
  EXPECT_EQ(Tachyon_RequestFlush("idle-agent"), 0);
  EXPECT_EQ(agent.flushCount(), 4);
  agent.setFlushOk(true);
  EXPECT_EQ(Tachyon_RequestFlush(nullptr), 1);
}

TEST_F(AgentTest, RequestFlushWithoutAnAgentFails) {
  startSidecar();
  EXPECT_EQ(Tachyon_RequestFlush("nobody-home"), 0);
  EXPECT_EQ(Tachyon_IsConnected(), 0);
}

TEST_F(AgentTest, FallsBackToSidecarThenSwitchesWhenTheAgentAppears) {
  const std::string address = sdk_testing::uniqueAgentAddress("late");
  const auto sidecar = configure(address);
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 2);
  EXPECT_EQ(Tachyon_IsConnected(), 0);
  Tachyon_BeginEvent("before-agent", "b", 0);
  (void)Tachyon_EndEvent();
  Tachyon_Internal_Flush();

  FakeAgent agent(address);
  ASSERT_TRUE(agent.ok());
  ASSERT_TRUE(waitFor([] { return Tachyon_IsConnected() == 1; }, 8000))
      << "no reconnect within the retry window";
  Tachyon_BeginEvent("after-agent", "a", 0);
  (void)Tachyon_EndEvent();
  Tachyon_Internal_Restart();
  ASSERT_TRUE(agent.waitForInstClosed(1, 5000));

  const ParsedStream file = ParsedStream::parse(sdk_testing::readFile(sidecar));
  ASSERT_TRUE(file.errors.empty()) << file.errors.front();
  ASSERT_EQ(file.select(FrameType::ScopeBegin).size(), 1u);
  EXPECT_EQ(file.select(FrameType::ScopeBegin)[0]->id, "before-agent");

  // The agent stream is a complete stream of its own: header, re-sent string definitions, then new data.
  const ParsedStream live = ParsedStream::parse(agent.bytes());
  ASSERT_TRUE(live.errors.empty()) << live.errors.front();
  ASSERT_TRUE(live.headerSeen);
  bool sawAfter = false;
  for (const auto* b : live.select(FrameType::ScopeBegin))
    if (b->id == "after-agent") sawAfter = true;
  EXPECT_TRUE(sawAfter);
  bool sawBeforeString = false;
  for (const auto& kv : live.strings)
    if (kv.second == "before-agent") sawBeforeString = true;
  EXPECT_TRUE(sawBeforeString);
}

TEST_F(AgentTest, AgentLossFallsBackToSidecarAndReconnects) {
  FakeAgent agent(sdk_testing::uniqueAgentAddress("loss"));
  ASSERT_TRUE(agent.ok());
  const auto sidecar = configure(agent.address());
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 1);
  Tachyon_BeginEvent("first", nullptr, 0);
  (void)Tachyon_EndEvent();
  Tachyon_Internal_Flush();

  ASSERT_TRUE(waitFor([&] { return agent.instConnections() >= 1; }, 5000));
  agent.dropInstConnections();
  // Keep producing so the next write notices the broken pipe.
  ASSERT_TRUE(waitFor(
      [] {
        Tachyon_BeginEvent("during-loss", nullptr, 0);
        (void)Tachyon_EndEvent();
        return Tachyon_IsConnected() == 0;
      },
      5000));
  // The agent is still listening, so the periodic retry connects again.
  ASSERT_TRUE(waitFor([&] { return agent.instConnections() >= 2; }, 8000));
  Tachyon_Internal_Restart();

  const ParsedStream file = ParsedStream::parse(sdk_testing::readFile(sidecar));
  EXPECT_TRUE(file.errors.empty()) << (file.errors.empty() ? "" : file.errors.front());
  EXPECT_TRUE(file.headerSeen);
}

TEST_F(AgentTest, ReopenedSidecarDoesNotTruncateTheFirstPhase) {
  const std::string address = sdk_testing::uniqueAgentAddress("reopen");
  const auto sidecar = configure(address);
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 2);
  Tachyon_BeginEvent("phase1", nullptr, 0);
  (void)Tachyon_EndEvent();
  Tachyon_Internal_Flush();

  FakeAgent agent(address);
  ASSERT_TRUE(agent.ok());
  ASSERT_TRUE(waitFor([] { return Tachyon_IsConnected() == 1; }, 8000));
  ASSERT_TRUE(waitFor([&] { return agent.instConnections() >= 1; }, 5000));
  agent.dropInstConnections();
  const std::string first = sidecar.string();
  ASSERT_TRUE(waitFor(
      [&] {
        Tachyon_BeginEvent("phase3", nullptr, 0);
        (void)Tachyon_EndEvent();
        return std::string(Tachyon_Internal_SidecarPath()) != first;
      },
      8000));
  const std::filesystem::path second = Tachyon_Internal_SidecarPath();
  EXPECT_EQ(second.filename().string(), "fallback.1.tachyon-inst");
  Tachyon_Internal_Restart();

  const ParsedStream a = ParsedStream::parse(sdk_testing::readFile(sidecar));
  ASSERT_TRUE(a.errors.empty()) << a.errors.front();
  ASSERT_EQ(a.select(FrameType::ScopeBegin).size(), 1u);
  EXPECT_EQ(a.select(FrameType::ScopeBegin)[0]->id, "phase1");
  const ParsedStream b = ParsedStream::parse(sdk_testing::readFile(second));
  EXPECT_TRUE(b.errors.empty()) << (b.errors.empty() ? "" : b.errors.front());
  EXPECT_TRUE(b.headerSeen);
}

TEST_F(AgentTest, UnsolicitedControlEventsAreDrainedWhileStreaming) {
  FakeAgent agent(sdk_testing::uniqueAgentAddress("ev"));
  ASSERT_TRUE(agent.ok());
  configure(agent.address());
  Tachyon_Internal_Restart();
  ASSERT_EQ(Tachyon_Internal_WaitReady(5000), 1);
  for (int i = 0; i < 2000; ++i) agent.broadcastEvent();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // several pump intervals
  EXPECT_EQ(Tachyon_IsConnected(), 1);
  EXPECT_EQ(Tachyon_RequestFlush("still-alive"), 1);
}

}  // namespace
}  // namespace tachyon
