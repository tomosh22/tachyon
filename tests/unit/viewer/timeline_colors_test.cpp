// WP-42: colour mapping of the threads timeline (state / wait category colours, band blending, hashing).
#include "viewer/timeline/timeline_colors.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace tachyon;
using namespace tachyon::viewer::timeline;

TEST(TimelineColors, PackingMatchesImGuiLayout) {
  const Color32 c = makeColor(1, 2, 3, 4);
  EXPECT_EQ(c, 0x04030201u);
  EXPECT_EQ(colorR(c), 1);
  EXPECT_EQ(colorG(c), 2);
  EXPECT_EQ(colorB(c), 3);
  EXPECT_EQ(colorA(c), 4);
  EXPECT_EQ(withAlpha(c, 200), makeColor(1, 2, 3, 200));
}

TEST(TimelineColors, WaitCategoriesMapToTheSpecifiedHues) {
  using analysis::WaitCategory;
  EXPECT_EQ(waitCategoryColor(WaitCategory::Synchronization), kSyncWaitColor);
  EXPECT_EQ(waitCategoryColor(WaitCategory::IO), kIoWaitColor);
  EXPECT_EQ(waitCategoryColor(WaitCategory::Memory), kMemoryWaitColor);
  EXPECT_EQ(waitCategoryColor(WaitCategory::Preempted), kReadyColor);
  EXPECT_EQ(waitCategoryColor(WaitCategory::Other), kOtherWaitColor);
  EXPECT_EQ(waitCategoryColorFromByte(analysis::kNoWaitCategory), kOtherWaitColor);
  EXPECT_EQ(waitCategoryColorFromByte(2), kMemoryWaitColor);
  // Red is red, dark blue is darker than light blue, amber is yellowish.
  EXPECT_GT(colorR(kSyncWaitColor), colorB(kSyncWaitColor) + 80);
  EXPECT_GT(colorB(kIoWaitColor), colorR(kIoWaitColor) + 80);
  EXPECT_GT(colorB(kMemoryWaitColor), 200);
  EXPECT_GT(colorG(kMemoryWaitColor), colorG(kIoWaitColor));
  EXPECT_GT(colorG(kRunningColor), colorR(kRunningColor) + 80);
  EXPECT_GT(colorR(kReadyColor), 200);
  EXPECT_LT(colorB(kReadyColor), 80);
}

TEST(TimelineColors, StateColorUsesThePlatformWaitTable) {
  model::ThreadStateInterval running;
  running.state = model::ThreadState::Running;
  EXPECT_EQ(stateColor(1, running), kRunningColor);
  model::ThreadStateInterval ready;
  ready.state = model::ThreadState::Ready;
  EXPECT_EQ(stateColor(1, ready), kReadyColor);
  model::ThreadStateInterval unknown;
  EXPECT_EQ(stateColor(1, unknown), kUnknownColor);

  model::ThreadStateInterval wait;
  wait.state = model::ThreadState::Waiting;
  wait.wait_reason = 6;  // Windows UserRequest: synchronization
  EXPECT_EQ(stateColor(1, wait), kSyncWaitColor);
  wait.wait_reason = 2;  // Windows PageIn: memory
  EXPECT_EQ(stateColor(1, wait), kMemoryWaitColor);
  wait.wait_reason = 0x02;  // Linux D: I/O
  EXPECT_EQ(stateColor(2, wait), kIoWaitColor);
  wait.wait_reason = 0x01;  // Linux S: synchronization
  EXPECT_EQ(stateColor(2, wait), kSyncWaitColor);
}

TEST(TimelineColors, BandColumnBlendsByFraction) {
  analysis::BandColumn pureRun;
  pureRun.running = 1;
  pureRun.unknown = 0;
  const Color32 run = bandColumnColor(pureRun);
  EXPECT_NEAR(colorG(run), colorG(kRunningColor), 16);
  EXPECT_NEAR(colorR(run), colorR(kRunningColor), 16);

  analysis::BandColumn pureWait;
  pureWait.waiting = 1;
  pureWait.unknown = 0;
  pureWait.waitCategory = static_cast<uint8_t>(analysis::WaitCategory::Synchronization);
  const Color32 wait = bandColumnColor(pureWait);
  EXPECT_NEAR(colorR(wait), colorR(kSyncWaitColor), 16);
  EXPECT_NEAR(colorB(wait), colorB(kSyncWaitColor), 16);

  analysis::BandColumn half;
  half.running = 0.5f;
  half.waiting = 0.5f;
  half.unknown = 0;
  half.waitCategory = static_cast<uint8_t>(analysis::WaitCategory::IO);
  const Color32 mix = bandColumnColor(half);
  EXPECT_NEAR(colorG(mix), (colorG(kRunningColor) + colorG(kIoWaitColor)) / 2, 17);

  const analysis::BandColumn nothing;  // unknown = 1
  EXPECT_NEAR(colorR(bandColumnColor(nothing)), colorR(kUnknownColor), 16);

  analysis::BandColumn zero;
  zero.unknown = 0;
  EXPECT_EQ(bandColumnColor(zero), kUnknownColor);
}

