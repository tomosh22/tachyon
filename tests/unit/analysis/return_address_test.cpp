// WP-32b: the return-address rule (every non-leaf frame is looked up at address - 1, the leaf and the
// interrupted user frame of a kernel-leaf sample at their exact address), D2 (stacks left empty by
// KernelFrames::Hide) and iterative NodeAgg destruction.
#include "aggregation_test_util.h"
#include "analysis/attribution.h"
#include "analysis/query.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <map>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::TestFunction;

constexpr uint64_t kMainStart = 0x1000, kAStart = 0x1100, kBStart = 0x1200;
const std::vector<TestFunction> kFunctions = {
    {"Main", 0x1000, 0x100}, {"A", 0x1100, 0x100}, {"B", 0x1200, 0x100}};
constexpr uint64_t kKernelAddr1 = 0xFFFF800000001000ull, kKernelAddr2 = 0xFFFF800000002000ull;

FunctionKey fk(uint64_t start) {
  return FunctionKey{ModuleIdx(0), start};
}

std::map<FunctionKey, FunctionRow> byKey(const std::vector<FunctionRow>& rows) {
  std::map<FunctionKey, FunctionRow> out;
  for (const FunctionRow& r : rows) out[r.key] = r;
  return out;
}

// Samples (100 ticks each) of one running thread.
struct Probe {
  HandModel h;
  uint64_t ts = 0;
  Probe() { h.state(0, 0, 100000, ThreadState::Running); }
  void samples(CallstackIdx s, int n, uint16_t flags = 0) {
    for (int i = 0; i < n; ++i, ts += 200) h.sample(0, ts, s, flags);
  }
};

// Main@0x1010 -> 0x1100 (return address right after Main's last instruction) -> B@0x1210.
TEST(ReturnAddressTest, LastInstructionCallIsAttributedToTheCaller) {
  Probe p;
  p.samples(p.h.stack({0x1010, 0x1100, 0x1210}), 10);
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);

  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok()) << rows.error().toString();
  const auto m = byKey(*rows);
  EXPECT_EQ(m.count(fk(kAStart)), 0u) << "spurious A row";
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at(fk(kMainStart)).inclusiveTicks, 1000u);  // Main twice in the stack, counted once
  EXPECT_EQ(m.at(fk(kMainStart)).exclusiveTicks, 0u);
  EXPECT_EQ(m.at(fk(kMainStart)).instances, 10u);
  EXPECT_EQ(m.at(fk(kBStart)).inclusiveTicks, 1000u);
  EXPECT_EQ(m.at(fk(kBStart)).exclusiveTicks, 1000u);

  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  ASSERT_EQ(tree->children.size(), 1u);
  const NodeAgg& main1 = tree->children[0];
  EXPECT_EQ(main1.key, fk(kMainStart));
  ASSERT_EQ(main1.children.size(), 1u);
  const NodeAgg& main2 = main1.children[0];
  EXPECT_EQ(main2.key, fk(kMainStart));  // not A
  ASSERT_EQ(main2.children.size(), 1u);
  EXPECT_EQ(main2.children[0].key, fk(kBStart));
  EXPECT_EQ(main2.children[0].exclusiveTicks, 1000u);

  auto bf = butterfly(ctx, {}, fk(kBStart));
  ASSERT_TRUE(bf.ok());
  ASSERT_EQ(bf->callers.size(), 1u);
  EXPECT_EQ(bf->callers[0].key, fk(kMainStart));
  EXPECT_EQ(bf->callers[0].inclusiveTicks, bf->self.inclusiveTicks);
  EXPECT_EQ(bf->self.inclusiveTicks, 1000u);

  auto bm = butterfly(ctx, {}, fk(kMainStart));
  ASSERT_TRUE(bm.ok());
  EXPECT_EQ(bm->self.inclusiveTicks, 1000u);
  for (const FunctionRow& r : bm->callers) EXPECT_NE(r.key, fk(kAStart));
  for (const FunctionRow& r : bm->callees) EXPECT_NE(r.key, fk(kAStart));
  auto ba = butterfly(ctx, {}, fk(kAStart));
  ASSERT_TRUE(ba.ok());
  EXPECT_EQ(ba->self.inclusiveTicks, 0u);
}

