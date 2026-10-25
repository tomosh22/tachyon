// WP-35: Stats (histogram, quartiles), functionStats instance rule, threadStateTotals.
#include "analysis/query.h"

#include "aggregation_test_util.h"
#include "analysis/stats.h"
#include "analysis/thread_states.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::TestFunction;

const std::vector<TestFunction> kFns = {{"A", 0x1000, 0x100}, {"B", 0x1100, 0x100}, {"C", 0x1200, 0x100}};
constexpr uint64_t kA = 0x1000, kB = 0x1100, kC = 0x1200;

FunctionKey fk(uint64_t start) {
  return FunctionKey{ModuleIdx(0), start};
}

// A Stats built by makeStats from an unsorted list, as the reference for quartile definitions.
double quantile(std::vector<uint64_t> v, double q) {
  std::sort(v.begin(), v.end());
  const double h = q * static_cast<double>(v.size() - 1);
  const size_t lo = static_cast<size_t>(h);
  const size_t hi = std::min(lo + 1, v.size() - 1);
  return static_cast<double>(v[lo]) +
         (h - static_cast<double>(lo)) * (static_cast<double>(v[hi]) - static_cast<double>(v[lo]));
}

TEST(StatsBuckets, EdgesAreLog2AndConsistent) {
  EXPECT_EQ(statsBucketOf(0), 0u);
  EXPECT_EQ(statsBucketOf(1), 1u);
  EXPECT_EQ(statsBucketOf(2), 2u);
  EXPECT_EQ(statsBucketOf(3), 2u);
  EXPECT_EQ(statsBucketOf(4), 3u);
  EXPECT_EQ(statsBucketOf(255), 8u);
  EXPECT_EQ(statsBucketOf(256), 9u);
  EXPECT_EQ(statsBucketOf(UINT64_MAX), 64u);
  EXPECT_EQ(statsBucketOf(uint64_t{1} << 63), 64u);
  EXPECT_LT(statsBucketOf(UINT64_MAX), kStatsMaxBuckets);
  for (uint32_t b = 0; b < kStatsMaxBuckets; ++b) {
    EXPECT_EQ(statsBucketOf(statsBucketLower(b)), b) << b;
    if (b < 64) {
      EXPECT_EQ(statsBucketOf(statsBucketUpper(b)), b + 1) << b;
      EXPECT_EQ(statsBucketOf(statsBucketUpper(b) - 1), b) << b;
    }
  }
  EXPECT_EQ(statsBucketUpper(64), UINT64_MAX);
}

TEST(MakeStats, EmptyIsAllZero) {
  std::vector<uint64_t> d;
  const Stats s = makeStats(d);
  EXPECT_EQ(s.count, 0u);
  EXPECT_EQ(s.min, 0);
  EXPECT_EQ(s.max, 0);
  EXPECT_EQ(s.mean, 0);
  EXPECT_TRUE(s.histogram.empty());
}

TEST(MakeStats, OddAndEvenCounts) {
  std::vector<uint64_t> odd = {50, 10, 30, 20, 40};
  Stats s = makeStats(odd);
  EXPECT_EQ(s.count, 5u);
  EXPECT_EQ(s.min, 10);
  EXPECT_EQ(s.max, 50);
  EXPECT_EQ(s.median, 30);
  EXPECT_EQ(s.p25, 20);
  EXPECT_EQ(s.p75, 40);
  EXPECT_EQ(s.mean, 30);
  EXPECT_EQ(s.totalTicks, 150u);

  std::vector<uint64_t> even = {4, 1, 3, 2};  // lower order statistics: median 2, p25 = 1, p75 = 3
  s = makeStats(even);
  EXPECT_DOUBLE_EQ(s.median, 2);
  EXPECT_DOUBLE_EQ(s.p25, 1);
  EXPECT_DOUBLE_EQ(s.p75, 3);

  std::vector<uint64_t> one = {7};
  s = makeStats(one);
  EXPECT_EQ(s.p25, 7);
  EXPECT_EQ(s.median, 7);
  EXPECT_EQ(s.p75, 7);
}

