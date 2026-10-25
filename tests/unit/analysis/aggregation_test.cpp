// topDownTree / bottomUpFunctions / butterfly on hand-built models: recursion, unresolved naming, kernel
// frames, filters, cancellation and the per-filter memo.
#include "analysis/query.h"

#include "aggregation_test_util.h"
#include "analysis/attribution.h"

#include <gtest/gtest.h>

#include <atomic>
#include <map>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;

constexpr uint64_t kMain = 0x1000, kF = 0x2000, kG = 0x3000, kH = 0x4000;
constexpr uint64_t kKernelAddr1 = 0xFFFF800000001000ull, kKernelAddr2 = 0xFFFF800000002000ull;

FunctionKey fk(uint64_t rva) {
  return FunctionKey{ModuleIdx(0), rva};
}

// Thread 0 runs throughout. Stacks (root first), 100 ticks per sample:
//   S1 main>f>f>g x10, S2 main>h x5, S3 main x2, S4 main>kernel1>kernel2 x4
struct Fixture {
  HandModel h{2};
  CallstackIdx fFirst;  // first f node of S1
  Fixture() {
    h.state(0, 0, 100000, ThreadState::Running);
    h.state(1, 0, 100000, ThreadState::Running);
    const CallstackIdx main = h.stack({kMain});
    fFirst = h.push(main, kF);
    const CallstackIdx s1 = h.push(h.push(fFirst, kF), kG);
    const CallstackIdx s2 = h.push(main, kH);
    const CallstackIdx s4 = h.pushAbs(h.pushAbs(main, kKernelAddr1), kKernelAddr2);
    uint64_t ts = 0;
    auto add = [&](CallstackIdx s, int n, uint32_t thread) {
      for (int i = 0; i < n; ++i, ts += 200) h.sample(thread, ts, s);
    };
    add(s1, 10, 0);
    add(s2, 5, 0);
    add(main, 2, 0);
    add(s4, 4, 1);
    h.finish();
  }
};

std::map<FunctionKey, FunctionRow> byKey(const std::vector<FunctionRow>& rows) {
  std::map<FunctionKey, FunctionRow> out;
  for (const FunctionRow& r : rows) out[r.key] = r;
  return out;
}

TEST(AggregationTest, BottomUpCountsRecursionOnce) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  Filter filter;
  filter.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  auto rows = bottomUpFunctions(ctx, filter);
  ASSERT_TRUE(rows.ok()) << rows.error().toString();
  const auto m = byKey(*rows);
  ASSERT_EQ(m.size(), 4u);
  EXPECT_EQ(m.at(fk(kMain)).inclusiveTicks, 1700u);
  EXPECT_EQ(m.at(fk(kMain)).exclusiveTicks, 200u);
  EXPECT_EQ(m.at(fk(kMain)).instances, 17u);
  EXPECT_EQ(m.at(fk(kF)).inclusiveTicks, 1000u);  // appears twice per stack, counted once
  EXPECT_EQ(m.at(fk(kF)).exclusiveTicks, 0u);
  EXPECT_EQ(m.at(fk(kF)).instances, 10u);
  EXPECT_EQ(m.at(fk(kG)).inclusiveTicks, 1000u);
  EXPECT_EQ(m.at(fk(kG)).exclusiveTicks, 1000u);
  EXPECT_EQ(m.at(fk(kH)).exclusiveTicks, 500u);
  // Sorted by exclusive descending.
  EXPECT_EQ((*rows)[0].key, fk(kG));
  EXPECT_EQ((*rows)[1].key, fk(kH));
  uint64_t excl = 0;
  for (const auto& r : *rows) excl += r.exclusiveTicks;
  EXPECT_EQ(excl, 1700u);
}