TEST(TimelineColors, BandColorsAreQuantisedSoNeighboursMerge) {
  analysis::BandColumn a, b;
  a.running = 0.800f;
  a.unknown = 0.200f;
  b.running = 0.802f;
  b.unknown = 0.198f;
  EXPECT_EQ(bandColumnColor(a), bandColumnColor(b));
  EXPECT_EQ(colorR(bandColumnColor(a)) % 16, 0);
  EXPECT_EQ(colorA(bandColumnColor(a)), 255);
}

TEST(TimelineColors, HashColorIsStableAndSpreads) {
  EXPECT_EQ(hashColor(42), hashColor(42));
  std::set<Color32> distinct;
  for (uint64_t k = 0; k < 64; ++k) distinct.insert(hashColor(k));
  EXPECT_GE(distinct.size(), 60u);
  for (uint64_t k = 0; k < 200; ++k) {
    const Color32 c = hashColor(k);
    EXPECT_EQ(colorA(c), 255);
    // Never too dark to see or so close to white that the text colour logic is meaningless.
    EXPECT_GT(std::max({colorR(c), colorG(c), colorB(c)}), 150);
  }
}

TEST(TimelineColors, CallstackColorFollowsTheLeafFrame) {
  model::TraceModel m;
  m.nodes.resize(4);  // 0 = root
  m.frames.resize(3);
  m.frames[0] = {ModuleIdx(0), 0x100};
  m.frames[1] = {ModuleIdx(0), 0x200};
  m.frames[2] = {ModuleIdx(1), 0x100};
  m.nodes[1] = {CallstackIdx(0), FrameIdx(0)};
  m.nodes[2] = {CallstackIdx(1), FrameIdx(1)};
  m.nodes[3] = {CallstackIdx(1), FrameIdx(2)};
  const auto byModule = [&](uint32_t n) {
    return callstackColor(m, CallstackIdx(n), FrameColorMode::ByModule);
  };
  const auto byFunc = [&](uint32_t n) {
    return callstackColor(m, CallstackIdx(n), FrameColorMode::ByFunction);
  };
  EXPECT_EQ(byModule(1), byModule(2));  // same module
  EXPECT_NE(byModule(1), byModule(3));  // different module
  EXPECT_NE(byFunc(1), byFunc(2));      // same module, different function
  EXPECT_NE(byFunc(1), byFunc(3));
  EXPECT_EQ(callstackColor(m, CallstackIdx(), FrameColorMode::ByModule), kUnknownColor);
  EXPECT_EQ(callstackColor(m, CallstackIdx(99), FrameColorMode::ByModule), kUnknownColor);
}

TEST(TimelineColors, StripColumnFadesWithShareAndCoverage) {
  model::TraceModel m;
  m.nodes.resize(2);
  m.frames.resize(1);
  m.frames[0] = {ModuleIdx(3), 0x100};
  m.nodes[1] = {CallstackIdx(0), FrameIdx(0)};

  analysis::StripColumn full;
  full.stack = CallstackIdx(1);
  full.share = 1;
  full.coverage = 1;
  const Color32 a = stripColumnColor(m, full, FrameColorMode::ByModule);
  EXPECT_GE(colorA(a), 0xE0);

  analysis::StripColumn thin = full;
  thin.coverage = 0.1f;
  EXPECT_LT(colorA(stripColumnColor(m, thin, FrameColorMode::ByModule)), colorA(a));

  analysis::StripColumn none;  // invalid stack
  EXPECT_EQ(colorA(stripColumnColor(m, none, FrameColorMode::ByModule)), 0);
  analysis::StripColumn uncovered = full;
  uncovered.coverage = 0;
  EXPECT_EQ(colorA(stripColumnColor(m, uncovered, FrameColorMode::ByModule)), 0);

  analysis::StripColumn mixed = full;
  mixed.share = 0.2f;
  EXPECT_NE(stripColumnColor(m, mixed, FrameColorMode::ByModule), a);
}

TEST(TimelineColors, TextColorContrasts) {
  EXPECT_EQ(textColorFor(makeColor(250, 250, 250)), makeColor(10, 10, 12));
  EXPECT_EQ(textColorFor(makeColor(10, 10, 40)), makeColor(245, 245, 245));
  const Color32 half = scaleBrightness(makeColor(100, 200, 50, 77), 0.5f);
  EXPECT_EQ(half, makeColor(50, 100, 25, 77));
  EXPECT_EQ(scaleBrightness(makeColor(200, 200, 200), 5.0f), makeColor(255, 255, 255));
}