TEST(MakeStats, MatchesSortedReferenceOnRandomData) {
  uint64_t x = 88172645463325252ull;
  for (size_t n : {2u, 3u, 7u, 100u, 1001u, 4096u}) {
    std::vector<uint64_t> d(n);
    for (auto& v : d) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      v = x % 100000;
    }
    const std::vector<uint64_t> copy = d;
    const Stats s = makeStats(d);
    EXPECT_DOUBLE_EQ(s.p25, quantile(copy, 0.25)) << n;
    EXPECT_DOUBLE_EQ(s.median, quantile(copy, 0.5)) << n;
    EXPECT_DOUBLE_EQ(s.p75, quantile(copy, 0.75)) << n;
    EXPECT_EQ(s.min, static_cast<double>(*std::min_element(copy.begin(), copy.end())));
    EXPECT_EQ(s.max, static_cast<double>(*std::max_element(copy.begin(), copy.end())));
    EXPECT_EQ(std::accumulate(s.histogram.begin(), s.histogram.end(), uint64_t{0}), n);
  }
}

TEST(MakeStats, HistogramBuckets) {
  std::vector<uint64_t> d = {0, 1, 2, 3, 4, 7, 8, 100, 100};
  const Stats s = makeStats(d);
  ASSERT_EQ(s.histogram.size(), 8u);  // up to bucket(100) = 7
  EXPECT_EQ(s.histogram[0], 1u);      // {0}
  EXPECT_EQ(s.histogram[1], 1u);      // {1}
  EXPECT_EQ(s.histogram[2], 2u);      // [2,4)
  EXPECT_EQ(s.histogram[3], 2u);      // [4,8)
  EXPECT_EQ(s.histogram[4], 1u);      // [8,16)
  EXPECT_EQ(s.histogram[5], 0u);
  EXPECT_EQ(s.histogram[6], 0u);
  EXPECT_EQ(s.histogram[7], 2u);  // [64,128)
}

// One thread, always Running over [0, 10000); sample interval 100.
struct Fix {
  HandModel hm;
  CallstackIdx ab, a, c;
  Fix() {
    hm.state(0, 0, 10000, ThreadState::Running);
    ab = hm.stack({kA + 0x10, kB + 0x10});
    a = hm.stack({kA + 0x10});
    c = hm.stack({kC + 0x10});
  }
  void sample(uint64_t ts, CallstackIdx cs, uint16_t flags = 0) { hm.sample(0, ts, cs, flags); }
  void finish() { hm.finish(kFns); }
};

TEST(FunctionStats, RunsSplitOnAbsenceAndOnGapsAboveTwoIntervals) {
  Fix f;
  for (uint64_t ts : {0, 100, 200}) f.sample(ts, f.ab);  // B run 1: 300
  for (uint64_t ts : {401, 500}) f.sample(ts, f.ab);     // gap 201 > 200: B run 2: 99 + 100 = 199
  f.sample(600, f.a);                                    // B absent: ends the run
  for (uint64_t ts : {700, 800}) f.sample(ts, f.ab);     // B run 3: 200
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);

  auto b = functionStats(ctx, {}, fk(kB));
  ASSERT_TRUE(b.ok()) << b.error().toString();
  EXPECT_EQ(b->count, 3u);
  EXPECT_EQ(b->min, 199);
  EXPECT_EQ(b->max, 300);
  EXPECT_EQ(b->totalTicks, 699u);
  EXPECT_DOUBLE_EQ(b->mean, 233.0);
  EXPECT_EQ(b->median, 200);
  EXPECT_DOUBLE_EQ(b->p25, 199);
  EXPECT_DOUBLE_EQ(b->p75, 200);
  ASSERT_EQ(b->histogram.size(), 10u);  // 300 -> bucket 9
  EXPECT_EQ(b->histogram[8], 2u);       // 199, 200 in [128, 256)
  EXPECT_EQ(b->histogram[9], 1u);       // 300 in [256, 512)

  // A is in every sample: a gap of 201 splits it into two runs.
  auto a = functionStats(ctx, {}, fk(kA));
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->count, 2u);
  EXPECT_EQ(a->min, 300);
  EXPECT_EQ(a->max, 499);  // 800 - 401 + 100
  EXPECT_EQ(a->unterminated, 0u);

  // A function that is in no sample.
  auto none = functionStats(ctx, {}, fk(0x9000));
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->count, 0u);
}

TEST(FunctionStats, GapOfExactlyTwoIntervalsContinuesTheRun) {
  Fix f;
  f.sample(0, f.ab);
  f.sample(200, f.ab);   // gap 200 == 2 intervals: same run
  f.sample(1000, f.ab);  // gap 800: new run
  f.sample(1201, f.ab);  // gap 201: new run
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);
  auto s = functionStats(ctx, {}, fk(kB));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->count, 3u);
  EXPECT_EQ(s->max, 300);  // 200 - 0 + 100
  EXPECT_EQ(s->min, 100);
}