TEST(AggregationTest, TopDownMergesByKeyPerDepthAndKeepsRecursionNested) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  Filter filter;
  filter.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  auto tree = topDownTree(ctx, filter);
  ASSERT_TRUE(tree.ok()) << tree.error().toString();
  EXPECT_EQ(tree->key, kRootKey);
  EXPECT_EQ(tree->inclusiveTicks, 1700u);
  EXPECT_EQ(tree->sampleCount, 17u);
  EXPECT_EQ(tree->exclusiveTicks, 0u);
  ASSERT_EQ(tree->children.size(), 1u);
  const NodeAgg& main = tree->children[0];
  EXPECT_EQ(main.key, fk(kMain));
  EXPECT_EQ(main.inclusiveTicks, 1700u);
  EXPECT_EQ(main.exclusiveTicks, 200u);
  ASSERT_EQ(main.children.size(), 2u);
  EXPECT_EQ(main.children[0].key, fk(kF));  // 1000 > 500
  EXPECT_EQ(main.children[1].key, fk(kH));
  const NodeAgg& f1 = main.children[0];
  EXPECT_EQ(f1.inclusiveTicks, 1000u);
  ASSERT_EQ(f1.children.size(), 1u);
  const NodeAgg& f2 = f1.children[0];
  EXPECT_EQ(f2.key, fk(kF));
  EXPECT_EQ(f2.inclusiveTicks, 1000u);
  ASSERT_EQ(f2.children.size(), 1u);
  EXPECT_EQ(f2.children[0].key, fk(kG));
  EXPECT_EQ(f2.children[0].exclusiveTicks, 1000u);
  EXPECT_EQ(f2.children[0].sampleCount, 10u);
}

TEST(AggregationTest, ButterflyCallersSumToSelfInclusive) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  Filter filter;
  filter.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  auto b = butterfly(ctx, filter, fk(kF));
  ASSERT_TRUE(b.ok()) << b.error().toString();
  EXPECT_EQ(b->self.key, fk(kF));
  EXPECT_EQ(b->self.inclusiveTicks, 1000u);
  EXPECT_EQ(b->self.exclusiveTicks, 0u);
  EXPECT_EQ(b->self.instances, 10u);
  ASSERT_EQ(b->callers.size(), 1u);  // outermost occurrence: the caller is main, not f
  EXPECT_EQ(b->callers[0].key, fk(kMain));
  EXPECT_EQ(b->callers[0].inclusiveTicks, 1000u);
  ASSERT_EQ(b->callees.size(), 1u);  // relative to the outermost f the callee is the recursive f
  EXPECT_EQ(b->callees[0].key, fk(kF));

  auto g = butterfly(ctx, filter, fk(kG));
  ASSERT_TRUE(g.ok());
  EXPECT_EQ(g->self.inclusiveTicks, 1000u);
  EXPECT_EQ(g->self.exclusiveTicks, 1000u);
  ASSERT_EQ(g->callers.size(), 1u);
  EXPECT_EQ(g->callers[0].key, fk(kF));
  EXPECT_EQ(g->callers[0].exclusiveTicks, 1000u);
  EXPECT_TRUE(g->callees.empty());

  auto m = butterfly(ctx, filter, fk(kMain));
  ASSERT_TRUE(m.ok());
  ASSERT_EQ(m->callers.size(), 1u);  // main is the stack base
  EXPECT_EQ(m->callers[0].key, kRootKey);
  EXPECT_EQ(m->callers[0].inclusiveTicks, m->self.inclusiveTicks);
  uint64_t calleeSum = 0;
  for (const auto& c : m->callees) calleeSum += c.inclusiveTicks;
  EXPECT_EQ(calleeSum + m->self.exclusiveTicks, m->self.inclusiveTicks);

  auto none = butterfly(ctx, filter, fk(0x9999));
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->self.inclusiveTicks, 0u);
  EXPECT_TRUE(none->callers.empty());
}

TEST(AggregationTest, UnresolvedFramesGroupPerAddressAndAreNamedModuleBangRva) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  EXPECT_EQ(functionKeyName(ctx, fk(0x2000)), "game.exe!0x2000");
  EXPECT_EQ(functionKeyName(ctx, FunctionKey{ModuleIdx::invalid(), 0x1234}), "<unmapped>!0x1234");
  EXPECT_EQ(functionKeyName(ctx, kRootKey), "<root>");
  EXPECT_EQ(functionKeyName(ctx, kKernelKey), "<kernel>");
  EXPECT_EQ(functionKeyName(ctx, FunctionKey{ModuleIdx(77), 0x10}), "<module 77>!0x10");
}

