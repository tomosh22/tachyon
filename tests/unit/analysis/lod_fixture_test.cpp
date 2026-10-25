// TimelineLod against the tracegen fixtures (tiny, medium): brute-force comparison of every bucket of every
// level, exactness of the direct path, consistency of the LOD and direct paths at level boundaries, span
// invariants at several zooms, determinism.
#include "analysis/lod.h"
#include "analysis/wait_reasons.h"
#include "lod_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <tuple>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using model::TraceModel;
using testutil::Ext;
using testutil::extents;
using testutil::fixture;
using testutil::pathOf;
using testutil::round255;

constexpr float kEps = 2e-6f;

struct Window {
  uint64_t t0, t1;
  uint32_t pixels;
};

// Zooms from the whole trace down to 1 ms, one off-centre and one that sticks out of the trace.
std::vector<Window> zooms(const TraceModel& m, uint32_t pixels) {
  const uint64_t s = m.header.start_ts, e = m.header.end_ts, total = e - s;
  const uint64_t clock = m.header.clock_frequency_hz;
  std::vector<Window> w;
  w.push_back({s, e, pixels});
  w.push_back({s, s + total / 8, pixels});
  w.push_back({s + total / 3, s + total / 3 + total / 64, pixels});
  w.push_back({s + total / 2, s + total / 2 + clock / 20, pixels});                    // 50 ms
  w.push_back({s + total / 2 + 12345, s + total / 2 + 12345 + clock / 1000, pixels});  // 1 ms
  w.push_back({s + total / 5, s + total / 5 + 100, std::min<uint32_t>(pixels, 64)});   // 100 ticks
  w.push_back({s > 5000 ? s - 5000 : 0, s + total / 16, 97});  // starts before the trace
  w.push_back({e - total / 16, e + total / 16, 331});          // ends after the trace
  return w;
}

// ---- brute force ----
struct BruteBand {
  uint64_t running = 0, ready = 0, waiting = 0;
  uint64_t cat[5] = {};
};

BruteBand bruteBand(const TraceModel& m, size_t thread, uint64_t a, uint64_t b) {
  BruteBand r;
  for (const auto& iv : m.statesByThread[thread]) {
    const uint64_t lo = std::max(iv.start, a), hi = std::min(iv.end, b);
    if (hi <= lo) continue;
    switch (iv.state) {
      case ThreadState::Running: r.running += hi - lo; break;
      case ThreadState::Ready: r.ready += hi - lo; break;
      case ThreadState::Waiting:
        r.waiting += hi - lo;
        r.cat[static_cast<size_t>(waitReasonCategory(m.header.platform, iv.wait_reason))] += hi - lo;
        break;
      case ThreadState::Unknown: break;
    }
  }
  return r;
}

uint8_t bruteCategory(const BruteBand& r) {
  uint8_t best = kNoWaitCategory;
  uint64_t bestV = 0;
  for (uint8_t c = 0; c < 5; ++c)
    if (r.cat[c] > bestV) {
      bestV = r.cat[c];
      best = c;
    }
  return best;
}

struct BruteStrip {
  uint32_t dominant = kInvalidIdx;
  uint64_t domTicks = 0, cover = 0;
  uint32_t lca = kInvalidIdx;
  uint32_t maxDepth = 0;
};

// `ext` sorted by start, non-overlapping.
BruteStrip bruteStrip(const TraceModel& m, const std::vector<Ext>& ext, uint64_t a, uint64_t b) {
  BruteStrip r;
  std::map<uint32_t, uint64_t> hist;
  auto it = std::partition_point(ext.begin(), ext.end(), [&](const Ext& e) { return e.e <= a; });
  for (; it != ext.end() && it->s < b; ++it) {
    const uint64_t lo = std::max(it->s, a), hi = std::min(it->e, b);
    if (hi <= lo) continue;
    hist[it->leaf] += hi - lo;
    r.cover += hi - lo;
    r.lca = r.lca == kInvalidIdx ? it->leaf : testutil::bruteLca(m, r.lca, it->leaf);
    r.maxDepth = std::max<uint32_t>(r.maxDepth, static_cast<uint32_t>(pathOf(m, it->leaf).size()));
  }
  for (const auto& [leaf, ticks] : hist)
    if (ticks > r.domTicks) {
      r.domTicks = ticks;
      r.dominant = leaf;
    }
  return r;
}

