#include <gtest/gtest.h>

#include "etw_hex.h"
#include "etw_stats.h"
#include "test_util.h"

using namespace tachyon::etlstat;
using namespace tachyon::etlstat::testing;

namespace {

constexpr size_t kSp = static_cast<size_t>(ParentKind::SampledProfile);
constexpr size_t kCs = static_cast<size_t>(ParentKind::CSwitch);
constexpr size_t kRt = static_cast<size_t>(ParentKind::ReadyThread);

void addSample(StatsAccumulator& acc, uint64_t ts, uint32_t tid, uint16_t cpu) {
  const ByteVec p = sampledProfilePayload(0x1000 + ts, tid);
  acc.add(makeEvent(guids::kPerfInfo, 46, 2, ts, tid, cpu, p));
}

void addStack(StatsAccumulator& acc, uint64_t arrival, uint64_t parentTs, uint32_t tid, size_t depth) {
  const ByteVec p = stackWalkPayload(parentTs, 100, tid, std::vector<uint64_t>(depth, 0x7ff600001000ull));
  acc.add(makeEvent(guids::kStackWalk, 32, 2, arrival, tid, 0, p));
}

}  // namespace

TEST(Stats, CountsPerProviderOpcodeVersion) {
  StatsAccumulator acc;
  const ByteVec sp = sampledProfilePayload(1, 2);
  const ByteVec sp3 = sampledProfilePayload(1, 2);
  acc.add(makeEvent(guids::kPerfInfo, 46, 2, 10, 2, 0, sp));
  acc.add(makeEvent(guids::kPerfInfo, 46, 2, 20, 2, 0, sp));
  acc.add(makeEvent(guids::kPerfInfo, 46, 3, 30, 2, 0, sp3));
  acc.add(makeEvent(guids::kThread, 72, 2, 40, 2, 0, ByteVec(8)));
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.totalEvents, 4u);
  EXPECT_EQ(s.keys.size(), 3u);
  const auto& v2 = s.keys.at(EventKey{guids::kPerfInfo, 46, 2});
  EXPECT_EQ(v2.count, 2u);
  EXPECT_EQ(v2.minSize, 16u);
  EXPECT_EQ(v2.maxSize, 16u);
  EXPECT_EQ(s.keys.at(EventKey{guids::kPerfInfo, 46, 3}).count, 1u);
  EXPECT_EQ(s.keys.at(EventKey{guids::kThread, 72, 2}).totalBytes, 8u);
  EXPECT_EQ(s.firstEventTs, 10u);
  EXPECT_EQ(s.lastEventTs, 40u);
}

TEST(Stats, MatchesStackToParentByTimestampAndThread) {
  StatsAccumulator acc;
  addSample(acc, 1000, 7, 0);
  addStack(acc, 1004, 1000, 7, 4);
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.stackEvents, 1u);
  EXPECT_EQ(s.stackMatched, 1u);
  EXPECT_EQ(s.stackUnmatched, 0u);
  EXPECT_EQ(s.parents[kSp].total, 1u);
  EXPECT_EQ(s.parents[kSp].withStack, 1u);
  EXPECT_EQ(s.parents[kSp].withoutStack, 0u);
  EXPECT_EQ(s.parents[kSp].maxDepth, 4u);
  EXPECT_EQ(s.maxStackDepth, 4u);
  EXPECT_EQ(s.sampledNonIdleWithStack, 1u);
}

TEST(Stats, WrongThreadOrTimestampIsUnmatched) {
  StatsAccumulator acc;
  addSample(acc, 1000, 7, 0);
  addStack(acc, 1004, 1000, 8, 3);  // wrong thread
  addStack(acc, 1005, 999, 7, 3);   // wrong timestamp
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.stackEvents, 2u);
  EXPECT_EQ(s.stackMatched, 0u);
  EXPECT_EQ(s.stackUnmatched, 2u);
  EXPECT_EQ(s.parents[kSp].withStack, 0u);
  EXPECT_EQ(s.parents[kSp].withoutStack, 1u);
}

