// WP-42: colour mapping of the threads timeline (state / wait category colours, band blending, hashing).
#include "viewer/timeline/timeline_colors.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;

namespace {
// The dark palette's state colours (the light ones are covered by theme_test.cpp).
const StatePalette& P = paletteFor(ColorScheme::Dark).states;
}  // namespace

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
  EXPECT_EQ(waitCategoryColor(WaitCategory::Synchronization, P), P.syncWait);
  EXPECT_EQ(waitCategoryColor(WaitCategory::IO, P), P.ioWait);
  EXPECT_EQ(waitCategoryColor(WaitCategory::Memory, P), P.memoryWait);
  EXPECT_EQ(waitCategoryColor(WaitCategory::Preempted, P), P.ready);
  EXPECT_EQ(waitCategoryColor(WaitCategory::Other, P), P.otherWait);
  EXPECT_EQ(waitCategoryColorFromByte(analysis::kNoWaitCategory, P), P.otherWait);
  EXPECT_EQ(waitCategoryColorFromByte(2, P), P.memoryWait);
  // Red is red, dark blue is darker than light blue, amber is yellowish.
  EXPECT_GT(colorR(P.syncWait), colorB(P.syncWait) + 80);
  EXPECT_GT(colorB(P.ioWait), colorR(P.ioWait) + 80);
  EXPECT_GT(colorB(P.memoryWait), 200);
  EXPECT_GT(colorG(P.memoryWait), colorG(P.ioWait));
  EXPECT_GT(colorG(P.running), colorR(P.running) + 80);
  EXPECT_GT(colorR(P.ready), 200);
  EXPECT_LT(colorB(P.ready), 80);
}

TEST(TimelineColors, StateColorUsesThePlatformWaitTable) {
  model::ThreadStateInterval running;
  running.state = model::ThreadState::Running;
  EXPECT_EQ(stateColor(1, running, P), P.running);
  model::ThreadStateInterval ready;
  ready.state = model::ThreadState::Ready;
  EXPECT_EQ(stateColor(1, ready, P), P.ready);
  model::ThreadStateInterval unknown;
  EXPECT_EQ(stateColor(1, unknown, P), P.unknown);

  model::ThreadStateInterval wait;
  wait.state = model::ThreadState::Waiting;
  wait.wait_reason = 6;  // Windows UserRequest: synchronization
  EXPECT_EQ(stateColor(1, wait, P), P.syncWait);
  wait.wait_reason = 2;  // Windows PageIn: memory
  EXPECT_EQ(stateColor(1, wait, P), P.memoryWait);
  wait.wait_reason = 0x02;  // Linux D: I/O
  EXPECT_EQ(stateColor(2, wait, P), P.ioWait);
  wait.wait_reason = 0x01;  // Linux S: synchronization
  EXPECT_EQ(stateColor(2, wait, P), P.syncWait);
}

TEST(TimelineColors, BandColumnBlendsByFraction) {
  analysis::BandColumn pureRun;
  pureRun.running = 1;
  pureRun.unknown = 0;
  const Color32 run = bandColumnColor(pureRun, P);
  EXPECT_NEAR(colorG(run), colorG(P.running), 16);
  EXPECT_NEAR(colorR(run), colorR(P.running), 16);

  analysis::BandColumn pureWait;
  pureWait.waiting = 1;
  pureWait.unknown = 0;
  pureWait.waitCategory = static_cast<uint8_t>(analysis::WaitCategory::Synchronization);
  const Color32 wait = bandColumnColor(pureWait, P);
  EXPECT_NEAR(colorR(wait), colorR(P.syncWait), 16);
  EXPECT_NEAR(colorB(wait), colorB(P.syncWait), 16);

  analysis::BandColumn half;
  half.running = 0.5f;
  half.waiting = 0.5f;
  half.unknown = 0;
  half.waitCategory = static_cast<uint8_t>(analysis::WaitCategory::IO);
  const Color32 mix = bandColumnColor(half, P);
  EXPECT_NEAR(colorG(mix), (colorG(P.running) + colorG(P.ioWait)) / 2, 17);

  const analysis::BandColumn nothing;  // unknown = 1
  EXPECT_NEAR(colorR(bandColumnColor(nothing, P)), colorR(P.unknown), 16);

  analysis::BandColumn zero;
  zero.unknown = 0;
  EXPECT_EQ(bandColumnColor(zero, P), P.unknown);
}

TEST(TimelineColors, BandColorsAreQuantisedSoNeighboursMerge) {
  analysis::BandColumn a, b;
  a.running = 0.800f;
  a.unknown = 0.200f;
  b.running = 0.802f;
  b.unknown = 0.198f;
  EXPECT_EQ(bandColumnColor(a, P), bandColumnColor(b, P));
  EXPECT_EQ(colorR(bandColumnColor(a, P)) % 16, 0);
  EXPECT_EQ(colorA(bandColumnColor(a, P)), 255);
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
    return callstackColor(m, CallstackIdx(n), FrameColorMode::ByModule, nullptr, FramePosition::Leaf,
                          P.unknown);
  };
  const auto byFunc = [&](uint32_t n) {
    return callstackColor(m, CallstackIdx(n), FrameColorMode::ByFunction, nullptr, FramePosition::Leaf,
                          P.unknown);
  };
  EXPECT_EQ(byModule(1), byModule(2));  // same module
  EXPECT_NE(byModule(1), byModule(3));  // different module
  EXPECT_NE(byFunc(1), byFunc(2));      // same module, different function
  EXPECT_NE(byFunc(1), byFunc(3));
  EXPECT_EQ(
      callstackColor(m, CallstackIdx(), FrameColorMode::ByModule, nullptr, FramePosition::Leaf, P.unknown),
      P.unknown);
  EXPECT_EQ(
      callstackColor(m, CallstackIdx(99), FrameColorMode::ByModule, nullptr, FramePosition::Leaf, P.unknown),
      P.unknown);
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
