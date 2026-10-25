// QueryCache (keys, LRU, byte budget, counters), FunctionRegistry and the cache behaviour of the queries.
#include "analysis/query_cache.h"

#include "aggregation_test_util.h"
#include "analysis/function_registry.h"
#include "analysis/query.h"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;

constexpr uint64_t kMain = 0x1000, kF = 0x2000, kG = 0x3000, kH = 0x4000;

FunctionKey fk(uint64_t rva) {
  return FunctionKey{ModuleIdx(0), rva};
}

// Three threads; stacks main>f>g, main>h, main. 100 ticks per sample, samples every 200 ticks.
struct Fixture {
  HandModel h{3};
  Fixture() {
    for (uint32_t t = 0; t < 3; ++t) h.state(t, 0, 100000, ThreadState::Running);
    const CallstackIdx main = h.stack({kMain});
    const CallstackIdx fg = h.push(h.push(main, kF), kG);
    const CallstackIdx hh = h.push(main, kH);
    for (uint32_t t = 0; t < 3; ++t) {
      uint64_t ts = 0;
      for (int i = 0; i < 30; ++i, ts += 200) h.sample(t, ts, i % 3 == 0 ? fg : (i % 3 == 1 ? hh : main));
    }
    h.finish();
  }
};

QueryKey keyOf(QueryKind kind, uint64_t t1, uint64_t arg = 0) {
  QueryKey k;
  k.kind = kind;
  k.arg = arg;
  k.filter.t1 = t1;
  return k;
}

TEST(QueryCacheTest, HitMissAndExactKeyEquality) {
  QueryCache cache(1 << 20);
  auto v = std::make_shared<const int>(7);
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::BottomUp, 10)), nullptr);
  EXPECT_TRUE(cache.insert(keyOf(QueryKind::BottomUp, 10), v, 100));
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::BottomUp, 10)), v);
  // Any differing component is a different entry: kind, extra argument, filter.
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::TopDown, 10)), nullptr);
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::BottomUp, 10, 1)), nullptr);
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::BottomUp, 11)), nullptr);
  QueryKey withThreads = keyOf(QueryKind::BottomUp, 10);
  withThreads.filter.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  EXPECT_EQ(cache.find<int>(withThreads), nullptr);
  QueryKey withFn = keyOf(QueryKind::BottomUp, 10);
  withFn.filter.function = FunctionIdx(0);
  EXPECT_EQ(cache.find<int>(withFn), nullptr);
  QueryKey withFnKey = keyOf(QueryKind::BottomUp, 10);
  withFnKey.function = fk(5);
  EXPECT_EQ(cache.find<int>(withFnKey), nullptr);

  const CacheCounters c = cache.totalCounters();
  EXPECT_EQ(c.hits, 1u);  // exactly one hit (the second find)
  EXPECT_EQ(c.misses, 7u);
  EXPECT_EQ(cache.counters(QueryKind::BottomUp).hits, 1u);
  EXPECT_EQ(cache.counters(QueryKind::TopDown).misses, 1u);
}

TEST(QueryCacheTest, ReplacingAKeyKeepsOneEntry) {
  QueryCache cache(1 << 20);
  cache.insert(keyOf(QueryKind::BottomUp, 1), std::make_shared<const int>(1), 100);
  auto second = std::make_shared<const int>(2);
  cache.insert(keyOf(QueryKind::BottomUp, 1), second, 150);
  EXPECT_EQ(cache.entryCount(), 1u);
  EXPECT_EQ(cache.bytesUsed(), 150u);
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::BottomUp, 1)), second);
}