TEST(FunctionStats, DepthChangeStartsANewInstanceAndRecursionKeepsTheOutermostDepth) {
  Fix f;
  const CallstackIdx cA = f.hm.stack({kC + 0x10, kA + 0x10});                    // A at depth 1
  const CallstackIdx recursive = f.hm.stack({kA + 0x10, kA + 0x20, kA + 0x30});  // outermost A at depth 0
  const CallstackIdx viaB = f.hm.stack({kA + 0x10, kB + 0x10, kA + 0x20});       // outermost A at depth 0
  f.sample(0, f.a);                                                              // depth 0
  f.sample(100, recursive);                                                      // depth 0: same instance
  f.sample(200, viaB);                                                           // depth 0: same instance
  f.sample(300, cA);                                                             // depth 1: new instance
  f.sample(400, cA);
  f.sample(500, f.a);  // depth 0 again: new instance
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);
  auto s = functionStats(ctx, {}, fk(kA));
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(s->count, 3u);
  EXPECT_EQ(s->min, 100);  // the last sample alone
  EXPECT_EQ(s->max, 300);  // samples 0, 100, 200
  EXPECT_EQ(s->totalTicks, 300u + 200u + 100u);
}

TEST(FunctionStats, ZeroWeightSamplesAreSkippedAndWeightIsClipped) {
  HandModel hm;
  hm.state(0, 0, 450, ThreadState::Running);
  hm.state(0, 450, 600, ThreadState::Unknown);
  hm.state(0, 600, 1000, ThreadState::Running);
  const CallstackIdx ab = hm.stack({kA + 0x10, kB + 0x10});
  const CallstackIdx c = hm.stack({kC + 0x10});
  hm.sample(0, 300, ab);  // weight 100
  hm.sample(0, 400, ab);  // weight 50: clipped to the end of the Running interval
  hm.sample(0, 500, c);   // weight 0 (Unknown): no B in it, but it breaks nothing
  hm.sample(0, 600, ab);  // gap from 400 is 200: continues; weight 100
  hm.sample(0, 700, ab);
  hm.finish(kFns);
  QueryContext ctx(hm.m, *hm.svc);
  auto s = functionStats(ctx, {}, fk(kB));
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(s->count, 1u);
  EXPECT_EQ(s->min, 700 - 300 + 100);

  // Only zero-weight samples: no instance at all.
  HandModel z;
  z.state(0, 0, 1000, ThreadState::Waiting);
  z.sample(0, 100, z.stack({kA + 0x10}));
  z.sample(0, 200, z.stack({kA + 0x10}));
  z.finish(kFns);
  QueryContext zctx(z.m, *z.svc);
  auto zs = functionStats(zctx, {}, fk(kA));
  ASSERT_TRUE(zs.ok());
  EXPECT_EQ(zs->count, 0u);
}

TEST(FunctionStats, WeightOfTheLastSampleIsClippedAtTheEndOfRunning) {
  HandModel hm;
  hm.state(0, 0, 330, ThreadState::Running);
  hm.sample(0, 0, hm.stack({kA + 0x10}));
  hm.sample(0, 100, hm.stack({kA + 0x10}));
  hm.sample(0, 300, hm.stack({kA + 0x10}));  // weight 30
  hm.finish(kFns);
  QueryContext ctx(hm.m, *hm.svc);
  auto s = functionStats(ctx, {}, fk(kA));
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(s->count, 1u);
  EXPECT_EQ(s->max, 300 - 0 + 30);
}

TEST(FunctionStats, RunsAreFormedPerThread) {
  HandModel hm(2);
  hm.state(0, 0, 10000, ThreadState::Running);
  hm.state(1, 0, 10000, ThreadState::Running);
  const CallstackIdx a = hm.stack({kA + 0x10});
  // Interleaved in time, but each thread has one continuous run.
  for (uint64_t ts : {0, 100, 200}) {
    hm.sample(0, ts, a);
    hm.sample(1, ts + 50, a);
  }
  hm.finish(kFns);
  QueryContext ctx(hm.m, *hm.svc);
  auto s = functionStats(ctx, {}, fk(kA));
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->count, 2u);
  EXPECT_EQ(s->min, 300);
  EXPECT_EQ(s->max, 300);

  Filter t1;
  t1.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto only1 = functionStats(ctx, t1, fk(kA));
  ASSERT_TRUE(only1.ok());
  EXPECT_EQ(only1->count, 1u);
}

