// WP-64: graceful degradation of the timeline LOD under a memory ceiling - trimMemory() frees the fine strip
// level and then the finest levels, LodOptions::maxBytes picks a coarser base level up front; queries stay
// correct (they fall back to the next coarser level or to the samples).
#include "analysis/lod.h"
#include "lod_test_util.h"

#include <gtest/gtest.h>

#include <cmath>

namespace tachyon::analysis {
namespace {

using model::TraceModel;
using testutil::fixture;

bool sameStrip(const SampleStripResult& a, const SampleStripResult& b) {
  if (a.columns.size() != b.columns.size()) return false;
  for (size_t i = 0; i < a.columns.size(); ++i) {
    if (a.columns[i].stack != b.columns[i].stack) return false;
    if (std::fabs(a.columns[i].share - b.columns[i].share) > 1e-5f) return false;
  }
  return true;
}

TEST(LodMemory, TrimmingFreesTheFineLevelThenTheFinestLevelsAndKeepsTheCoarsest) {
  const TraceModel& m = fixture("medium");
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  const size_t before = lod->memoryBytes();
  const size_t levels = lod->levels().size();
  ASSERT_GE(levels, 4u);
  EXPECT_FALSE(lod->trimState().fineDropped);
  EXPECT_EQ(lod->trimState().droppedLevels, 0u);

  // The first request is satisfied by the fine strip level alone when it is big enough.
  const size_t freedFine = lod->trimMemory(1);
  EXPECT_GT(freedFine, 0u);
  EXPECT_TRUE(lod->trimState().fineDropped);
  EXPECT_EQ(lod->trimState().droppedLevels, 0u)
      << "levels are only dropped when the fine level was not enough";
  EXPECT_EQ(lod->memoryBytes(), before - freedFine);

  const size_t freedLevel = lod->trimMemory(before);  // everything but the coarsest level
  EXPECT_GT(freedLevel, 0u);
  EXPECT_EQ(lod->trimState().droppedLevels, levels - 1);
  EXPECT_LT(lod->memoryBytes(), before / 100);
  EXPECT_EQ(lod->trimMemory(before), 0u) << "nothing left to free";
  // The coarsest level is still readable, a dropped one is not.
  LodBucket b;
  EXPECT_TRUE(lod->bucket(ThreadIdx(0), levels - 1, 0, b));
  EXPECT_FALSE(lod->bucket(ThreadIdx(0), 0, 0, b));
}

TEST(LodMemory, TrimmingBeforeTheBuildIsFinishedDoesNothing) {
  const TraceModel& m = fixture("medium");
  auto lod = TimelineLod::prepare(m);
  ASSERT_TRUE(lod.ok());
  EXPECT_EQ(lod->trimMemory(1 << 20), 0u);
  EXPECT_EQ(lod->trimState().droppedLevels, 0u);
}

TEST(LodMemory, QueriesStayCorrectAfterTrimming) {
  const TraceModel& m = fixture("medium");
  auto full = buildLod(m);
  auto trimmed = buildLod(m);
  ASSERT_TRUE(full.ok() && trimmed.ok());
  trimmed->trimMemory(full->memoryBytes() / 2);  // the fine level and the finest levels
  ASSERT_GT(trimmed->trimState().droppedLevels, 0u);
  const uint64_t s = m.header.start_ts, total = m.header.end_ts - m.header.start_ts;
  const uint64_t firstWidth = trimmed->levels()[trimmed->trimState().droppedLevels].bucketWidth;
  size_t checkedDirect = 0, checkedLevel = 0;
  for (double f : {0.3, 0.7, 1.0, 1.9, 3.0, 8.0, 40.0}) {
    const uint64_t w = static_cast<uint64_t>(static_cast<double>(firstWidth) * f * 400);
    if (w == 0 || w >= total) continue;
    for (size_t thread = 0; thread < m.threads.size(); thread += 7) {
      RowQuery rq;
      rq.thread = ThreadIdx(static_cast<uint32_t>(thread));
      rq.t0 = s + total / 5;
      rq.t1 = rq.t0 + w;
      rq.pixelWidth = 400;
      StateBandResult b, bd;
      SampleStripResult st, sd;
      ASSERT_TRUE(trimmed->rowColumns(rq, b, st).ok());
      RowQuery direct = rq;
      direct.mode = QueryMode::Direct;
      ASSERT_TRUE(trimmed->rowColumns(direct, bd, sd).ok());
      if (b.level == kDirectLevel) {
        // Below the first remaining level: exactly the direct answer.
        EXPECT_TRUE(sameStrip(st, sd)) << f;
        for (size_t c = 0; c < 400; ++c) {
          ASSERT_NEAR(b.columns[c].running, bd.columns[c].running, 1e-5f);
          ASSERT_NEAR(b.columns[c].waiting, bd.columns[c].waiting, 1e-5f);
        }
        ++checkedDirect;
      } else {
        // At or above it: the same answer the untrimmed LOD gives for these (coarser) levels.
        StateBandResult fb;
        SampleStripResult fs;
        ASSERT_TRUE(full->rowColumns(rq, fb, fs).ok());
        if (fb.level == b.level) {
          EXPECT_TRUE(sameStrip(st, fs)) << f;
          ++checkedLevel;
        }
      }
    }
  }
  EXPECT_GT(checkedDirect, 0u);
  EXPECT_GT(checkedLevel, 0u);
  // Flame chart and buckets of the surviving levels still work.
  TimelineQuery tq;
  tq.thread = ThreadIdx(0);
  tq.depthBegin = 1;
  tq.depthEnd = 8;
  tq.t0 = s;
  tq.t1 = s + total / 3;
  tq.pixelWidth = 300;
  TimelineResult flame;
  EXPECT_TRUE(trimmed->timeline(tq, flame).ok());
  EXPECT_FALSE(flame.spans.empty());
}

TEST(LodMemory, AByteBudgetRaisesTheBaseLevelUpFront) {
  const TraceModel& m = fixture("medium");
  auto unlimited = buildLod(m);
  ASSERT_TRUE(unlimited.ok());
  EXPECT_EQ(unlimited->degradedBaseLevels(), 0u);
  LodOptions opt;
  opt.maxBytes = unlimited->memoryBytes() / 6;
  auto limited = buildLod(m, opt);
  ASSERT_TRUE(limited.ok());
  EXPECT_GT(limited->degradedBaseLevels(), 0u);
  EXPECT_GT(limited->levels().front().level, unlimited->levels().front().level);
  EXPECT_LE(limited->memoryBytes(), opt.maxBytes + opt.maxBytes / 4);
  EXPECT_LT(limited->memoryBytes(), unlimited->memoryBytes());
  EXPECT_FALSE(limited->trimState().fineDropped);  // never built: no fine level at all
  // Fine zooms are answered from the samples and are exact.
  RowQuery rq;
  rq.thread = ThreadIdx(1);
  rq.t0 = m.header.start_ts + 123456;
  rq.t1 = rq.t0 + 400 * 2000;
  rq.pixelWidth = 400;
  StateBandResult b, bd;
  SampleStripResult st, sd;
  ASSERT_TRUE(limited->rowColumns(rq, b, st).ok());
  EXPECT_EQ(b.level, kDirectLevel);
  rq.mode = QueryMode::Direct;
  ASSERT_TRUE(limited->rowColumns(rq, bd, sd).ok());
  EXPECT_TRUE(sameStrip(st, sd));
  // A budget below the top level alone still builds (one level is kept).
  LodOptions tiny;
  tiny.maxBytes = 1;
  auto minimal = buildLod(m, tiny);
  ASSERT_TRUE(minimal.ok());
  EXPECT_EQ(minimal->levels().size(), 1u);
}

}  // namespace
}  // namespace tachyon::analysis