TEST(QueryCacheTest, EvictsLeastRecentlyUsedWithinTheByteBudget) {
  QueryCache cache(1000);
  for (uint64_t i = 0; i < 4; ++i)
    cache.insert(keyOf(QueryKind::BottomUp, i), std::make_shared<const int>(static_cast<int>(i)), 300);
  // 4 x 300 > 1000: the oldest (0) was evicted.
  EXPECT_EQ(cache.entryCount(), 3u);
  EXPECT_LE(cache.bytesUsed(), 1000u);
  EXPECT_EQ(cache.counters(QueryKind::BottomUp).evictions, 1u);
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::BottomUp, 0)), nullptr);
  // Touch 1, then add another: 2 is now the least recently used.
  ASSERT_NE(cache.find<int>(keyOf(QueryKind::BottomUp, 1)), nullptr);
  cache.insert(keyOf(QueryKind::BottomUp, 9), std::make_shared<const int>(9), 300);
  EXPECT_NE(cache.find<int>(keyOf(QueryKind::BottomUp, 1)), nullptr);
  EXPECT_EQ(cache.find<int>(keyOf(QueryKind::BottomUp, 2)), nullptr);
  EXPECT_NE(cache.find<int>(keyOf(QueryKind::BottomUp, 3)), nullptr);
}

TEST(QueryCacheTest, OversizedValuesAreRejectedAndShrinkingTheBudgetEvicts) {
  QueryCache cache(1000);
  EXPECT_FALSE(cache.insert(keyOf(QueryKind::TopDown, 1), std::make_shared<const int>(1), 1001));
  EXPECT_EQ(cache.counters(QueryKind::TopDown).rejected, 1u);
  EXPECT_EQ(cache.entryCount(), 0u);
  for (uint64_t i = 0; i < 3; ++i)
    cache.insert(keyOf(QueryKind::BottomUp, i), std::make_shared<const int>(0), 300);
  cache.setBudget(400);
  EXPECT_EQ(cache.entryCount(), 1u);
  EXPECT_EQ(cache.counters(QueryKind::BottomUp).evictions, 2u);
  cache.clear();
  EXPECT_EQ(cache.entryCount(), 0u);
  EXPECT_EQ(cache.bytesUsed(), 0u);
}

TEST(QueryCacheTest, HandedOutValuesSurviveEviction) {
  QueryCache cache(100);
  cache.insert(keyOf(QueryKind::BottomUp, 1), std::make_shared<const int>(5), 80);
  auto held = cache.find<int>(keyOf(QueryKind::BottomUp, 1));
  cache.insert(keyOf(QueryKind::BottomUp, 2), std::make_shared<const int>(6), 80);
  cache.clear();
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(*held, 5);
}

TEST(QueryCacheTest, ConcurrentUseIsConsistent) {
  QueryCache cache(4000);
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&, t] {
      for (uint64_t i = 0; i < 5000; ++i) {
        const QueryKey k = keyOf(QueryKind::BottomUp, (i * 7 + static_cast<uint64_t>(t)) % 50);
        if (!cache.find<int>(k)) cache.insert(k, std::make_shared<const int>(1), 200);
      }
    });
  }
  for (auto& th : ts) th.join();
  EXPECT_LE(cache.bytesUsed(), 4000u);
  EXPECT_LE(cache.entryCount(), 20u);
  const CacheCounters c = cache.totalCounters();
  EXPECT_EQ(c.hits + c.misses, 4u * 5000u);
}

TEST(FunctionRegistryTest, InternsAppendOnlyAndRoundTrips) {
  FunctionRegistry reg;
  EXPECT_EQ(reg.size(), 0u);
  EXPECT_FALSE(reg.find(fk(1)).has_value());
  const FunctionIdx a = reg.intern(fk(1));
  const FunctionIdx b = reg.intern(fk(2));
  EXPECT_EQ(a.get(), 0u);
  EXPECT_EQ(b.get(), 1u);
  EXPECT_EQ(reg.intern(fk(1)), a);
  EXPECT_EQ(reg.size(), 2u);
  EXPECT_EQ(reg.find(fk(2)), b);
  ASSERT_TRUE(reg.key(b).has_value());
  EXPECT_EQ(*reg.key(b), fk(2));
  EXPECT_FALSE(reg.key(FunctionIdx(2)).has_value());
  EXPECT_FALSE(reg.key(FunctionIdx::invalid()).has_value());
  // Scope keys and the synthetic keys are ordinary keys.
  EXPECT_NE(reg.intern(kKernelKey), reg.intern(kRootKey));
}