TEST(ReturnAddressTest, LeafUsesItsExactAddress) {
  Probe p;
  p.samples(p.h.stack({0x1010, 0x1100}), 4);  // leaf exactly at A's first instruction
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at(fk(kAStart)).exclusiveTicks, 400u);
  EXPECT_EQ(m.at(fk(kMainStart)).exclusiveTicks, 0u);
}

// The same frame is a leaf in one stack and a caller in another: each position resolves on its own.
TEST(ReturnAddressTest, SameFrameResolvesPerPosition) {
  Probe p;
  const CallstackIdx shared = p.h.stack({0x1010, 0x1100});
  p.samples(shared, 3);                    // 0x1100 is the leaf -> A
  p.samples(p.h.push(shared, 0x1210), 2);  // 0x1100 is a return address -> Main
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  EXPECT_EQ(m.at(fk(kAStart)).inclusiveTicks, 300u);
  EXPECT_EQ(m.at(fk(kAStart)).exclusiveTicks, 300u);
  EXPECT_EQ(m.at(fk(kMainStart)).inclusiveTicks, 500u);
  EXPECT_EQ(m.at(fk(kBStart)).exclusiveTicks, 200u);
}

// Root-first: Main@0x1010, 0x1100 (interrupted IP, first user frame below the kernel frames), kernel1,
// kernel2.
CallstackIdx kernelStack(HandModel& h) {
  const CallstackIdx user = h.stack({0x1010, 0x1100});
  return h.pushAbs(h.pushAbs(user, kKernelAddr1), kKernelAddr2);
}

TEST(ReturnAddressTest, KernelLeafSampleKeepsTheInterruptedFrameExact) {
  Probe p;
  p.samples(kernelStack(p.h), 5, trace::kSampleFlagKernelLeaf);
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  ASSERT_EQ(m.size(), 4u);  // Main, A (exact), kernel1, kernel2
  EXPECT_EQ(m.at(fk(kAStart)).inclusiveTicks, 500u);
  EXPECT_EQ(m.at(fk(kMainStart)).inclusiveTicks, 500u);
  EXPECT_EQ(m.at(FunctionKey{ModuleIdx::invalid(), kKernelAddr2}).exclusiveTicks, 500u);

  // Hide: the kernel time lands on the interrupted frame, which is A.
  AggregationOptions o;
  o.kernelFrames = KernelFrames::Hide;
  QueryContext hide(p.h.m, *p.h.svc, o);
  auto hrows = bottomUpFunctions(hide, {});
  ASSERT_TRUE(hrows.ok());
  const auto hm = byKey(*hrows);
  ASSERT_EQ(hm.size(), 2u);
  EXPECT_EQ(hm.at(fk(kAStart)).exclusiveTicks, 500u);

  // Group: Main -> A -> <kernel>.
  o.kernelFrames = KernelFrames::Group;
  QueryContext group(p.h.m, *p.h.svc, o);
  auto tree = topDownTree(group, {});
  ASSERT_TRUE(tree.ok());
  const NodeAgg* n = &*tree;
  const FunctionKey want[] = {fk(kMainStart), fk(kAStart), kKernelKey};
  for (const FunctionKey& k : want) {
    ASSERT_EQ(n->children.size(), 1u);
    n = &n->children[0];
    EXPECT_EQ(n->key, k);
  }
}

TEST(ReturnAddressTest, SameStackWithoutTheKernelFlagTreatsEveryCallerAsReturnAddress) {
  Probe p;
  p.samples(kernelStack(p.h), 5, 0);
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  EXPECT_EQ(m.count(fk(kAStart)), 0u);
  EXPECT_EQ(m.at(fk(kMainStart)).inclusiveTicks, 500u);
}