// ---- every bucket of every level ----
void checkAllBuckets(const TraceModel& m, const LodOptions& opt) {
  auto lod = buildLod(m, opt);
  ASSERT_TRUE(lod.ok()) << lod.error().toString();
  const uint64_t start = m.header.start_ts, total = m.header.end_ts - m.header.start_ts;
  const auto& levels = lod->levels();
  ASSERT_FALSE(levels.empty());
  EXPECT_EQ(levels.back().bucketCount, 1u);
  for (size_t t = 0; t < m.threads.size(); ++t) {
    const auto ext = extents(m, t);
    for (size_t l = 0; l < levels.size(); ++l) {
      const uint64_t w = levels[l].bucketWidth;
      ASSERT_EQ(w, 64ull << levels[l].level);
      ASSERT_EQ(levels[l].bucketCount, (total + w - 1) / w);
      for (uint32_t i = 0; i < levels[l].bucketCount; ++i) {
        const uint64_t bs = start + i * w, be = std::min(bs + w, m.header.end_ts), ew = be - bs;
        LodBucket got;
        ASSERT_TRUE(lod->bucket(ThreadIdx(static_cast<uint32_t>(t)), l, i, got));
        const BruteBand band = bruteBand(m, t, bs, be);
        ASSERT_EQ(got.running, round255(band.running, ew))
            << "t" << t << " L" << levels[l].level << " b" << i;
        ASSERT_EQ(got.ready, round255(band.ready, ew)) << "t" << t << " L" << levels[l].level << " b" << i;
        ASSERT_EQ(got.waiting, round255(band.waiting, ew))
            << "t" << t << " L" << levels[l].level << " b" << i;
        ASSERT_EQ(got.waitCategory, bruteCategory(band)) << "t" << t << " L" << levels[l].level << " b" << i;
        const BruteStrip s = bruteStrip(m, ext, bs, be);
        ASSERT_EQ(got.dominant.get(), s.dominant) << "t" << t << " L" << levels[l].level << " b" << i;
        ASSERT_EQ(got.commonPrefix.get(), s.lca) << "t" << t << " L" << levels[l].level << " b" << i;
        if (s.cover == 0) {
          ASSERT_EQ(got.share, 0);
          ASSERT_EQ(got.coverage, 0);
          ASSERT_EQ(got.maxDepth, 0);
        } else {
          ASSERT_EQ(got.share, round255(s.domTicks, s.cover))
              << "t" << t << " L" << levels[l].level << " b" << i;
          ASSERT_EQ(got.coverage, std::max<uint8_t>(1, round255(s.cover, ew)));
          ASSERT_EQ(got.maxDepth, s.maxDepth);
        }
      }
    }
  }
}

TEST(LodMipmap, EveryBucketMatchesBruteForceOnTinyDefaultOptions) {
  const TraceModel& m = fixture("tiny");
  LodOptions opt;
  checkAllBuckets(m, opt);
}

TEST(LodMipmap, EveryBucketMatchesBruteForceOnTinyFineLevelsStepOne) {
  const TraceModel& m = fixture("tiny");
  LodOptions opt;
  opt.forceBaseLevel = 3;  // 512-tick buckets, every level number up to the top
  opt.levelStep = 1;
  checkAllBuckets(m, opt);
}

TEST(LodMipmap, EveryBucketMatchesBruteForceOnTinyStepThree) {
  const TraceModel& m = fixture("tiny");
  LodOptions opt;
  opt.forceBaseLevel = 2;
  opt.levelStep = 3;
  checkAllBuckets(m, opt);
}