TEST(AggregationTest, ScopeKeysAreNamedFromTheTraceStringTable) {
  Fixture f;
  const StringIdx id = f.h.m.strings.intern("Update");
  QueryContext ctx(f.h.m, *f.h.svc);
  EXPECT_EQ(functionKeyName(ctx, FunctionKey::scope(id)), "scope:Update");
}

TEST(AggregationTest, KernelFramesShowGroupHide) {
  Fixture f;
  Filter filter;
  filter.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  {
    QueryContext ctx(f.h.m, *f.h.svc);
    auto rows = bottomUpFunctions(ctx, filter);
    ASSERT_TRUE(rows.ok());
    const auto m = byKey(*rows);
    ASSERT_EQ(m.size(), 3u);
    const FunctionKey k2{ModuleIdx::invalid(), kKernelAddr2};
    EXPECT_EQ(m.at(k2).exclusiveTicks, 400u);
    EXPECT_EQ(m.at(FunctionKey{ModuleIdx::invalid(), kKernelAddr1}).inclusiveTicks, 400u);
  }
  {
    AggregationOptions o;
    o.kernelFrames = KernelFrames::Group;
    QueryContext ctx(f.h.m, *f.h.svc, o);
    auto rows = bottomUpFunctions(ctx, filter);
    ASSERT_TRUE(rows.ok());
    const auto m = byKey(*rows);
    ASSERT_EQ(m.size(), 2u);  // main + <kernel>
    EXPECT_EQ(m.at(kKernelKey).inclusiveTicks, 400u);
    EXPECT_EQ(m.at(kKernelKey).exclusiveTicks, 400u);
    EXPECT_EQ(m.at(kKernelKey).instances, 4u);
    auto tree = topDownTree(ctx, filter);
    ASSERT_TRUE(tree.ok());
    ASSERT_EQ(tree->children.size(), 1u);
    ASSERT_EQ(tree->children[0].children.size(), 1u);
    EXPECT_EQ(tree->children[0].children[0].key, kKernelKey);
    EXPECT_TRUE(tree->children[0].children[0].children.empty());
  }
  {
    AggregationOptions o;
    o.kernelFrames = KernelFrames::Hide;
    QueryContext ctx(f.h.m, *f.h.svc, o);
    auto rows = bottomUpFunctions(ctx, filter);
    ASSERT_TRUE(rows.ok());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].key, fk(kMain));
    EXPECT_EQ((*rows)[0].exclusiveTicks, 400u);  // kernel time lands on the user caller
  }
}

TEST(AggregationTest, KernelModuleFramesAreRecognised) {
  HandModel h;
  h.state(0, 0, 100000, ThreadState::Running);
  const CallstackIdx s = h.pushKernelModule(h.pushKernelModule(h.stack({kMain}), 0x10), 0x20);
  h.sample(0, 10, s);
  h.finish();
  AggregationOptions o;
  o.kernelFrames = KernelFrames::Group;
  QueryContext ctx(h.m, *h.svc, o);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  EXPECT_EQ(rows->size(), 2u);
  EXPECT_EQ(byKey(*rows).at(kKernelKey).exclusiveTicks, 100u);
}

TEST(AggregationTest, ThreadAndTimeFilters) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  auto all = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(byKey(*all).at(fk(kMain)).inclusiveTicks, 2100u);  // 17 + 4 samples

  Filter t1;
  t1.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto r1 = bottomUpFunctions(ctx, t1);
  ASSERT_TRUE(r1.ok());
  EXPECT_EQ(byKey(*r1).at(fk(kMain)).inclusiveTicks, 400u);

  // Samples of thread 0 are at ts = 0, 200, ... (10 of S1 first): [0, 1000) is the first five samples.
  Filter range;
  range.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  range.t0 = 0;
  range.t1 = 1000;
  auto r2 = bottomUpFunctions(ctx, range);
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(byKey(*r2).at(fk(kG)).exclusiveTicks, 500u);

  Filter empty;
  empty.t0 = 50;
  empty.t1 = 50;
  auto r3 = bottomUpFunctions(ctx, empty);
  ASSERT_TRUE(r3.ok());
  EXPECT_TRUE(r3->empty());
  auto t3 = topDownTree(ctx, empty);
  ASSERT_TRUE(t3.ok());
  EXPECT_EQ(t3->inclusiveTicks, 0u);
  EXPECT_TRUE(t3->children.empty());
}