TEST(Stats, ParentCanOnlyBeMatchedOnce) {
  StatsAccumulator acc;
  addSample(acc, 1000, 7, 0);
  addStack(acc, 1004, 1000, 7, 3);
  addStack(acc, 1005, 1000, 7, 3);  // duplicate stack for the same parent
  acc.finish();
  EXPECT_EQ(acc.snapshot().stackMatched, 1u);
  EXPECT_EQ(acc.snapshot().stackUnmatched, 1u);
}

TEST(Stats, TwoParentsWithSameKeyMatchInArrivalOrder) {
  StatsAccumulator acc;
  addSample(acc, 1000, 7, 0);
  addSample(acc, 1000, 7, 1);  // same timestamp and thread on another CPU (pathological)
  addStack(acc, 1004, 1000, 7, 3);
  addStack(acc, 1005, 1000, 7, 3);
  acc.finish();
  EXPECT_EQ(acc.snapshot().stackMatched, 2u);
  EXPECT_EQ(acc.snapshot().stackUnmatched, 0u);
  EXPECT_EQ(acc.snapshot().parents[kSp].withStack, 2u);
}

TEST(Stats, CSwitchMatchesViaHeaderNewOrOldThread) {
  StatsAccumulator acc;
  // Stack keyed on the header thread.
  acc.add(makeEvent(guids::kThread, 36, 2, 100, 11, 0, cswitchPayload(11, 22)));
  addStack(acc, 103, 100, 11, 2);
  // Stack keyed on the old thread (payload only).
  acc.add(makeEvent(guids::kThread, 36, 2, 200, 99, 0, cswitchPayload(11, 22)));
  addStack(acc, 203, 200, 22, 2);
  // Stack keyed on the new thread (payload only).
  acc.add(makeEvent(guids::kThread, 36, 2, 300, 99, 0, cswitchPayload(11, 22)));
  addStack(acc, 303, 300, 11, 2);
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.parents[kCs].total, 3u);
  EXPECT_EQ(s.parents[kCs].withStack, 3u);
  EXPECT_EQ(s.stackRoleCount[static_cast<size_t>(MatchRole::HeaderThread)], 1u);
  EXPECT_EQ(s.stackRoleCount[static_cast<size_t>(MatchRole::PayloadOldThread)], 1u);
  EXPECT_EQ(s.stackRoleCount[static_cast<size_t>(MatchRole::PayloadThread)], 1u);
}

TEST(Stats, ReadyThreadTrackedSeparately) {
  StatsAccumulator acc;
  acc.add(makeEvent(guids::kThread, 50, 2, 100, 5, 0, readyThreadPayload(6)));
  acc.add(makeEvent(guids::kThread, 50, 2, 200, 5, 0, readyThreadPayload(6)));
  addStack(acc, 203, 200, 6, 2);
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.parents[kRt].total, 2u);
  EXPECT_EQ(s.parents[kRt].withStack, 1u);
  EXPECT_EQ(s.parents[kRt].withoutStack, 1u);
}

TEST(Stats, EvictsParentsOutsideWindowAndLateStackIsUnmatched) {
  StatsConfig cfg;
  cfg.pendingWindowTicks = 1000;
  StatsAccumulator acc(cfg);
  addSample(acc, 1000, 7, 0);
  addSample(acc, 5000, 8, 0);       // advances time past the window, evicting the first parent
  addStack(acc, 5002, 1000, 7, 3);  // too late
  addStack(acc, 5003, 5000, 8, 3);
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.stackMatched, 1u);
  EXPECT_EQ(s.stackUnmatched, 1u);
  EXPECT_EQ(s.parents[kSp].withoutStack, 1u);
  EXPECT_EQ(s.parents[kSp].withStack, 1u);
}

TEST(Stats, MalformedPayloadsAreCountedNotCrashed) {
  StatsAccumulator acc;
  acc.add(makeEvent(guids::kPerfInfo, 46, 2, 10, 1, 0, ByteVec(5)));
  acc.add(makeEvent(guids::kStackWalk, 32, 2, 11, 1, 0, ByteVec(3)));
  acc.add(makeEvent(guids::kThread, 36, 2, 12, 1, 0, ByteVec(3)));
  acc.add(makeEvent(guids::kThread, 50, 2, 13, 1, 0, ByteVec(3)));
  acc.finish();
  EXPECT_EQ(acc.snapshot().malformedEvents, 4u);
  EXPECT_EQ(acc.snapshot().sampledProfile, 0u);
}

