// WP-64: TimelineLod::rowColumns (the fused band + strip pass) returns what stateBands + sampleStrip return,
// at every zoom (levels, the direct path, windows sticking out of the trace), for several level steps.
#include "analysis/lod.h"
#include "lod_test_util.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace tachyon::analysis {
namespace {

using model::TraceModel;
using testutil::fixture;

void expectSame(const StateBandResult& a, const StateBandResult& b, const std::string& what) {
  ASSERT_EQ(a.columns.size(), b.columns.size()) << what;
  EXPECT_EQ(a.pending, b.pending) << what;
  EXPECT_EQ(a.level, b.level) << what;
  for (size_t i = 0; i < a.columns.size(); ++i) {
    const BandColumn &x = a.columns[i], &y = b.columns[i];
    ASSERT_NEAR(x.running, y.running, 2e-5f) << what << " column " << i;
    ASSERT_NEAR(x.ready, y.ready, 2e-5f) << what << " column " << i;
    ASSERT_NEAR(x.waiting, y.waiting, 2e-5f) << what << " column " << i;
    ASSERT_NEAR(x.unknown, y.unknown, 2e-5f) << what << " column " << i;
    ASSERT_EQ(x.waitCategory, y.waitCategory) << what << " column " << i;
  }
}

void expectSame(const SampleStripResult& a, const SampleStripResult& b, const std::string& what) {
  ASSERT_EQ(a.columns.size(), b.columns.size()) << what;
  EXPECT_EQ(a.pending, b.pending) << what;
  EXPECT_EQ(a.level, b.level) << what;
  for (size_t i = 0; i < a.columns.size(); ++i) {
    const StripColumn &x = a.columns[i], &y = b.columns[i];
    ASSERT_EQ(x.stack.get(), y.stack.get()) << what << " column " << i;
    ASSERT_NEAR(x.share, y.share, 2e-5f) << what << " column " << i;
    ASSERT_NEAR(x.coverage, y.coverage, 2e-5f) << what << " column " << i;
  }
}

TEST(LodRowColumns, MatchesSeparateQueriesAtEveryZoom) {
  const TraceModel& m = fixture("medium");
  const uint64_t s = m.header.start_ts, e = m.header.end_ts, total = e - s;
  for (uint32_t step : {1u, 2u, 3u}) {
    LodOptions opt;
    opt.levelStep = step;
    auto lod = buildLod(m, opt);
    ASSERT_TRUE(lod.ok());
    std::vector<std::pair<uint64_t, uint64_t>> windows;
    // Every level width x {0.45 .. 4.2} per pixel, at three positions, plus windows sticking out of the trace.
    for (const LodLevelInfo& l : lod->levels()) {
      for (double f : {0.45, 0.9, 1.0, 1.7, 3.6, 3.99, 4.2}) {
        const uint64_t w = static_cast<uint64_t>(static_cast<double>(l.bucketWidth) * f * 301);
        if (w == 0 || w >= total) continue;
        for (double pos : {0.0, 0.37, 0.999}) {
          const uint64_t t0 = s + static_cast<uint64_t>(static_cast<double>(total - w) * pos);
          windows.emplace_back(t0, t0 + w);
        }
      }
    }
    windows.emplace_back(s, e);
    windows.emplace_back(s > 5000 ? s - 5000 : 0, s + total / 16);
    windows.emplace_back(e - total / 16, e + total / 16);
    windows.emplace_back(e + 10, e + 1000);  // entirely after the trace
    size_t fused = 0;
    for (const auto& [t0, t1] : windows) {
      for (uint32_t pixels : {301u, 1920u}) {
        for (size_t thread = 0; thread < m.threads.size(); thread += 5) {
          RowQuery rq;
          rq.thread = ThreadIdx(static_cast<uint32_t>(thread));
          rq.t0 = t0;
          rq.t1 = t1;
          rq.pixelWidth = pixels;
          StateBandResult fb;
          SampleStripResult fs;
          ASSERT_TRUE(lod->rowColumns(rq, fb, fs).ok());
          StateBandQuery bq;
          bq.thread = rq.thread;
          bq.t0 = t0;
          bq.t1 = t1;
          bq.pixelWidth = pixels;
          SampleStripQuery sq;
          sq.thread = rq.thread;
          sq.t0 = t0;
          sq.t1 = t1;
          sq.pixelWidth = pixels;
          StateBandResult b;
          SampleStripResult st;
          ASSERT_TRUE(lod->stateBands(bq, b).ok());
          ASSERT_TRUE(lod->sampleStrip(sq, st).ok());
          const std::string what = "step " + std::to_string(step) + " window [" + std::to_string(t0) + "," +
                                   std::to_string(t1) + ") px " + std::to_string(pixels) + " thread " +
                                   std::to_string(thread);
          expectSame(fb, b, what);
          expectSame(fs, st, what);
          if (fb.level != kDirectLevel) ++fused;
        }
      }
    }
    EXPECT_GT(fused, 100u) << "the fused path must be exercised";
  }
}

TEST(LodRowColumns, DirectModeAndPendingThreadsFallBack) {
  const TraceModel& m = fixture("medium");
  auto lod = TimelineLod::prepare(m);  // prepared, not built: every thread pending
  ASSERT_TRUE(lod.ok());
  RowQuery rq;
  rq.thread = ThreadIdx(0);
  rq.t0 = m.header.start_ts;
  rq.t1 = m.header.end_ts;
  rq.pixelWidth = 200;
  StateBandResult b;
  SampleStripResult s;
  ASSERT_TRUE(lod->rowColumns(rq, b, s).ok());
  EXPECT_TRUE(b.pending);
  EXPECT_TRUE(s.pending);
  ASSERT_TRUE(lod->build().ok());
  rq.mode = QueryMode::Direct;
  ASSERT_TRUE(lod->rowColumns(rq, b, s).ok());
  EXPECT_EQ(b.level, kDirectLevel);
  EXPECT_EQ(s.level, kDirectLevel);
  rq.pixelWidth = 0;
  EXPECT_FALSE(lod->rowColumns(rq, b, s).ok());
  rq.pixelWidth = 10;
  rq.thread = ThreadIdx(9999);
  EXPECT_FALSE(lod->rowColumns(rq, b, s).ok());
}

}  // namespace
}  // namespace tachyon::analysis