TEST(AggregationTest, SubtreeRootCutsStacksAboveIt) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  Filter filter;
  filter.subtreeRoot = f.fFirst;
  auto rows = bottomUpFunctions(ctx, filter);
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  EXPECT_EQ(m.size(), 2u);  // f and g; main is above the subtree root
  EXPECT_EQ(m.at(fk(kF)).inclusiveTicks, 1000u);
  auto tree = topDownTree(ctx, filter);
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(tree->key, fk(kF));
  EXPECT_EQ(tree->inclusiveTicks, 1000u);
}

TEST(AggregationTest, InvalidFiltersAreRejected) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  Filter bad;
  bad.threads = std::vector<ThreadIdx>{ThreadIdx(9)};
  EXPECT_EQ(bottomUpFunctions(ctx, bad).error().code, ErrorCode::InvalidArgument);
  Filter fn;
  fn.function = FunctionIdx(1);  // never issued by ctx.functionIdx()
  EXPECT_EQ(topDownTree(ctx, fn).error().code, ErrorCode::InvalidArgument);
  Filter badRoot;
  badRoot.subtreeRoot = CallstackIdx(1u << 20);
  EXPECT_EQ(butterfly(ctx, badRoot, fk(kF)).error().code, ErrorCode::InvalidArgument);
}

TEST(AggregationTest, CancelledQueriesReturnCancelled) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  std::atomic<bool> flag{true};
  EXPECT_EQ(bottomUpFunctions(ctx, {}, Cancel{&flag}).error().code, ErrorCode::Cancelled);
  EXPECT_EQ(topDownTree(ctx, {}, Cancel{&flag}).error().code, ErrorCode::Cancelled);
  EXPECT_EQ(butterfly(ctx, {}, fk(kF), Cancel{&flag}).error().code, ErrorCode::Cancelled);
  // A cancelled query leaves nothing behind: the next one computes normally.
  EXPECT_TRUE(bottomUpFunctions(ctx, {}).ok());
}

TEST(AggregationTest, ResultsAreMemoizedPerFilter) {
  Fixture f;
  QueryContext ctx(f.h.m, *f.h.svc);
  Filter a;
  a.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  Filter b;
  b.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto r1 = bottomUpFunctions(ctx, a);
  ASSERT_TRUE(r1.ok());
  EXPECT_EQ(ctx.cacheStats().hits, 0u);
  auto r2 = bottomUpFunctions(ctx, a);
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(ctx.cacheStats().hits, 1u);
  EXPECT_EQ(r1->size(), r2->size());
  // Other queries on the same filter reuse the accumulated node weights.
  ASSERT_TRUE(topDownTree(ctx, a).ok());
  EXPECT_EQ(ctx.cacheStats().hits, 2u);
  // A different filter misses; the same thread list in another order is the same filter.
  ASSERT_TRUE(bottomUpFunctions(ctx, b).ok());
  EXPECT_EQ(ctx.cacheStats().hits, 2u);
  Filter ab;
  ab.threads = std::vector<ThreadIdx>{ThreadIdx(1), ThreadIdx(0)};
  Filter ba;
  ba.threads = std::vector<ThreadIdx>{ThreadIdx(0), ThreadIdx(1)};
  ASSERT_TRUE(bottomUpFunctions(ctx, ab).ok());
  const uint64_t before = ctx.cacheStats().hits;
  ASSERT_TRUE(bottomUpFunctions(ctx, ba).ok());
  EXPECT_EQ(ctx.cacheStats().hits, before + 1);
  ctx.clearCache();
  const uint64_t hits = ctx.cacheStats().hits;
  ASSERT_TRUE(bottomUpFunctions(ctx, a).ok());
  EXPECT_EQ(ctx.cacheStats().hits, hits);
}

}  // namespace
}  // namespace tachyon::analysis