TEST(ReturnAddressTest, FlaggedAndUnflaggedSamplesOfOneStackAreKeptApart) {
  Probe p;
  const CallstackIdx s = kernelStack(p.h);
  p.samples(s, 3, trace::kSampleFlagKernelLeaf);
  p.samples(s, 2, 0);
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  EXPECT_EQ(m.at(fk(kAStart)).inclusiveTicks, 300u);
  EXPECT_EQ(m.at(fk(kMainStart)).inclusiveTicks, 500u);
  const std::vector<ThreadIdx> threads{ThreadIdx(0)};
  auto w = accumulateNodeWeights(p.h.m, 0, UINT64_MAX, threads, 1, {});
  ASSERT_TRUE(w.ok());
  ASSERT_EQ(w->size(), 2u);
  EXPECT_FALSE((*w)[0].kernelLeaf);
  EXPECT_EQ((*w)[0].samples, 2u);
  EXPECT_TRUE((*w)[1].kernelLeaf);
  EXPECT_EQ((*w)[1].samples, 3u);
}

TEST(ReturnAddressTest, UnmappedAndUnresolvedFramesKeepTheirOwnAddressAsKey) {
  Probe p;
  // module 0 resolves; an unmapped user frame and a kernel frame sit below it.
  const CallstackIdx s = p.h.pushAbs(p.h.pushAbs(p.h.stack({0x1010}), 0x700000001000ull), kKernelAddr1);
  p.samples(s, 3);
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  EXPECT_EQ(m.at(FunctionKey{ModuleIdx::invalid(), 0x700000001000ull}).inclusiveTicks, 300u);
  EXPECT_EQ(m.at(FunctionKey{ModuleIdx::invalid(), kKernelAddr1}).exclusiveTicks, 300u);
  EXPECT_EQ(functionKeyName(ctx, FunctionKey{ModuleIdx::invalid(), 0x700000001000ull}),
            "<unmapped>!0x700000001000");
  // The symbol service never resolves an unmapped frame, in either variant.
  for (uint32_t f = 0; f < p.h.m.frames.size(); ++f) {
    if (p.h.m.frames[f].module.valid()) continue;
    EXPECT_FALSE(p.h.svc->lookup(FrameIdx(f)).resolved);
    EXPECT_FALSE(p.h.svc->lookupCaller(FrameIdx(f)).resolved);
  }
}

TEST(ReturnAddressTest, UnresolvedMappedCallerFramesStillGroupPerRva) {
  Probe p;  // no symbols: nothing resolves
  p.samples(p.h.stack({0x2000, 0x2100, 0x2200}), 2);
  p.h.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  ASSERT_EQ(m.size(), 3u);
  EXPECT_EQ(m.at(fk(0x2000)).inclusiveTicks, 200u);
  EXPECT_EQ(m.at(fk(0x2100)).inclusiveTicks, 200u);
  EXPECT_EQ(functionKeyName(ctx, fk(0x2100)), "game.exe!0x2100");
}

TEST(ReturnAddressTest, FrameKeyAtAppliesTheRulePerPosition) {
  Probe p;
  const CallstackIdx plain = p.h.stack({0x1010, 0x1100, 0x1210});
  const CallstackIdx kern = kernelStack(p.h);
  p.samples(plain, 1);
  p.samples(kern, 1, trace::kSampleFlagKernelLeaf);
  p.h.finish(kFunctions);
  QueryContext ctx(p.h.m, *p.h.svc);
  // plain: position 0 = B@0x1210 (exact), 1 = 0x1100 (caller -> Main), 2 = Main@0x1010 (caller -> Main).
  EXPECT_EQ(frameKeyAt(ctx, plain, 0, false), fk(kBStart));
  EXPECT_EQ(frameKeyAt(ctx, plain, 1, false), fk(kMainStart));
  EXPECT_EQ(frameKeyAt(ctx, plain, 2, false), fk(kMainStart));
  EXPECT_EQ(frameKeyAt(ctx, plain, 3, false), kRootKey);
  // kern: positions 0, 1 are kernel; 2 = 0x1100.
  EXPECT_EQ(frameKeyAt(ctx, kern, 2, true), fk(kAStart));
  EXPECT_EQ(frameKeyAt(ctx, kern, 2, false), fk(kMainStart));
  EXPECT_EQ(frameKeyAt(ctx, kern, 3, true), fk(kMainStart));
  EXPECT_TRUE(usesExactAddress(p.h.m, kern, 0, true));
  EXPECT_FALSE(usesExactAddress(p.h.m, kern, 1, true));
  EXPECT_TRUE(usesExactAddress(p.h.m, kern, 2, true));
  EXPECT_FALSE(usesExactAddress(p.h.m, kern, 3, true));
  EXPECT_FALSE(usesExactAddress(p.h.m, kern, 2, false));
}