TEST(FunctionStats, TimeRangeSelectsSamplesAndTruncatesRuns) {
  Fix f;
  for (uint64_t ts = 0; ts < 1000; ts += 100) f.sample(ts, f.ab);  // one run 0..900: 1000
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);
  Filter range;
  range.t0 = 300;
  range.t1 = 700;  // samples 300..600 (half-open)
  auto s = functionStats(ctx, range, fk(kB));
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(s->count, 1u);
  EXPECT_EQ(s->max, 600 - 300 + 100);
  Filter empty;
  empty.t0 = 5000;
  empty.t1 = 5000;
  auto e = functionStats(ctx, empty, fk(kB));
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(e->count, 0u);
}

TEST(FunctionStats, SubtreeAndFunctionFiltersApply) {
  Fix f;
  const CallstackIdx cb = f.hm.stack({kC + 0x10, kB + 0x10});
  for (uint64_t ts : {0, 100}) f.sample(ts, f.ab);
  for (uint64_t ts : {200, 300}) f.sample(ts, cb);
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);
  // B is in all four samples, consecutive: one instance of 400.
  auto all = functionStats(ctx, {}, fk(kB));
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all->count, 1u);
  EXPECT_EQ(all->max, 400);
  // Only stacks containing C: samples 200, 300.
  Filter fc;
  fc.function = ctx.functionIdx(fk(kC));
  auto onlyC = functionStats(ctx, fc, fk(kB));
  ASSERT_TRUE(onlyC.ok());
  ASSERT_EQ(onlyC->count, 1u);
  EXPECT_EQ(onlyC->max, 200);
  // Subtree below A: depth of B is measured from the cut (A is the new root: B at depth 1).
  Filter sub;
  sub.subtreeRoot = f.hm.m.nodes[f.ab.get()].parent;  // the node of frame A
  auto underA = functionStats(ctx, sub, fk(kB));
  ASSERT_TRUE(underA.ok());
  ASSERT_EQ(underA->count, 1u);
  EXPECT_EQ(underA->max, 200);
}

TEST(FunctionStats, KernelFrameOptionsChangeTheDepth) {
  HandModel hm;
  hm.state(0, 0, 10000, ThreadState::Running);
  // Stacks with kernel frames below the user function: [K1, A, B] and [K1, K2, A, B].
  CallstackIdx k1 = hm.pushAbs(CallstackIdx(0), 0xFFFFF80000001000ull);
  CallstackIdx k2 = hm.pushAbs(k1, 0xFFFFF80000002000ull);
  const CallstackIdx s1 = hm.push(hm.push(k1, kA + 0x10), kB + 0x10);
  const CallstackIdx s2 = hm.push(hm.push(k2, kA + 0x10), kB + 0x10);
  hm.sample(0, 0, s1);
  hm.sample(0, 100, s2);
  hm.finish(kFns);
  {
    QueryContext show(hm.m, *hm.svc);  // Show: B at depth 2 then 3: two instances
    auto s = functionStats(show, {}, fk(kB));
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->count, 2u);
  }
  {
    AggregationOptions o;
    o.kernelFrames = KernelFrames::Group;  // [<kernel>, A, B] both: one instance
    QueryContext group(hm.m, *hm.svc, o);
    auto s = functionStats(group, {}, fk(kB));
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(s->count, 1u);
    EXPECT_EQ(s->max, 200);
  }
  {
    AggregationOptions o;
    o.kernelFrames = KernelFrames::Hide;  // [A, B] both
    QueryContext hide(hm.m, *hm.svc, o);
    auto s = functionStats(hide, {}, fk(kB));
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(s->count, 1u);
  }
}

// Provider whose function [0x1000, 0x1100) has an inlined callee "G" at 0x1010..0x1030.
class InlineProvider final : public symbols::ISymbolProvider {
 public:
  explicit InlineProvider(symbols::SymbolStringTable& strings) : m_strings(strings) {}
  Result<void> load(const model::Module&, const std::filesystem::path&) override { return {}; }
  symbols::ResolvedAddress resolve(uint64_t rva) override {
    symbols::ResolvedAddress r;
    if (rva >= 0x1000 && rva < 0x1100) {
      r.function = m_strings.intern("F");
      r.functionStartRva = 0x1000;
      r.resolved = true;
      if (rva >= 0x1010 && rva < 0x1030)
        r.inlines.push_back(symbols::InlineFrame{m_strings.intern("G"), {}, 1});
    } else if (rva >= 0x1200 && rva < 0x1300) {
      r.function = m_strings.intern("X");
      r.functionStartRva = 0x1200;
      r.resolved = true;
    }
    return r;
  }
  Result<std::vector<symbols::LineRange>> linesForFunction(uint64_t) override {
    return makeError(ErrorCode::NotFound, "no lines");
  }
  Result<symbols::FunctionBounds> functionBounds(uint64_t) override {
    return makeError(ErrorCode::NotFound, "none");
  }