// ---- direct path is exact ----
void checkDirectExact(const TraceModel& m, size_t thread, const Window& w, const TimelineLod& lod) {
  const auto ext = extents(m, thread);
  StateBandQuery bq;
  bq.thread = ThreadIdx(static_cast<uint32_t>(thread));
  bq.t0 = w.t0;
  bq.t1 = w.t1;
  bq.pixelWidth = w.pixels;
  bq.mode = QueryMode::Direct;
  StateBandResult br;
  ASSERT_TRUE(lod.stateBands(bq, br).ok());
  ASSERT_EQ(br.columns.size(), w.pixels);
  SampleStripQuery sq;
  sq.thread = bq.thread;
  sq.t0 = w.t0;
  sq.t1 = w.t1;
  sq.pixelWidth = w.pixels;
  sq.mode = QueryMode::Direct;
  SampleStripResult sr;
  ASSERT_TRUE(lod.sampleStrip(sq, sr).ok());
  ASSERT_EQ(sr.columns.size(), w.pixels);
  for (uint32_t c = 0; c < w.pixels; ++c) {
    const uint64_t a = pixelEdge(w.t0, w.t1, w.pixels, c), b = pixelEdge(w.t0, w.t1, w.pixels, c + 1);
    if (b == a) continue;
    const double width = static_cast<double>(b - a);
    const BruteBand band = bruteBand(m, thread, a, b);
    const BandColumn& bc = br.columns[c];
    ASSERT_NEAR(bc.running, static_cast<double>(band.running) / width, kEps) << "col " << c;
    ASSERT_NEAR(bc.ready, static_cast<double>(band.ready) / width, kEps) << "col " << c;
    ASSERT_NEAR(bc.waiting, static_cast<double>(band.waiting) / width, kEps) << "col " << c;
    ASSERT_NEAR(bc.running + bc.ready + bc.waiting + bc.unknown, 1.0, 1e-5);
    ASSERT_EQ(bc.waitCategory, bruteCategory(band)) << "col " << c;
    const BruteStrip s = bruteStrip(m, ext, a, b);
    const StripColumn& sc = sr.columns[c];
    ASSERT_EQ(sc.stack.get(), s.dominant) << "col " << c;
    if (s.cover) {
      ASSERT_NEAR(sc.share, static_cast<double>(s.domTicks) / static_cast<double>(s.cover), kEps);
      ASSERT_NEAR(sc.coverage, static_cast<double>(s.cover) / width, kEps);
    }
  }
}

TEST(LodDirect, BandsAndStripsAreExactOnTiny) {
  const TraceModel& m = fixture("tiny");
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  for (size_t t = 0; t < m.threads.size(); ++t)
    for (const Window& w : zooms(m, 211)) checkDirectExact(m, t, w, *lod);
}

TEST(LodDirect, BandsAndStripsAreExactOnMedium) {
  const TraceModel& m = fixture("medium");
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  // The whole-trace window is O(samples) per thread for the brute force, so use few threads and the narrower
  // zooms; the whole trace is covered on tiny.
  for (size_t t : {size_t(0), size_t(7)}) {
    const auto z = zooms(m, 157);
    for (size_t i = 2; i < z.size(); ++i) checkDirectExact(m, t, z[i], *lod);
  }
}

// ---- LOD path against brute force ----
TEST(LodQueries, StateBandsWithinBucketBoundOfBruteForce) {
  for (const char* name : {"tiny", "medium"}) {
    const TraceModel& m = fixture(name);
    auto lod = buildLod(m);
    ASSERT_TRUE(lod.ok());
    const size_t threads = std::string(name) == "tiny" ? m.threads.size() : 3;
    for (size_t t = 0; t < threads; ++t) {
      for (const Window& w : zooms(m, 480)) {
        StateBandQuery q;
        q.thread = ThreadIdx(static_cast<uint32_t>(t));
        q.t0 = w.t0;
        q.t1 = w.t1;
        q.pixelWidth = w.pixels;
        StateBandResult r;
        ASSERT_TRUE(lod->stateBands(q, r).ok());
        const double span = static_cast<double>(w.t1 - w.t0) / w.pixels;
        double bound = 0.02;
        if (r.level != kDirectLevel) bound = 2.0 * static_cast<double>(64ull << r.level) / span + 0.02;
        for (uint32_t c = 0; c < w.pixels; ++c) {
          const uint64_t a = pixelEdge(w.t0, w.t1, w.pixels, c), b = pixelEdge(w.t0, w.t1, w.pixels, c + 1);
          const BruteBand band = bruteBand(m, t, a, b);
          const double width = static_cast<double>(b - a);
          ASSERT_NEAR(r.columns[c].running, static_cast<double>(band.running) / width, std::min(bound, 1.0))
              << name << " col " << c;
          ASSERT_NEAR(r.columns[c].ready, static_cast<double>(band.ready) / width, std::min(bound, 1.0))
              << name << " col " << c;
          ASSERT_NEAR(r.columns[c].waiting, static_cast<double>(band.waiting) / width, std::min(bound, 1.0))
              << name << " col " << c;
        }
      }
    }
  }
}