TEST(FunctionRegistryTest, ConcurrentInterningGivesOneHandlePerKey) {
  FunctionRegistry reg;
  std::vector<std::thread> ts;
  std::vector<std::vector<FunctionIdx>> got(4, std::vector<FunctionIdx>(500));
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&, t] {
      for (uint64_t i = 0; i < 500; ++i) got[t][i] = reg.intern(fk(i));
    });
  for (auto& th : ts) th.join();
  EXPECT_EQ(reg.size(), 500u);
  for (int t = 1; t < 4; ++t) EXPECT_EQ(got[t], got[0]);
  for (uint64_t i = 0; i < 500; ++i) EXPECT_EQ(*reg.key(got[0][i]), fk(i));
}

TEST(QueryContextCacheTest, HitAndMissAccountingPerQuery) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  ASSERT_TRUE(bottomUpFunctions(ctx, {}).ok());
  CacheStats s = ctx.cacheStats();
  EXPECT_EQ(s.resultHits, 0u);
  EXPECT_EQ(s.resultMisses, 1u);
  EXPECT_EQ(s.hits, 0u);
  EXPECT_EQ(s.misses, 2u);  // the result and the weight table
  EXPECT_EQ(s.entries, 2u);
  EXPECT_GT(s.bytes, 0u);
  ASSERT_TRUE(bottomUpFunctions(ctx, {}).ok());
  s = ctx.cacheStats();
  EXPECT_EQ(s.resultHits, 1u);
  EXPECT_EQ(s.resultMisses, 1u);
  // Another query on the same filter: result miss, weight hit.
  ASSERT_TRUE(topDownTree(ctx, {}).ok());
  ASSERT_TRUE(butterfly(ctx, {}, fk(kF)).ok());
  s = ctx.cacheStats();
  EXPECT_EQ(s.resultMisses, 3u);
  EXPECT_EQ(s.hits, 1u + 2u);
  // The butterfly key matters.
  ASSERT_TRUE(butterfly(ctx, {}, fk(kG)).ok());
  ASSERT_TRUE(butterfly(ctx, {}, fk(kF)).ok());
  s = ctx.cacheStats();
  EXPECT_EQ(s.resultMisses, 4u);
  EXPECT_EQ(s.resultHits, 2u);
  ctx.clearCache();
  s = ctx.cacheStats();
  EXPECT_EQ(s.entries, 0u);
  EXPECT_EQ(s.bytes, 0u);
  ASSERT_TRUE(bottomUpFunctions(ctx, {}).ok());
  EXPECT_EQ(ctx.cacheStats().resultMisses, 5u);
}

TEST(QueryContextCacheTest, EquivalentFiltersShareAnEntry) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  Filter plain;
  ASSERT_TRUE(bottomUpFunctions(ctx, plain).ok());
  // Naming every thread, in any order, is the same as naming none.
  Filter all;
  all.threads = std::vector<ThreadIdx>{ThreadIdx(2), ThreadIdx(0), ThreadIdx(1), ThreadIdx(1)};
  ASSERT_TRUE(bottomUpFunctions(ctx, all).ok());
  // The root node as subtree root is no subtree.
  Filter root;
  root.subtreeRoot = CallstackIdx(0);
  ASSERT_TRUE(bottomUpFunctions(ctx, root).ok());
  // All empty ranges are one entry.
  Filter e1, e2;
  e1.t0 = 50;
  e1.t1 = 50;
  e2.t0 = 900;
  e2.t1 = 10;
  ASSERT_TRUE(bottomUpFunctions(ctx, e1).ok());
  ASSERT_TRUE(bottomUpFunctions(ctx, e2).ok());
  const CacheStats s = ctx.cacheStats();
  EXPECT_EQ(s.resultMisses, 2u);  // plain, and the empty range
  EXPECT_EQ(s.resultHits, 3u);
}

