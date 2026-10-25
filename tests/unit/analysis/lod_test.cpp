// TimelineLod on hand-built models: exact span semantics, deep stacks, level selection, pending threads,
// cancellation.
#include "analysis/lod.h"

#include <gtest/gtest.h>

#include <atomic>

namespace tachyon::analysis {
namespace {

using model::TraceModel;

// Nodes: 0 root; 1 A(d1); 2 B(d2, parent A); 3 C(d2, parent A); 4 D(d3, parent B); 5 E(d1).
void addNodes(TraceModel& m) {
  const uint32_t parents[] = {kInvalidIdx, 0, 1, 1, 2, 0};
  for (uint32_t p : parents) m.nodes.push_back({CallstackIdx(p), FrameIdx(0)});
}

TraceModel makeModel(uint64_t start, uint64_t end, uint64_t interval) {
  TraceModel m;
  m.header.start_ts = start;
  m.header.end_ts = end;
  m.header.clock_frequency_hz = 10000000;
  m.sampleIntervalTicks = interval;
  m.threads.resize(1);
  m.samplesByThread.resize(1);
  m.statesByThread.resize(1);
  addNodes(m);
  return m;
}

void addSample(TraceModel& m, size_t thread, uint64_t ts, uint32_t leaf) {
  m.samplesByThread[thread].ts.push_back(ts);
  m.samplesByThread[thread].callstack.push_back(CallstackIdx(leaf));
  m.samplesByThread[thread].cpu.push_back(0);
  m.samplesByThread[thread].flags.push_back(0);
}

struct Sp {
  uint32_t depth;
  uint64_t t0, t1;
  uint32_t node;
  uint32_t merged;
  bool operator==(const Sp&) const = default;
};
std::vector<Sp> spansOf(const TimelineResult& r, uint32_t onlyDepth = 0) {
  std::vector<Sp> v;
  for (const auto& s : r.spans)
    if (onlyDepth == 0 || s.depth == onlyDepth)
      v.push_back({s.depth, s.t0, s.t1, s.node.get(), s.mergedCount});
  return v;
}

// Sample i (64 ticks each, from 1000) has leaf: D D D D C C A E - - B B B C D D (- = no sample), then four
// empty slots, so a pixel column of 64 ticks is exactly one base-level-0 bucket.
TraceModel patternModel() {
  TraceModel m = makeModel(1000, 1000 + 64 * 20, 64);
  const uint32_t leaves[] = {4, 4, 4, 4, 3, 3, 1, 5, 0, 0, 2, 2, 2, 3, 4, 4};
  for (size_t i = 0; i < 16; ++i)
    if (leaves[i] != 0) addSample(m, 0, 1000 + 64 * i, leaves[i]);
  return m;
}

TEST(LodSpans, ExactSemanticsDirectAndLodAgreeAtBucketBoundary) {
  const TraceModel m = patternModel();
  LodOptions opt;
  opt.forceBaseLevel = 0;  // 64-tick buckets
  opt.levelStep = 1;
  auto lod = buildLod(m, opt);
  ASSERT_TRUE(lod.ok()) << lod.error().toString();
  for (QueryMode mode : {QueryMode::Direct, QueryMode::Auto}) {
    TimelineQuery q;
    q.thread = ThreadIdx(0);
    q.depthBegin = 1;
    q.depthEnd = 10;
    q.t0 = 1000;
    q.t1 = 1000 + 64 * 20;
    q.pixelWidth = 20;
    q.mode = mode;
    TimelineResult r;
    ASSERT_TRUE(lod->timeline(q, r).ok());
    EXPECT_EQ(r.level, mode == QueryMode::Auto ? 0 : kDirectLevel);
    auto col = [](uint64_t c) { return 1000 + 64 * c; };
    const std::vector<Sp> expect = {
        // depth 1: A for columns 0-6 (D, C, A leaves), E for 7, A for 10-15
        {1, col(0), col(7), 1, 1},
        {1, col(7), col(8), 5, 1},
        {1, col(10), col(16), 1, 1},
        // depth 2: B for 0-3, C for 4-5, nothing for 6-7 (leaves A and E are depth 1), B 10-12, C 13, B 14-15
        {2, col(0), col(4), 2, 1},
        {2, col(4), col(6), 3, 1},
        {2, col(10), col(13), 2, 1},
        {2, col(13), col(14), 3, 1},
        {2, col(14), col(16), 2, 1},
        // depth 3: D for 0-3 and 14-15
        {3, col(0), col(4), 4, 1},
        {3, col(14), col(16), 4, 1},
    };
    EXPECT_EQ(spansOf(r), expect) << "mode " << int(mode);
  }
}

TEST(LodSpans, MixedColumnsMergeAndCountSamples) {
  const TraceModel m = patternModel();
  LodOptions opt;
  opt.forceBaseLevel = 0;
  opt.levelStep = 1;
  auto lod = buildLod(m, opt);
  ASSERT_TRUE(lod.ok());
  // Two samples per column: (D,D) (D,D) (C,C) (A,E) gap (B,B) (B,C) (D,D) gap gap
  for (QueryMode mode : {QueryMode::Direct, QueryMode::Auto}) {
    TimelineQuery q;
    q.thread = ThreadIdx(0);
    q.depthBegin = 1;
    q.depthEnd = 4;
    q.t0 = 1000;
    q.t1 = 1000 + 64 * 20;
    q.pixelWidth = 10;
    q.mode = mode;
    TimelineResult r;
    ASSERT_TRUE(lod->timeline(q, r).ok());
    auto col = [](uint64_t c) { return 1000 + 128 * c; };
    const uint32_t inv = kInvalidIdx;
    // Column 3 holds A and E: depth 1 is Mixed (2 samples start inside) and deeper depths are Empty.
    EXPECT_EQ(
        spansOf(r, 1),
        (std::vector<Sp>{{1, col(0), col(3), 1, 1}, {1, col(3), col(4), inv, 2}, {1, col(5), col(8), 1, 1}}));
    // Column 6 holds B and C: depth 2 is Mixed there.
    EXPECT_EQ(spansOf(r, 2), (std::vector<Sp>{{2, col(0), col(2), 2, 1},
                                              {2, col(2), col(3), 3, 1},
                                              {2, col(5), col(6), 2, 1},
                                              {2, col(6), col(7), inv, 2},
                                              {2, col(7), col(8), 2, 1}}));
    EXPECT_EQ(spansOf(r, 3), (std::vector<Sp>{{3, col(0), col(2), 4, 1}, {3, col(7), col(8), 4, 1}}));
  }
}

TEST(LodSpans, ExactRegimeFollowsSampleEdges) {
  const TraceModel m = patternModel();
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  // 2000 pixels over 1280 ticks: a pixel is narrower than a sample, spans use the sample edges.
  for (QueryMode mode : {QueryMode::Direct, QueryMode::Auto}) {
    TimelineQuery q;
    q.thread = ThreadIdx(0);
    q.depthBegin = 1;
    q.depthEnd = 10;
    q.t0 = 1000;
    q.t1 = 1000 + 64 * 20;
    q.pixelWidth = 2000;
    q.mode = mode;
    TimelineResult r;
    ASSERT_TRUE(lod->timeline(q, r).ok());
    EXPECT_EQ(r.level, kDirectLevel);
    auto at = [](uint64_t i) { return 1000 + 64 * i; };
    EXPECT_EQ(spansOf(r), (std::vector<Sp>{{1, at(0), at(7), 1, 1},
                                           {1, at(7), at(8), 5, 1},
                                           {1, at(10), at(16), 1, 1},
                                           {2, at(0), at(4), 2, 1},
                                           {2, at(4), at(6), 3, 1},
                                           {2, at(10), at(13), 2, 1},
                                           {2, at(13), at(14), 3, 1},
                                           {2, at(14), at(16), 2, 1},
                                           {3, at(0), at(4), 4, 1},
                                           {3, at(14), at(16), 4, 1}}));
    // The window clips spans and restricts them to the samples inside.
    q.t0 = at(2) + 10;
    q.t1 = at(5) + 20;
    q.pixelWidth = 4000;
    ASSERT_TRUE(lod->timeline(q, r).ok());
    EXPECT_EQ(spansOf(r), (std::vector<Sp>{{1, at(2) + 10, at(5) + 20, 1, 1},
                                           {2, at(2) + 10, at(4), 2, 1},
                                           {2, at(4), at(5) + 20, 3, 1},
                                           {3, at(2) + 10, at(4), 4, 1}}));
  }
}

TEST(LodSpans, ExactRegimeMergesRunsNarrowerThanAPixel) {
  // 128 samples one tick apart (extent clipped to 1 tick) alternating B and C under A, interval 64.
  TraceModel m = makeModel(1000, 1000 + 512, 64);
  for (uint64_t i = 0; i < 128; ++i) addSample(m, 0, 1000 + i, i % 2 ? 3 : 2);
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  TimelineQuery q;
  q.thread = ThreadIdx(0);
  q.depthBegin = 1;
  q.depthEnd = 5;
  q.t0 = 1000;
  q.t1 = 1128;
  q.pixelWidth = 64;  // 2 ticks per pixel, every run at depth 2 is 1 tick
  TimelineResult r;
  ASSERT_TRUE(lod->timeline(q, r).ok());
  EXPECT_EQ(r.level, kDirectLevel);
  // The last sample has a full 64-tick extent (no next sample) and stands on its own, clipped to the window.
  EXPECT_EQ(
      spansOf(r),
      (std::vector<Sp>{{1, 1000, 1128, 1, 1}, {2, 1000, 1127, kInvalidIdx, 127}, {2, 1127, 1128, 3, 1}}));
  // At 1 pixel per tick the same runs are wide enough to stand on their own.
  q.pixelWidth = 128;
  ASSERT_TRUE(lod->timeline(q, r).ok());
  ASSERT_EQ(r.spans.size(), 129u);
  EXPECT_EQ(r.spans[1].t1 - r.spans[1].t0, 1u);
  EXPECT_EQ(r.spans[1].node.get(), 2u);
  EXPECT_EQ(r.spans[2].node.get(), 3u);
}

TEST(LodSpans, DeepStackUsesLevelAncestors) {
  TraceModel m = makeModel(0, 1 << 20, 64);
  m.nodes.clear();
  m.nodes.push_back({CallstackIdx(), FrameIdx(0)});
  const uint32_t depth = 20000;
  for (uint32_t i = 1; i <= depth; ++i) m.nodes.push_back({CallstackIdx(i - 1), FrameIdx(0)});
  for (uint64_t i = 0; i < 4096; ++i) addSample(m, 0, i * 64, depth);
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  TimelineQuery q;
  q.thread = ThreadIdx(0);
  q.depthBegin = 15000;
  q.depthEnd = 15004;
  q.t0 = 0;
  q.t1 = 4096 * 64;
  q.pixelWidth = 64;
  for (QueryMode mode : {QueryMode::Direct, QueryMode::Auto}) {
    q.mode = mode;
    TimelineResult r;
    ASSERT_TRUE(lod->timeline(q, r).ok());
    ASSERT_EQ(r.spans.size(), 4u);
    for (uint32_t i = 0; i < 4; ++i) {
      EXPECT_EQ(r.spans[i].depth, 15000 + i);
      EXPECT_EQ(r.spans[i].node.get(), 15000 + i);
      EXPECT_EQ(r.spans[i].t0, 0u);
      EXPECT_EQ(r.spans[i].t1, 4096u * 64);
    }
  }
  // Depth 0 (the root) yields nothing; a range beyond the deepest node is clipped.
  q.depthBegin = 0;
  q.depthEnd = 1;
  TimelineResult r;
  ASSERT_TRUE(lod->timeline(q, r).ok());
  EXPECT_TRUE(r.spans.empty());
  q.depthBegin = depth + 1;
  q.depthEnd = depth + 100;
  ASSERT_TRUE(lod->timeline(q, r).ok());
  EXPECT_TRUE(r.spans.empty());
}

TEST(LodQuery, InvalidArguments) {
  const TraceModel m = patternModel();
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  StateBandQuery q;
  q.thread = ThreadIdx(0);
  q.t0 = 1000;
  q.t1 = 2000;
  q.pixelWidth = 10;
  StateBandResult r;
  EXPECT_TRUE(lod->stateBands(q, r).ok());
  q.pixelWidth = 0;
  EXPECT_FALSE(lod->stateBands(q, r).ok());
  q.pixelWidth = kMaxPixelWidth + 1;
  EXPECT_FALSE(lod->stateBands(q, r).ok());
  q.pixelWidth = 10;
  q.t1 = q.t0;
  EXPECT_FALSE(lod->stateBands(q, r).ok());
  q.t1 = 2000;
  q.thread = ThreadIdx(5);
  EXPECT_FALSE(lod->stateBands(q, r).ok());
  TimelineLod unprepared;
  EXPECT_FALSE(unprepared.stateBands(q, r).ok());
}

TEST(LodQuery, OutsideTraceIsUnknownAndEmpty) {
  const TraceModel m = patternModel();
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  StateBandQuery q;
  q.thread = ThreadIdx(0);
  q.t0 = 5000000;
  q.t1 = 6000000;
  q.pixelWidth = 16;
  StateBandResult r;
  ASSERT_TRUE(lod->stateBands(q, r).ok());
  for (const auto& c : r.columns) {
    EXPECT_EQ(c.unknown, 1.0f);
    EXPECT_EQ(c.waitCategory, kNoWaitCategory);
  }
  SampleStripQuery sq;
  sq.thread = ThreadIdx(0);
  sq.t0 = 5000000;
  sq.t1 = 6000000;
  sq.pixelWidth = 16;
  SampleStripResult sr;
  ASSERT_TRUE(lod->sampleStrip(sq, sr).ok());
  for (const auto& c : sr.columns) EXPECT_FALSE(c.stack.valid());
  TimelineQuery tq;
  tq.thread = ThreadIdx(0);
  tq.depthEnd = 40;
  tq.t0 = 0;
  tq.t1 = 500;
  tq.pixelWidth = 16;
  TimelineResult tr;
  ASSERT_TRUE(lod->timeline(tq, tr).ok());
  EXPECT_TRUE(tr.spans.empty());
}

TEST(LodLevels, GeometryAndSelection) {
  const uint64_t total = (64ull << 12) * 3;
  TraceModel m = makeModel(1000, 1000 + total, 64);  // 3 * 4096 buckets of 64 ticks
  for (uint64_t i = 0; i < 100; ++i) addSample(m, 0, 1000 + i * 64, 4);
  LodOptions opt;
  opt.forceBaseLevel = 0;
  opt.levelStep = 2;
  auto lod = buildLod(m, opt);
  ASSERT_TRUE(lod.ok());
  const auto& lv = lod->levels();
  ASSERT_GE(lv.size(), 3u);
  EXPECT_EQ(lv[0].level, 0u);
  EXPECT_EQ(lv[0].bucketWidth, 64u);
  EXPECT_EQ(lv[0].bucketCount, 3u * 4096);
  EXPECT_EQ(lv[1].level, 2u);
  EXPECT_EQ(lv[1].bucketWidth, 256u);
  EXPECT_EQ(lv.back().bucketCount, 1u);

  SampleStripResult r;
  SampleStripQuery q;
  q.thread = ThreadIdx(0);
  q.t0 = 1000;
  q.pixelWidth = 100;
  // Pixel span of 1000 ticks: the coarsest level with width <= 1000 is L2 (256); L4 is 1024 > 1000.
  q.t1 = q.t0 + 1000 * 100;
  ASSERT_TRUE(lod->sampleStrip(q, r).ok());
  EXPECT_EQ(r.level, 2);
  // Pixel span of exactly 1024: L4.
  q.t1 = q.t0 + 1024 * 100;
  ASSERT_TRUE(lod->sampleStrip(q, r).ok());
  EXPECT_EQ(r.level, 4);
  // Pixel span below the base width: direct.
  q.t1 = q.t0 + 63 * 100;
  ASSERT_TRUE(lod->sampleStrip(q, r).ok());
  EXPECT_EQ(r.level, kDirectLevel);
  // Whole trace in two pixels: the coarsest level whose width fits half the trace.
  q.t1 = q.t0 + total;
  q.pixelWidth = 2;
  ASSERT_TRUE(lod->sampleStrip(q, r).ok());
  uint8_t expected = kDirectLevel;
  for (const auto& l : lv)
    if (l.bucketWidth * 2 <= total) expected = static_cast<uint8_t>(l.level);
  EXPECT_EQ(r.level, expected);
}

TEST(LodBuild, PendingUntilBuiltThenConsistent) {
  TraceModel m = makeModel(0, 64u << 14, 64);
  for (uint64_t i = 0; i < 2000; ++i) addSample(m, 0, i * 64, static_cast<uint32_t>(2 + (i / 100) % 3));
  LodOptions opt;
  opt.forceBaseLevel = 0;
  auto prepared = TimelineLod::prepare(m, opt);
  ASSERT_TRUE(prepared.ok());
  TimelineLod& lod = *prepared;
  EXPECT_EQ(lod.readyThreadCount(), 0u);
  EXPECT_FALSE(lod.threadReady(ThreadIdx(0)));
  EXPECT_EQ(lod.progress(), 0.0f);

  SampleStripQuery wide;
  wide.thread = ThreadIdx(0);
  wide.t0 = 0;
  wide.t1 = 64u << 14;
  wide.pixelWidth = 64;
  SampleStripResult pending;
  ASSERT_TRUE(lod.sampleStrip(wide, pending).ok());
  EXPECT_TRUE(pending.pending);
  ASSERT_EQ(pending.columns.size(), 64u);
  for (const auto& c : pending.columns) EXPECT_FALSE(c.stack.valid());

  // Below the base level the answer is direct and available before the build.
  SampleStripQuery fine = wide;
  fine.t1 = 64 * 50;
  fine.pixelWidth = 100;
  SampleStripResult before;
  ASSERT_TRUE(lod.sampleStrip(fine, before).ok());
  EXPECT_FALSE(before.pending);
  EXPECT_EQ(before.level, kDirectLevel);
  EXPECT_TRUE(before.columns[0].stack.valid());

  std::vector<float> progress;
  ASSERT_TRUE(lod.build(nullptr, [&](float p) { progress.push_back(p); }).ok());
  EXPECT_TRUE(lod.threadReady(ThreadIdx(0)));
  EXPECT_EQ(lod.progress(), 1.0f);
  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.back(), 1.0f);
  for (size_t i = 1; i < progress.size(); ++i) EXPECT_GE(progress[i], progress[i - 1]);