// ---- LOD and direct agree where a pixel column is one bucket ----
std::vector<std::tuple<uint32_t, uint64_t, uint64_t, uint32_t, uint32_t>> key(const TimelineResult& r) {
  std::vector<std::tuple<uint32_t, uint64_t, uint64_t, uint32_t, uint32_t>> v;
  for (const auto& s : r.spans) v.emplace_back(s.depth, s.t0, s.t1, s.node.get(), s.mergedCount);
  return v;
}

void checkBoundary(const TraceModel& m, const LodOptions& opt, size_t maxThreads) {
  auto lod = buildLod(m, opt);
  ASSERT_TRUE(lod.ok());
  const auto& levels = lod->levels();
  for (size_t li = 0; li < levels.size(); ++li) {
    if (levels[li].bucketCount < 3) continue;
    const uint64_t w = levels[li].bucketWidth;
    const uint32_t n = std::min<uint32_t>(levels[li].bucketCount - 1, 200);
    // Window of n whole buckets: first at bucket 1 (or 0 when there are only a few), all full-width.
    const uint32_t b0 = (levels[li].bucketCount - 1 - n) >= 1 ? 1 : 0;
    const uint64_t t0 = m.header.start_ts + b0 * w, t1 = t0 + n * w;
    for (size_t t = 0; t < std::min(m.threads.size(), maxThreads); ++t) {
      const ThreadIdx th(static_cast<uint32_t>(t));
      StateBandQuery bq{th, t0, t1, n, QueryMode::Auto};
      StateBandResult lodB, dirB;
      ASSERT_TRUE(lod->stateBands(bq, lodB).ok());
      EXPECT_EQ(lodB.level, levels[li].level);
      bq.mode = QueryMode::Direct;
      ASSERT_TRUE(lod->stateBands(bq, dirB).ok());
      SampleStripQuery sq{th, t0, t1, n, QueryMode::Auto};
      SampleStripResult lodS, dirS;
      ASSERT_TRUE(lod->sampleStrip(sq, lodS).ok());
      EXPECT_EQ(lodS.level, levels[li].level);
      sq.mode = QueryMode::Direct;
      ASSERT_TRUE(lod->sampleStrip(sq, dirS).ok());
      TimelineQuery tq;
      tq.thread = th;
      tq.depthBegin = 1;
      tq.depthEnd = 64;
      tq.t0 = t0;
      tq.t1 = t1;
      tq.pixelWidth = n;
      TimelineResult lodT, dirT;
      ASSERT_TRUE(lod->timeline(tq, lodT).ok());
      // Below one sample interval per pixel the flame chart is always computed exactly from the samples.
      EXPECT_EQ(lodT.level,
                levels[li].bucketWidth >= m.sampleIntervalTicks ? levels[li].level : kDirectLevel);
      tq.mode = QueryMode::Direct;
      ASSERT_TRUE(lod->timeline(tq, dirT).ok());
      EXPECT_EQ(key(lodT), key(dirT)) << "thread " << t << " level " << levels[li].level;
      for (uint32_t c = 0; c < n; ++c) {
        const BandColumn &a = lodB.columns[c], &b = dirB.columns[c];
        // Each fraction is rounded to 1/255; three rounded fractions can sum to 256/255 and are renormalised.
        ASSERT_NEAR(a.running, b.running, 1.5f / 255 + kEps);
        ASSERT_NEAR(a.ready, b.ready, 1.5f / 255 + kEps);
        ASSERT_NEAR(a.waiting, b.waiting, 1.5f / 255 + kEps);
        ASSERT_EQ(a.waitCategory, b.waitCategory)
            << "thread " << t << " level " << levels[li].level << " col " << c;
        ASSERT_EQ(lodS.columns[c].stack, dirS.columns[c].stack)
            << "thread " << t << " level " << levels[li].level << " col " << c;
        if (dirS.columns[c].stack.valid()) {
          ASSERT_NEAR(lodS.columns[c].share, dirS.columns[c].share, 0.5f / 255 + kEps);
          ASSERT_NEAR(lodS.columns[c].coverage, dirS.columns[c].coverage, 0.5f / 255 + kEps + 1.0f / 255);
        }
      }
    }
  }
}

