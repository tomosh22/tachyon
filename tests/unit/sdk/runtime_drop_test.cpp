// Overflow policy: records are dropped when the ring is full, every drop is counted and reported, and a
// dropped Begin takes its End with it so recorded scopes always nest.
#include "runtime_fixture.h"

#include <tachyon/tachyon.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace tachyon {
namespace {

using inst::FrameType;
using inst::test_support::ParsedStream;
using sdk_testing::RuntimeTest;

class DropTest : public RuntimeTest {};

TEST_F(DropTest, OverflowIsCountedReportedAndKeepsScopesBalanced) {
  sdk_testing::setEnv("TACHYON_RING_KB", "16");  // smallest ring
  const auto path = startSidecar();
  constexpr int kScopes = 20000;
  const std::string payload(1000, 'p');  // ~1 KiB per Begin: far faster than a 1 ms drain can keep up with
  uint64_t emitted = 0;
  for (int i = 0; i < kScopes; ++i) {
    Tachyon_BeginEvent("burst", payload.c_str(), 0);
    if (i % 5 == 0) {
      Tachyon_BeginEvent("burst-inner", nullptr, 0);
      (void)Tachyon_EndEvent();
      emitted += 2;
    }
    (void)Tachyon_EndEvent();
    emitted += 2;
  }
  const uint64_t counted = Tachyon_Internal_ThreadDropCount();
  EXPECT_GT(counted, 0u);
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();

  uint64_t begins = 0, ends = 0, depth = 0, maxDepth = 0, reported = 0;
  for (const auto& e : ps.events) {
    if (e.type == FrameType::ScopeBegin) {
      ++begins;
      maxDepth = std::max(maxDepth, ++depth);
    } else if (e.type == FrameType::ScopeEnd) {
      ++ends;
      ASSERT_GT(depth, 0u) << "End without Begin";
      --depth;
    } else if (e.type == FrameType::Dropped) {
      reported += e.number;
    }
  }
  EXPECT_EQ(depth, 0u);
  EXPECT_EQ(begins, ends);
  EXPECT_LE(maxDepth, 2u);
  EXPECT_EQ(reported, counted);                  // everything dropped was reported (final drain included)
  EXPECT_EQ(begins + ends + reported, emitted);  // exact accounting
  EXPECT_GT(begins, 0u);
}

TEST_F(DropTest, NoDropsWhenTheDrainKeepsUp) {
  sdk_testing::setEnv("TACHYON_RING_KB", "1024");  // generous: only scheduling jitter could cause drops
  const auto path = startSidecar();
  for (int i = 0; i < 2000; ++i) {
    Tachyon_BeginEvent("calm", "x", 0);
    (void)Tachyon_EndEvent();
    if (i % 100 == 99) std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  EXPECT_EQ(Tachyon_Internal_ThreadDropCount(), 0u);
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  EXPECT_EQ(ps.select(FrameType::ScopeBegin).size(), 2000u);
  EXPECT_EQ(ps.select(FrameType::ScopeEnd).size(), 2000u);
  EXPECT_TRUE(ps.select(FrameType::Dropped).empty());
}

TEST_F(DropTest, ThreadExitDrainsAndRetiresTheRing) {
  const auto path = startSidecar();
  for (int round = 0; round < 20; ++round) {
    std::thread t([] {
      Tachyon_BeginEvent("short-lived", nullptr, 0);
      (void)Tachyon_EndEvent();
    });
    t.join();
  }
  const ParsedStream ps = finishAndParse(path);
  ASSERT_TRUE(ps.errors.empty()) << ps.errors.front();
  EXPECT_EQ(ps.select(FrameType::ScopeBegin).size(), 20u);
  EXPECT_EQ(ps.select(FrameType::ScopeEnd).size(), 20u);
}

}  // namespace
}  // namespace tachyon