  SampleStripResult after;
  ASSERT_TRUE(lod.sampleStrip(wide, after).ok());
  EXPECT_FALSE(after.pending);
  EXPECT_TRUE(after.columns[0].stack.valid());
  SampleStripResult afterFine;
  ASSERT_TRUE(lod.sampleStrip(fine, afterFine).ok());
  for (size_t i = 0; i < before.columns.size(); ++i) {
    EXPECT_EQ(before.columns[i].stack, afterFine.columns[i].stack);
    EXPECT_EQ(before.columns[i].share, afterFine.columns[i].share);
  }
  EXPECT_GT(lod.memoryBytes(), 0u);
}

TEST(LodBuild, Cancellation) {
  TraceModel m = makeModel(0, 64u << 12, 64);
  m.threads.resize(8);
  m.samplesByThread.resize(8);
  m.statesByThread.resize(8);
  for (size_t t = 0; t < 8; ++t)
    for (uint64_t i = 0; i < 4000; ++i) addSample(m, t, i * 64, static_cast<uint32_t>(1 + i % 4));
  LodOptions opt;
  opt.forceBaseLevel = 0;
  opt.threads = 1;

  std::atomic<bool> cancel{true};
  auto cancelled = buildLod(m, opt, &cancel);
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);

  // Cancel from the progress callback after the first thread: the rest stays unbuilt, and a later build()
  // finishes it with the same result as an uninterrupted build.
  auto prepared = TimelineLod::prepare(m, opt);
  ASSERT_TRUE(prepared.ok());
  cancel = false;
  auto r = prepared->build(&cancel, [&](float) { cancel = true; });
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
  EXPECT_GE(prepared->readyThreadCount(), 1u);
  EXPECT_LT(prepared->readyThreadCount(), 8u);
  cancel = false;
  ASSERT_TRUE(prepared->build(&cancel).ok());
  EXPECT_EQ(prepared->readyThreadCount(), 8u);
  auto full = buildLod(m, opt);
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(prepared->contentHash(), full->contentHash());
}

TEST(LodBuild, CorruptNodeCycleIsRejected) {
  TraceModel m = makeModel(0, 1000, 64);
  m.nodes[2].parent = CallstackIdx(3);
  m.nodes[3].parent = CallstackIdx(2);
  auto lod = buildLod(m);
  ASSERT_FALSE(lod.ok());
  EXPECT_EQ(lod.error().code, ErrorCode::CorruptData);
}

}  // namespace
}  // namespace tachyon::analysis