TEST(LodBoundary, LevelQueriesEqualDirectOnTiny) {
  LodOptions opt;
  checkBoundary(fixture("tiny"), opt, 99);
  opt.forceBaseLevel = 2;
  opt.levelStep = 1;
  checkBoundary(fixture("tiny"), opt, 99);
}

TEST(LodBoundary, LevelQueriesEqualDirectOnMedium) {
  LodOptions opt;
  checkBoundary(fixture("medium"), opt, 2);
}

// ---- span invariants ----
void checkSpans(const TraceModel& m, const TimelineLod& lod, size_t thread, const Window& w, QueryMode mode) {
  TimelineQuery q;
  q.thread = ThreadIdx(static_cast<uint32_t>(thread));
  q.depthBegin = 1;
  q.depthEnd = 100;
  q.t0 = w.t0;
  q.t1 = w.t1;
  q.pixelWidth = w.pixels;
  q.mode = mode;
  TimelineResult r;
  ASSERT_TRUE(lod.timeline(q, r).ok());
  const std::string ctx = "thread " + std::to_string(thread) + " [" + std::to_string(w.t0) + "," +
                          std::to_string(w.t1) + ") px " + std::to_string(w.pixels) + " mode " +
                          std::to_string(int(mode));
  std::vector<uint64_t> edges;
  for (uint32_t c = 0; c <= w.pixels; ++c) edges.push_back(pixelEdge(w.t0, w.t1, w.pixels, c));
  const auto ext = extents(m, thread);
  const auto& sc = m.samplesByThread[thread];
  // One pixel at least as wide as a sample interval: spans lie on pixel edges; else they follow the samples.
  const bool columnRegime = w.t1 - w.t0 >= m.sampleIntervalTicks * w.pixels;

  std::map<uint32_t, std::vector<const TimelineSpan*>> byDepth;
  for (size_t i = 0; i < r.spans.size(); ++i) {
    const TimelineSpan& s = r.spans[i];
    ASSERT_GE(s.depth, 1u) << ctx;
    ASSERT_LT(s.t0, s.t1) << ctx;
    ASSERT_GE(s.t0, w.t0) << ctx;
    ASSERT_LE(s.t1, w.t1) << ctx;
    if (columnRegime) {
      ASSERT_TRUE(std::binary_search(edges.begin(), edges.end(), s.t0)) << ctx;
      ASSERT_TRUE(std::binary_search(edges.begin(), edges.end(), s.t1)) << ctx;
    }
    if (i > 0) {
      const TimelineSpan& p = r.spans[i - 1];
      ASSERT_TRUE(p.depth < s.depth || (p.depth == s.depth && p.t1 <= s.t0))
          << "sorted/non-overlapping " << ctx;
    }
    byDepth[s.depth].push_back(&s);
    const uint64_t first =
        static_cast<uint64_t>(std::lower_bound(sc.ts.begin(), sc.ts.end(), s.t0) - sc.ts.begin());
    const uint64_t last =
        static_cast<uint64_t>(std::lower_bound(sc.ts.begin(), sc.ts.end(), s.t1) - sc.ts.begin());
    if (s.node.valid()) {
      ASSERT_EQ(s.mergedCount, 1u) << ctx;
      ASSERT_EQ(pathOf(m, s.node.get()).size(), s.depth) << ctx;
    } else if (columnRegime) {
      ASSERT_EQ(s.mergedCount, last - first) << ctx;
    } else {
      ASSERT_GE(s.mergedCount, 2u) << ctx;
      ASSERT_LT(s.t1 - s.t0 + 0, 1ull << 62);
    }
  }
  // Uniform spans: every sample overlapping them has the span's node as prefix at that depth.
  size_t checked = 0;
  const size_t stride = std::max<size_t>(1, ext.size() / 4000);
  for (size_t i = 0; i < ext.size(); i += stride) {
    const Ext& e = ext[i];
    const uint64_t lo = std::max(e.s, w.t0), hi = std::min(e.e, w.t1);
    if (hi <= lo) continue;
    ++checked;
    const auto path = pathOf(m, e.leaf);
    for (uint32_t d = 1; d <= path.size(); ++d) {
      // coverage: the first tick of the overlap lies in a span of this depth
      const auto& v = byDepth[d];
      auto it = std::upper_bound(v.begin(), v.end(), lo,
                                 [](uint64_t t, const TimelineSpan* s) { return t < s->t0; });
      ASSERT_NE(it, v.begin()) << "uncovered sample at depth " << d << " " << ctx;
      const TimelineSpan& s = **(it - 1);
      ASSERT_LT(lo, s.t1) << "uncovered sample at depth " << d << " " << ctx;
      if (s.node.valid()) ASSERT_EQ(s.node.get(), path[d - 1]) << ctx;
    }
  }
  if (mode == QueryMode::Direct) {
    // No span where no sample has a prefix (direct path is exact at pixel granularity).
    for (const auto& s : r.spans) {
      bool found = false;
      auto it = std::partition_point(ext.begin(), ext.end(), [&](const Ext& e) { return e.e <= s.t0; });
      for (; it != ext.end() && it->s < s.t1 && !found; ++it) found = pathOf(m, it->leaf).size() >= s.depth;
      ASSERT_TRUE(found) << "span without samples at depth " << s.depth << " " << ctx;
    }
  }
  (void)checked;
}