TEST(Stats, TracksMaxDepthAcrossParents) {
  StatsAccumulator acc;
  addSample(acc, 100, 1, 0);
  addStack(acc, 101, 100, 1, 12);
  addSample(acc, 200, 1, 0);
  addStack(acc, 201, 200, 1, 3);
  acc.add(makeEvent(guids::kThread, 36, 2, 300, 1, 0, cswitchPayload(1, 2)));
  addStack(acc, 301, 300, 1, 200);
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.maxStackDepth, 200u);
  EXPECT_EQ(s.parents[kSp].maxDepth, 12u);
  EXPECT_EQ(s.parents[kCs].maxDepth, 200u);
  EXPECT_EQ(s.parents[kSp].depthSum, 15u);
}

TEST(Stats, PerCpuSampleAccounting) {
  StatsAccumulator acc;
  addSample(acc, 100, 5, 0);
  addSample(acc, 200, 0, 1);  // idle
  addSample(acc, 300, 6, 0);
  acc.finish();
  const auto& s = acc.snapshot();
  ASSERT_EQ(s.cpus.size(), 2u);
  EXPECT_EQ(s.cpus[0].samples, 2u);
  EXPECT_EQ(s.cpus[0].nonIdleSamples, 2u);
  EXPECT_EQ(s.cpus[1].samples, 1u);
  EXPECT_EQ(s.cpus[1].nonIdleSamples, 0u);
  EXPECT_EQ(s.sampledNonIdle, 2u);
  EXPECT_EQ(s.firstSampleTs, 100u);
  EXPECT_EQ(s.lastSampleTs, 300u);
  EXPECT_EQ(s.samplesPerThread.at(5), 1u);
}

TEST(Stats, DetectsSetNameEvents) {
  StatsAccumulator acc;
  ByteVec name;
  put<uint32_t>(name, 100);
  put<uint32_t>(name, 101);
  putWide(name, u"Main");
  acc.add(makeEvent(guids::kThread, 72, 2, 10, 101, 0, name));
  acc.finish();
  const auto& s = acc.snapshot();
  EXPECT_EQ(s.setNameEvents, 1u);
  ASSERT_EQ(s.threadNames.size(), 1u);
  EXPECT_EQ(s.threadNames[0].name, "Main");
  EXPECT_EQ(s.threadNames[0].threadId, 101u);
}

TEST(Stats, SyntheticFixtureEndToEnd) {
  auto events = loadHexEventFile(std::string(TACHYON_FIXTURE_DIR) + "/etw/synthetic_basic.hexevents");
  ASSERT_TRUE(events.ok()) << events.error().toString();
  StatsAccumulator acc;
  for (const HexEvent& e : *events) acc.add(e.view());
  acc.finish();
  const auto& s = acc.snapshot();

  EXPECT_EQ(s.malformedEvents, 0u);
  EXPECT_EQ(s.sampledProfile, 4u);
  EXPECT_EQ(s.sampledNonIdle, 3u);
  EXPECT_EQ(s.stackEvents, 6u);
  EXPECT_EQ(s.stackMatched, 5u);
  EXPECT_EQ(s.stackUnmatched, 1u);
  EXPECT_EQ(s.maxStackDepth, 5u);
  EXPECT_EQ(s.parents[kSp].withStack, 4u);
  EXPECT_EQ(s.parents[kCs].withStack, 1u);
  EXPECT_EQ(s.parents[kRt].withoutStack, 1u);
  EXPECT_EQ(s.setNameEvents, 1u);
  ASSERT_EQ(s.threadNames.size(), 1u);
  EXPECT_EQ(s.threadNames[0].name, "RenderThread");
  EXPECT_EQ(s.threadToProcess.at(101), 100u);
  EXPECT_EQ(s.processImage.at(100), "game.exe");
  EXPECT_EQ(s.keys.at(EventKey{guids::kImage, 10, 3}).count, 1u);
  ASSERT_EQ(s.cpus.size(), 2u);
  EXPECT_EQ(s.cpus[0].samples, 3u);
  EXPECT_EQ(s.cpus[1].samples, 1u);
}