 private:
  symbols::SymbolStringTable& m_strings;
};

TEST(FunctionStats, InlineExpansionShiftsDepthAndMakesInlineKeysQueryable) {
  HandModel hm;
  hm.state(0, 0, 10000, ThreadState::Running);
  hm.svc = std::make_unique<symbols::SymbolService>(std::vector<std::string>{}, std::filesystem::path{});
  hm.svc->setLocate(
      [](const model::Module&) -> Result<std::filesystem::path> { return std::filesystem::path("t"); });
  symbols::SymbolStringTable& strings = hm.svc->stringSink();
  hm.svc->setProviderFactory(
      [&strings](const model::Module&,
                 const std::filesystem::path&) -> std::unique_ptr<symbols::ISymbolProvider> {
        return std::make_unique<InlineProvider>(strings);
      });
  // Leaf frames F+0x10 (inline G) at depth 1 under X, and F+0x20 under X->X.
  const CallstackIdx xf = hm.stack({0x1210, 0x1010});
  const CallstackIdx xxf = hm.stack({0x1210, 0x1220, 0x1010});
  hm.sample(0, 0, xf);
  hm.sample(0, 100, xf);
  hm.sample(0, 200, xxf);  // F at depth 2: different instance for F and for G
  hm.svc->resolveAllAsync(hm.m);
  hm.svc->waitAll();

  AggregationOptions o;
  o.expandInline = true;
  QueryContext ctx(hm.m, *hm.svc, o);
  const FunctionKey g{ModuleIdx(0), symbols::kInlineKeyBit | hm.svc->stringSink().intern("G").get()};
  auto gs = functionStats(ctx, {}, g);
  ASSERT_TRUE(gs.ok()) << gs.error().toString();
  EXPECT_EQ(gs->count, 2u);  // depth 2 twice, then depth 3
  EXPECT_EQ(gs->max, 200);
  auto fs = functionStats(ctx, {}, fk(0x1000));
  ASSERT_TRUE(fs.ok());
  EXPECT_EQ(fs->count, 2u);

  QueryContext plain(hm.m, *hm.svc);  // without expansion the inline key is in no stack
  auto none = functionStats(plain, {}, g);
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->count, 0u);
}

TEST(FunctionStats, CachedResultsAreSharedAndKeyedByFilter) {
  Fix f;
  for (uint64_t ts : {0, 100, 200}) f.sample(ts, f.ab);
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);
  auto first = functionStatsShared(ctx, {}, fk(kB));
  ASSERT_TRUE(first.ok());
  const CacheStats before = ctx.cacheStats();
  auto second = functionStatsShared(ctx, {}, fk(kB));
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first->get(), second->get());
  const CacheStats after = ctx.cacheStats();
  EXPECT_EQ(after.resultHits, before.resultHits + 1);
  EXPECT_EQ(after.resultMisses, before.resultMisses);

  // Equivalent filters (thread list naming every thread) hit the same entry; different keys do not.
  Filter eq;
  eq.threads = std::vector<ThreadIdx>{ThreadIdx(0), ThreadIdx(0)};
  auto third = functionStatsShared(ctx, eq, fk(kB));
  ASSERT_TRUE(third.ok());
  EXPECT_EQ(first->get(), third->get());
  auto other = functionStatsShared(ctx, {}, fk(kA));
  ASSERT_TRUE(other.ok());
  EXPECT_NE(first->get(), other->get());
  EXPECT_GT(ctx.cacheStats().bytes, 0u);

  ctx.clearCache();
  auto again = functionStatsShared(ctx, {}, fk(kB));
  ASSERT_TRUE(again.ok());
  EXPECT_NE(first->get(), again->get());
  EXPECT_EQ((*first)->count, (*again)->count);
}

TEST(FunctionStats, CancelledQueriesReturnCancelledAndAreNotCached) {
  Fix f;
  for (uint64_t ts = 0; ts < 5000; ts += 100) f.sample(ts, f.ab);
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);
  std::atomic<bool> flag{true};
  auto r = functionStats(ctx, {}, fk(kB), Cancel{&flag});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
  EXPECT_EQ(ctx.cacheStats().entries, 0u);
  auto ok = functionStats(ctx, {}, fk(kB));
  EXPECT_TRUE(ok.ok());
}