TEST(LodSpansFixture, CoverWithoutOverlapOnTiny) {
  const TraceModel& m = fixture("tiny");
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  auto fine = buildLod(m, [] {
    LodOptions o;
    o.forceBaseLevel = 2;
    o.levelStep = 1;
    return o;
  }());
  ASSERT_TRUE(fine.ok());
  for (size_t t = 0; t < m.threads.size(); ++t) {
    for (uint32_t px : {1920u, 300u, 7u}) {
      for (const Window& w : zooms(m, px)) {
        for (QueryMode mode : {QueryMode::Auto, QueryMode::Direct}) {
          checkSpans(m, *lod, t, w, mode);
          checkSpans(m, *fine, t, w, mode);
        }
      }
    }
  }
}

TEST(LodSpansFixture, CoverWithoutOverlapOnMedium) {
  const TraceModel& m = fixture("medium");
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  for (size_t t : {size_t(0), size_t(9)}) {
    for (const Window& w : zooms(m, 1920)) {
      // The direct whole-trace window is O(samples x depth) in the checker; Auto covers it.
      checkSpans(m, *lod, t, w, QueryMode::Auto);
      if (w.t1 - w.t0 < 700000000ull) checkSpans(m, *lod, t, w, QueryMode::Direct);
    }
  }
}

TEST(LodSpansFixture, NarrowSpansAreMergedNotEmittedBelowOnePixel) {
  const TraceModel& m = fixture("tiny");
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  const uint64_t s = m.header.start_ts;
  for (uint32_t px : {50u, 400u}) {  // pixels at least one sample interval wide: column regime
    TimelineQuery q;
    q.thread = ThreadIdx(0);
    q.depthEnd = 64;
    q.t0 = s;
    q.t1 = s + 2000000;
    q.pixelWidth = px;
    TimelineResult r;
    ASSERT_TRUE(lod->timeline(q, r).ok());
    ASSERT_FALSE(r.spans.empty());
    bool anyMerged = false;
    for (const auto& sp : r.spans) {
      EXPECT_GE(sp.t1 - sp.t0, (q.t1 - q.t0) / px);  // at least one pixel (floor)
      anyMerged |= !sp.node.valid();
    }
    EXPECT_TRUE(anyMerged);
  }
}

// ---- determinism ----
TEST(LodDeterminism, SameContentForAnyWorkerCountAndQueriesRepeat) {
  for (const char* name : {"tiny", "medium"}) {
    const TraceModel& m = fixture(name);
    uint64_t hash = 0;
    for (unsigned threads : {1u, 3u, 8u}) {
      LodOptions opt;
      opt.threads = threads;
      auto lod = buildLod(m, opt);
      ASSERT_TRUE(lod.ok());
      if (hash == 0) hash = lod->contentHash();
      EXPECT_EQ(lod->contentHash(), hash) << name << " threads " << threads;
      EXPECT_EQ(lod->readyThreadCount(), m.threads.size());

      const Window w = zooms(m, 640)[2];
      TimelineQuery q;
      q.thread = ThreadIdx(1);
      q.depthEnd = 64;
      q.t0 = w.t0;
      q.t1 = w.t1;
      q.pixelWidth = w.pixels;
      TimelineResult a, b;
      ASSERT_TRUE(lod->timeline(q, a).ok());
      ASSERT_TRUE(lod->timeline(q, b).ok());
      EXPECT_EQ(key(a), key(b));
    }
  }
}

}  // namespace
}  // namespace tachyon::analysis