TEST(ReturnAddressTest, ExpandInlineFollowsThePhysicalFramesVariant) {
  // The provider table has no inline chains, so keys equal the plain ones; the point is that expansion
  // goes through the same variant selection and still gives no spurious A.
  Probe p;
  p.samples(p.h.stack({0x1010, 0x1100, 0x1210}), 2);
  p.h.finish(kFunctions);
  AggregationOptions o;
  o.expandInline = true;
  QueryContext ctx(p.h.m, *p.h.svc, o);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  EXPECT_EQ(byKey(*rows).count(fk(kAStart)), 0u);
}

TEST(HideKernelTest, StacksWithoutUserFramesAreCountedByBothViews) {
  Probe p;
  const CallstackIdx allKernel = p.h.pushAbs(p.h.pushAbs(CallstackIdx(0), kKernelAddr1), kKernelAddr2);
  p.samples(allKernel, 3);
  p.samples(CallstackIdx(0), 2);      // empty stack
  p.samples(p.h.stack({0x1010}), 4);  // user stack
  p.h.finish(kFunctions);
  AggregationOptions o;
  o.kernelFrames = KernelFrames::Hide;
  QueryContext ctx(p.h.m, *p.h.svc, o);

  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = byKey(*rows);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at(kKernelKey).inclusiveTicks, 500u);
  EXPECT_EQ(m.at(kKernelKey).exclusiveTicks, 500u);
  EXPECT_EQ(m.at(kKernelKey).instances, 5u);
  EXPECT_EQ(m.at(fk(kMainStart)).exclusiveTicks, 400u);

  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(tree->inclusiveTicks, 900u);
  EXPECT_EQ(tree->exclusiveTicks, 0u);
  ASSERT_EQ(tree->children.size(), 2u);
  const NodeAgg* k = tree->children[0].key == kKernelKey ? &tree->children[0] : &tree->children[1];
  ASSERT_EQ(k->key, kKernelKey);
  EXPECT_EQ(k->inclusiveTicks, 500u);
  EXPECT_EQ(k->exclusiveTicks, 500u);
  EXPECT_EQ(k->sampleCount, 5u);

  uint64_t excl = 0;
  for (const FunctionRow& r : *rows) excl += r.exclusiveTicks;
  EXPECT_EQ(excl, tree->inclusiveTicks);
}

TEST(NodeAggTest, DeepTreeDestructionAndMoveAreIterative) {
  constexpr size_t kDepth = 300000;
  NodeAgg root;
  NodeAgg* cur = &root;
  for (size_t i = 0; i < kDepth; ++i) {
    cur->children.emplace_back();
    cur = &cur->children.back();
  }
  NodeAgg moved = std::move(root);
  NodeAgg other;
  other = std::move(moved);
  EXPECT_EQ(other.children.size(), 1u);
}  // `other` is destroyed here: must not overflow the stack

TEST(NodeAggTest, TopDownTreeOf300000FramesIsBuiltAndFreed) {
  constexpr size_t kDepth = 300000;
  Probe p;
  CallstackIdx cur(0);
  for (size_t i = 0; i < kDepth; ++i) cur = p.h.push(cur, 0x10000 + i);
  p.samples(cur, 2);
  p.h.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok()) << tree.error().toString();
  size_t depth = 0;
  for (const NodeAgg* n = &*tree; !n->children.empty(); n = &n->children[0]) ++depth;
  EXPECT_EQ(depth, kDepth);
}

}  // namespace
}  // namespace tachyon::analysis