TEST(FunctionStats, RejectsInvalidFilters) {
  Fix f;
  f.sample(0, f.ab);
  f.finish();
  QueryContext ctx(f.hm.m, *f.hm.svc);
  Filter badThread;
  badThread.threads = std::vector<ThreadIdx>{ThreadIdx(5)};
  auto r = functionStats(ctx, badThread, fk(kB));
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  Filter foreign;
  foreign.function = FunctionIdx(12345);
  auto g = functionStats(ctx, foreign, fk(kB));
  ASSERT_FALSE(g.ok());
  EXPECT_EQ(g.error().code, ErrorCode::InvalidArgument);
}

TEST(ThreadStateTotals, ClipsIntervalsToTheRange) {
  HandModel hm(2);
  hm.state(0, 0, 100, ThreadState::Running);
  hm.state(0, 100, 300, ThreadState::Waiting);
  hm.state(0, 300, 350, ThreadState::Ready);
  hm.state(0, 350, 400, ThreadState::Running);
  hm.state(0, 400, 500, ThreadState::Unknown);
  hm.state(1, 0, 500, ThreadState::Running);
  hm.finish();
  QueryContext ctx(hm.m, *hm.svc);

  auto all = threadStateTotals(ctx, {}, ThreadIdx(0));
  ASSERT_TRUE(all.ok()) << all.error().toString();
  EXPECT_EQ(all->runningTicks, 150u);
  EXPECT_EQ(all->waitingTicks, 200u);
  EXPECT_EQ(all->readyTicks, 50u);
  EXPECT_EQ(all->unknownTicks, 100u);
  EXPECT_EQ(all->runningIntervals, 2u);
  EXPECT_EQ(all->waitingIntervals, 1u);
  EXPECT_EQ(all->readyIntervals, 1u);

  Filter range;
  range.t0 = 50;
  range.t1 = 320;
  auto part = threadStateTotals(ctx, range, ThreadIdx(0));
  ASSERT_TRUE(part.ok());
  EXPECT_EQ(part->runningTicks, 50u);
  EXPECT_EQ(part->waitingTicks, 200u);
  EXPECT_EQ(part->readyTicks, 20u);
  EXPECT_EQ(part->unknownTicks, 0u);
  EXPECT_EQ(part->runningIntervals, 1u);

  // Totals add up over a split of the range.
  Filter lo, hi;
  lo.t1 = 200;
  hi.t0 = 200;
  auto l = threadStateTotals(ctx, lo, ThreadIdx(0));
  auto h = threadStateTotals(ctx, hi, ThreadIdx(0));
  ASSERT_TRUE(l.ok() && h.ok());
  EXPECT_EQ(l->runningTicks + h->runningTicks, all->runningTicks);
  EXPECT_EQ(l->waitingTicks + h->waitingTicks, all->waitingTicks);

  // The thread argument decides; Filter::threads is ignored.
  Filter other;
  other.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto ign = threadStateTotals(ctx, other, ThreadIdx(0));
  ASSERT_TRUE(ign.ok());
  EXPECT_EQ(ign->runningTicks, 150u);
  auto t1 = threadStateTotals(ctx, {}, ThreadIdx(1));
  ASSERT_TRUE(t1.ok());
  EXPECT_EQ(t1->runningTicks, 500u);

  Filter empty;
  empty.t0 = empty.t1 = 100;
  auto e = threadStateTotals(ctx, empty, ThreadIdx(0));
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(e->runningTicks + e->waitingTicks + e->readyTicks + e->unknownTicks, 0u);

  EXPECT_EQ(threadStateTotals(ctx, {}, ThreadIdx(9)).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(threadStateTotals(ctx, {}, ThreadIdx::invalid()).error().code, ErrorCode::InvalidArgument);
}

TEST(ThreadStateTotals, CacheHitReturnsTheSameObject) {
  HandModel hm;
  hm.state(0, 0, 100, ThreadState::Running);
  hm.finish();
  QueryContext ctx(hm.m, *hm.svc);
  auto a = threadStateTotalsShared(ctx, {}, ThreadIdx(0));
  auto b = threadStateTotalsShared(ctx, {}, ThreadIdx(0));
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->get(), b->get());
}

}  // namespace
}  // namespace tachyon::analysis