TEST(QueryContextCacheTest, SharedResultsAreTheCachedObjectAndByValueIsACopy) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  auto a = bottomUpFunctionsShared(ctx, {});
  auto b = bottomUpFunctionsShared(ctx, {});
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->get(), b->get());
  auto byValue = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(byValue.ok());
  EXPECT_EQ(byValue->size(), (*a)->size());
  EXPECT_NE(byValue->data(), (*a)->data());
  auto t1 = topDownTreeShared(ctx, {});
  auto t2 = topDownTreeShared(ctx, {});
  ASSERT_TRUE(t1.ok() && t2.ok());
  EXPECT_EQ(t1->get(), t2->get());
  auto tv = topDownTree(ctx, {});
  ASSERT_TRUE(tv.ok());
  EXPECT_EQ(tv->inclusiveTicks, (*t1)->inclusiveTicks);
  EXPECT_EQ(tv->children.size(), (*t1)->children.size());
  auto b1 = butterflyShared(ctx, {}, fk(kF));
  auto b2 = butterflyShared(ctx, {}, fk(kF));
  ASSERT_TRUE(b1.ok() && b2.ok());
  EXPECT_EQ(b1->get(), b2->get());
}

TEST(QueryContextCacheTest, ByValueTopDownCopyHandlesVeryDeepTrees) {
  HandModel h(1);
  h.state(0, 0, 100000, ThreadState::Running);
  CallstackIdx cur(0);
  for (uint64_t i = 0; i < 100000; ++i) cur = h.push(cur, 0x1000 + (i % 2000) * 16);
  h.sample(0, 0, cur);
  h.finish();
  QueryContext ctx(h.m, *h.svc);
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok()) << tree.error().toString();
  auto again = topDownTree(ctx, {});  // from the cache: an iterative copy
  ASSERT_TRUE(again.ok());
  size_t depth = 0;
  for (const NodeAgg* n = &*again; !n->children.empty(); n = &n->children[0]) ++depth;
  EXPECT_EQ(depth, 100000u);
}

TEST(QueryContextCacheTest, ByteBudgetBoundsTheCacheAndEvictsOldResults) {
  Fixture f;
  QueryCacheConfig cfg;
  cfg.budgetBytes = 4096;
  QueryContext ctx(f.h.m, *f.h.svc, {}, cfg);
  for (uint64_t t = 1; t <= 40; ++t) {
    Filter filter;
    filter.t1 = t * 150;
    ASSERT_TRUE(bottomUpFunctions(ctx, filter).ok());
    ASSERT_TRUE(topDownTree(ctx, filter).ok());
    EXPECT_LE(ctx.cacheStats().bytes, 4096u);
  }
  EXPECT_GT(ctx.cacheStats().evictions, 0u);
  // The most recent result is still there.
  Filter last;
  last.t1 = 40 * 150;
  const uint64_t hits = ctx.cacheStats().resultHits;
  ASSERT_TRUE(topDownTree(ctx, last).ok());
  EXPECT_EQ(ctx.cacheStats().resultHits, hits + 1);
  ctx.setCacheBudget(0);
  EXPECT_EQ(ctx.cacheStats().entries, 0u);
  // Everything still works (and is simply not cached).
  ASSERT_TRUE(bottomUpFunctions(ctx, last).ok());
  EXPECT_EQ(ctx.cacheStats().entries, 0u);
}

TEST(QueryContextCacheTest, CancelledQueriesAreNotCached) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  std::atomic<bool> flag{true};
  EXPECT_EQ(bottomUpFunctions(ctx, {}, Cancel{&flag}).error().code, ErrorCode::Cancelled);
  EXPECT_EQ(ctx.cacheStats().entries, 0u);
  EXPECT_TRUE(bottomUpFunctions(ctx, {}).ok());
}

TEST(QueryContextCacheTest, ConcurrentQueriesOnOneContextAgree) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  std::vector<std::thread> ts;
  std::vector<uint64_t> totals(8, 0);
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < 50; ++i) {
        Filter filter;
        filter.t1 = 100 + static_cast<uint64_t>(i % 5) * 1000;
        auto rows = bottomUpFunctions(ctx, filter);
        ASSERT_TRUE(rows.ok());
        for (const auto& r : *rows) totals[t] += r.exclusiveTicks;
      }
    });
  for (auto& th : ts) th.join();
  for (int t = 1; t < 8; ++t) EXPECT_EQ(totals[t], totals[0]);
}

}  // namespace
}  // namespace tachyon::analysis
