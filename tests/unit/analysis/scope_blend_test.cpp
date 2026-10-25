// WP-53: scope blending (3.8 rule 5) on hand-built models: placement, monotonic order, unmapped frames,
// missing symbols, nesting, scopes open across a sample boundary, the return-address edge cases, the other
// queries, cache separation and "blending off changes nothing".
#include "analysis/scope_blend.h"
#include "aggregation_test_util.h"
#include "analysis/query.h"
#include "analysis/query_cache.h"
#include "scope_test_util.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::pathsOf;
using testutil::rowsByName;
using testutil::ScopeBuilder;
using testutil::TestFunction;

constexpr uint64_t kMain = 0x1000, kA = 0x1100, kB = 0x1200;
const std::vector<TestFunction> kFunctions = {{"Main", kMain, 0x100}, {"A", kA, 0x100}, {"B", kB, 0x100}};

AggregationOptions blendOn() {
  AggregationOptions o;
  o.blendScopes = true;
  return o;
}

// Samples (100 ticks each, every 200 ticks starting at `ts`) of thread 0.
struct Probe {
  HandModel h;
  ScopeBuilder sc{h};
  uint64_t ts = 0;
  Probe() { h.state(0, 0, 100000, ThreadState::Running); }
  void samples(CallstackIdx s, int n) {
    for (int i = 0; i < n; ++i, ts += 200) h.sample(0, ts, s);
  }
  void finish(bool symbols = true) {
    sc.sort();
    if (symbols)
      h.finish(kFunctions);
    else
      h.finish();
  }
};

TEST(ScopeBlend, PlacesScopeDirectlyAboveTheFrameContainingTheCaller) {
  Probe p;
  p.sc.begin(0, "S1", 0x1150);  // inside A
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 10);  // Main -> A -> B
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok()) << tree.error().toString();
  const auto paths = pathsOf(ctx, *tree);
  EXPECT_EQ(paths.size(), 1u);
  EXPECT_EQ(paths.at("Main/A/scope:S1/B"), 1000u);
  EXPECT_EQ(tree->inclusiveTicks, 1000u);

  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const auto m = rowsByName(ctx, *rows);
  EXPECT_EQ(m.at("scope:S1").inclusiveTicks, 1000u);
  EXPECT_EQ(m.at("scope:S1").exclusiveTicks, 0u);
  EXPECT_EQ(m.at("scope:S1").instances, 10u);
  EXPECT_EQ(m.at("B").exclusiveTicks, 1000u);
}

TEST(ScopeBlend, ScopeAboveTheLeafBecomesTheLeaf) {
  Probe p;
  p.sc.begin(0, "S1", 0x1250);  // inside B, the leaf
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 10);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  const auto paths = pathsOf(ctx, *tree);
  EXPECT_EQ(paths.at("Main/A/B/scope:S1"), 1000u);
  auto rows = bottomUpFunctions(ctx, {});
  const auto m = rowsByName(ctx, *rows);
  EXPECT_EQ(m.at("scope:S1").exclusiveTicks, 1000u);
  EXPECT_EQ(m.at("B").exclusiveTicks, 0u);
  EXPECT_EQ(m.at("B").inclusiveTicks, 1000u);
}

TEST(ScopeBlend, PlacementIsMonotonicForNestedScopes) {
  Probe p;
  // Outer matches B (deepest), the inner one matches only Main: it must still stay below the outer one.
  p.sc.begin(0, "Outer", 0x1250);
  p.sc.begin(10, "Inner", 0x1020);
  p.sc.end(99000);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 10);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  const auto paths = pathsOf(ctx, *tree);
  ASSERT_EQ(paths.size(), 2u);  // the sample at ts 0 only sees Outer
  EXPECT_EQ(paths.at("Main/A/B/scope:Outer"), 100u);
  EXPECT_EQ(paths.at("Main/A/B/scope:Outer/scope:Inner"), 900u);
}

TEST(ScopeBlend, OuterScopeDeeperThanInnerKeepsBothInOrderWithOtherFramesBetween) {
  Probe p;
  // Outer matches Main (slot 1), inner matches A (slot 2), third matches nothing (slot stays 2).
  p.sc.begin(0, "S0", 0x1020);
  p.sc.begin(0, "S1", 0x1120);
  p.sc.begin(0, "S2", 0x9000);
  p.sc.end(100000);
  p.sc.end(100000);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 5);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  const auto paths = pathsOf(ctx, *tree);
  ASSERT_EQ(paths.size(), 1u);
  EXPECT_EQ(paths.at("Main/scope:S0/A/scope:S1/scope:S2/B"), 500u);
}

TEST(ScopeBlend, ScopeWithoutAMatchGoesDirectlyAboveTheRootFrame) {
  Probe p;
  p.sc.begin(0, "Lost", 0x9000);  // no function there
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110}), 4);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  ASSERT_EQ(tree->children.size(), 1u);
  EXPECT_EQ(functionKeyName(ctx, tree->children[0].key), "game.exe!Main");
  const auto paths = pathsOf(ctx, *tree);
  EXPECT_EQ(paths.at("Main/scope:Lost/A"), 400u);
}

TEST(ScopeBlend, UnmappedFrameMatchesOnlyItsExactAddress) {
  Probe p;
  const CallstackIdx s = p.h.pushAbs(p.h.stack({0x1010}), 0x7ffe00001000ull);
  p.sc.begin(0, "Exact", 0x7ffe00001000ull);
  p.sc.begin(0, "Near", 0x7ffe00001004ull);  // same "function" but not the same address: no match
  p.sc.end(100000);
  p.sc.end(100000);
  p.samples(s, 3);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  const auto paths = pathsOf(ctx, *tree);
  ASSERT_EQ(paths.size(), 1u);
  // Exact matches the unmapped leaf (slot 2); Near matches nothing, so it follows monotonically (slot 2).
  EXPECT_EQ(paths.at("Main/<unmapped>!0x7ffe00001000/scope:Exact/scope:Near"), 300u);
}

TEST(ScopeBlend, UnmappedFrameWithAScopeThatMatchesNothingGoesAboveTheRootFrame) {
  Probe p;
  const CallstackIdx s = p.h.pushAbs(p.h.stack({0x1010}), 0x7ffe00001000ull);
  p.sc.begin(0, "Near", 0x7ffe00001004ull);
  p.sc.end(100000);
  p.samples(s, 3);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(pathsOf(ctx, *tree).at("Main/scope:Near/<unmapped>!0x7ffe00001000"), 300u);
}

TEST(ScopeBlend, WithoutSymbolsEveryScopeGoesAboveTheRootFrame) {
  Probe p;
  p.sc.begin(0, "S1", 0x1150);
  p.sc.begin(0, "S2", 0x1250);
  p.sc.end(100000);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 4);
  p.finish(/*symbols=*/false);
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok()) << tree.error().toString();
  const auto paths = pathsOf(ctx, *tree);
  ASSERT_EQ(paths.size(), 1u);
  EXPECT_EQ(paths.at("0x1010/scope:S1/scope:S2/0x1110/0x1210"), 400u);
}

TEST(ScopeBlend, NestedScopesWithTheSameIdStayNested) {
  Probe p;
  p.sc.begin(0, "X", 0x1150);
  p.sc.begin(0, "X", 0x1150);
  p.sc.end(100000);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 3);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(pathsOf(ctx, *tree).at("Main/A/scope:X/scope:X/B"), 300u);
  auto rows = bottomUpFunctions(ctx, {});
  const auto m = rowsByName(ctx, *rows);
  EXPECT_EQ(m.at("scope:X").inclusiveTicks, 300u);  // recursion counted once per sample
  EXPECT_EQ(m.at("scope:X").instances, 3u);
}

TEST(ScopeBlend, ScopeOpenAcrossASampleBoundaryAppliesToTheSamplesInside) {
  Probe p;
  // Samples every 200 ticks: 0, 200, 400, 600, 800. The scope is open for [250, 600): samples 400 only; a
  // sample exactly at the end (600) is outside, exactly at the begin it is inside.
  p.sc.begin(250, "S", 0x1150);
  p.sc.end(600);
  p.sc.begin(800, "T", 0x1150);
  p.sc.end(1000);
  p.samples(p.h.stack({0x1010, 0x1110}), 6);  // ts 0..1000
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  const auto paths = pathsOf(ctx, *tree);
  EXPECT_EQ(paths.at("Main/A/scope:S"), 100u);  // ts 400
  EXPECT_EQ(paths.at("Main/A/scope:T"), 100u);  // ts 800 (begin == ts is inside)
  EXPECT_EQ(paths.at("Main/A"), 400u);          // 0, 200, 600 (end == ts is outside), 1000 (end of T)
}

TEST(ScopeBlend, ReturnAddressRuleDecidesWhichFunctionANonLeafFrameBelongsTo) {
  Probe p;
  // Frame 1 is 0x1100 = the first byte of A, but as a return address it is the instruction after the last one
  // of Main: it belongs to Main, so a scope whose caller is in A has no matching frame.
  const CallstackIdx s = p.h.stack({0x1010, 0x1100, 0x1210});
  p.sc.begin(0, "InMain", 0x10F0);
  p.sc.end(50000);
  p.sc.begin(50000, "InA", 0x1100);
  p.sc.end(100000);
  p.ts = 0;
  for (int i = 0; i < 4; ++i, p.ts += 200) p.h.sample(0, p.ts, s);
  p.ts = 50000;
  for (int i = 0; i < 4; ++i, p.ts += 200) p.h.sample(0, p.ts, s);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  const auto paths = pathsOf(ctx, *tree);
  // Frames: Main, Main (return address), B. InMain matches both Main frames: deepest = the second (slot 2).
  EXPECT_EQ(paths.at("Main/Main/scope:InMain/B"), 400u);
  // InA's caller 0x1100 lies in A, but no frame of the stack is attributed to A: above the root frame.
  EXPECT_EQ(paths.at("Main/scope:InA/Main/B"), 400u);
}

TEST(ScopeBlend, CallerAtTheLastByteOfAFunctionMatchesItButOnePastDoesNot) {
  Probe p;
  p.sc.begin(0, "Last", 0x11FF);  // last byte of A
  p.sc.begin(0, "Past", 0x1200);  // first byte of B
  p.sc.end(100000);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110}), 2);  // Main -> A (leaf)
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  // Last matches A (slot 2); Past matches nothing -> monotonic slot 2.
  EXPECT_EQ(pathsOf(ctx, *tree).at("Main/A/scope:Last/scope:Past"), 200u);

  // Reversed order: the outer scope matches nothing (above the root frame), the inner one matches A.
  Probe q;
  q.sc.begin(0, "Past", 0x1200);
  q.sc.begin(0, "Last", 0x11FF);
  q.sc.end(100000);
  q.sc.end(100000);
  q.samples(q.h.stack({0x1010, 0x1110}), 2);
  q.finish();
  QueryContext ctx2(q.h.m, *q.h.svc, blendOn());
  auto tree2 = topDownTree(ctx2, {});
  ASSERT_TRUE(tree2.ok());
  EXPECT_EQ(pathsOf(ctx2, *tree2).at("Main/scope:Past/A/scope:Last"), 200u);
}

TEST(ScopeBlend, SamplesOutsideAnyScopeAreUnchanged) {
  Probe p;
  p.sc.begin(5000, "S", 0x1150);
  p.sc.end(6000);
  p.samples(p.h.stack({0x1010, 0x1110}), 5);  // ts 0..800, all before the scope
  p.finish();
  QueryContext on(p.h.m, *p.h.svc, blendOn());
  QueryContext off(p.h.m, *p.h.svc);
  auto a = topDownTree(on, {});
  auto b = topDownTree(off, {});
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(pathsOf(on, *a), pathsOf(off, *b));
}

TEST(ScopeBlend, BlendingOffIsIdenticalToATraceWithoutScopes) {
  Probe withScopes;
  withScopes.sc.begin(0, "S1", 0x1150);
  withScopes.sc.begin(10, "S2", 0x1250);
  withScopes.sc.end(90000);
  withScopes.sc.end(100000);
  Probe without;
  for (Probe* p : {&withScopes, &without}) {
    p->samples(p->h.stack({0x1010, 0x1110, 0x1210}), 10);
    p->samples(p->h.stack({0x1010, 0x1020}), 6);
    p->finish();
  }
  QueryContext a(withScopes.h.m, *withScopes.h.svc);
  QueryContext b(without.h.m, *without.h.svc);
  auto ta = topDownTree(a, {});
  auto tb = topDownTree(b, {});
  ASSERT_TRUE(ta.ok() && tb.ok());
  EXPECT_EQ(pathsOf(a, *ta), pathsOf(b, *tb));
  EXPECT_EQ(ta->inclusiveTicks, tb->inclusiveTicks);
  EXPECT_EQ(ta->sampleCount, tb->sampleCount);
  auto ra = bottomUpFunctions(a, {});
  auto rb = bottomUpFunctions(b, {});
  ASSERT_TRUE(ra.ok() && rb.ok());
  ASSERT_EQ(ra->size(), rb->size());
  for (size_t i = 0; i < ra->size(); ++i) {
    EXPECT_EQ((*ra)[i].key, (*rb)[i].key);
    EXPECT_EQ((*ra)[i].inclusiveTicks, (*rb)[i].inclusiveTicks);
    EXPECT_EQ((*ra)[i].exclusiveTicks, (*rb)[i].exclusiveTicks);
    EXPECT_EQ((*ra)[i].instances, (*rb)[i].instances);
  }
}

TEST(ScopeBlend, TotalsAreUnchangedByBlending) {
  Probe p;
  p.sc.begin(0, "S1", 0x1150);
  p.sc.begin(2000, "S2", 0x1250);
  p.sc.end(6000);
  p.sc.end(7000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 20);
  p.samples(p.h.stack({0x1010, 0x1020}), 20);
  p.finish();
  QueryContext on(p.h.m, *p.h.svc, blendOn());
  QueryContext off(p.h.m, *p.h.svc);
  auto a = topDownTree(on, {});
  auto b = topDownTree(off, {});
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->inclusiveTicks, b->inclusiveTicks);
  EXPECT_EQ(a->sampleCount, b->sampleCount);
  // Real functions keep their rows: blending only adds scope rows.
  auto ra = bottomUpFunctions(on, {});
  auto rb = bottomUpFunctions(off, {});
  ASSERT_TRUE(ra.ok() && rb.ok());
  const auto ma = rowsByName(on, *ra), mb = rowsByName(off, *rb);
  for (const auto& [name, row] : mb) {
    EXPECT_EQ(ma.at(name).inclusiveTicks, row.inclusiveTicks) << name;
  }
}

TEST(ScopeBlend, AllQueriesAgreeAndButterflyOfAScopeIsExact) {
  Probe p;
  p.sc.begin(0, "S", 0x1150);
  p.sc.end(100000);
  const CallstackIdx nMain = p.h.push(CallstackIdx(0), 0x1010);
  const CallstackIdx nA = p.h.push(nMain, 0x1110);
  const CallstackIdx nB = p.h.push(nA, 0x1210);
  p.samples(nB, 10);
  p.samples(nA, 5);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  const FunctionKey sk{symbols::kScopeModule, p.h.m.strings.find("S").get()};
  auto bf = butterfly(ctx, {}, sk);
  ASSERT_TRUE(bf.ok());
  EXPECT_EQ(bf->self.inclusiveTicks, 1500u);
  EXPECT_EQ(bf->self.exclusiveTicks, 500u);  // the second stack ends at A: the scope is the leaf
  ASSERT_EQ(bf->callers.size(), 1u);
  EXPECT_EQ(functionKeyName(ctx, bf->callers[0].key), "game.exe!A");
  EXPECT_EQ(bf->callers[0].inclusiveTicks, 1500u);
  ASSERT_EQ(bf->callees.size(), 1u);
  EXPECT_EQ(functionKeyName(ctx, bf->callees[0].key), "game.exe!B");
  EXPECT_EQ(bf->callees[0].inclusiveTicks, 1000u);

  // Filter::function = the scope keeps only samples whose (blended) stack has it.
  Filter f;
  f.function = ctx.functionIdx(sk);
  auto tree = topDownTree(ctx, f);
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(tree->inclusiveTicks, 1500u);
}

TEST(ScopeBlend, ScopesAboveACutFrameAreDroppedByTheSubtreeCut) {
  Probe p;
  p.sc.begin(0, "AboveCut", 0x1020);  // matches Main: slot 1, above the Main frame
  p.sc.begin(0, "BelowCut", 0x1150);  // matches A
  p.sc.end(100000);
  p.sc.end(100000);
  const CallstackIdx nMain = p.h.push(CallstackIdx(0), 0x1010);
  const CallstackIdx a = p.h.push(nMain, 0x1110);
  const CallstackIdx full = p.h.push(a, 0x1210);
  p.samples(full, 4);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  // Cut at the A node: Main and the scope above Main are removed.
  Filter f;
  f.subtreeRoot = a;
  auto tree = topDownTree(ctx, f);
  ASSERT_TRUE(tree.ok());
  const auto paths = pathsOf(ctx, *tree);
  ASSERT_EQ(paths.size(), 1u);
  EXPECT_EQ(paths.at("A/scope:BelowCut/B"), 400u);
}

TEST(ScopeBlend, FunctionStatsAndFramesSeeTheBlendedStacks) {
  Probe p;
  p.sc.begin(0, "S", 0x1150);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 10);
  p.finish();
  p.h.m.frameMarks.push_back(model::FrameMark{0, ThreadIdx(0), p.h.m.strings.intern("Frame"), 0});
  p.h.m.frameMarks.push_back(model::FrameMark{3000, ThreadIdx(0), p.h.m.strings.intern("Frame"), 1});
  QueryContext on(p.h.m, *p.h.svc, blendOn());
  QueryContext off(p.h.m, *p.h.svc);

  // The sampled function B: same run of samples blended or not (the scope does not break it).
  auto sb = functionStats(on, {}, FunctionKey{ModuleIdx(0), kB});
  auto so = functionStats(off, {}, FunctionKey{ModuleIdx(0), kB});
  ASSERT_TRUE(sb.ok() && so.ok());
  EXPECT_EQ(sb->count, so->count);
  EXPECT_EQ(sb->totalTicks, so->totalTicks);

  auto fb = frames(on, {});
  auto fo = frames(off, {});
  ASSERT_TRUE(fb.ok() && fo.ok());
  ASSERT_EQ(fb->size(), 1u);
  ASSERT_EQ(fo->size(), 1u);
  // B is the leaf unblended; with the scope above A the leaf is still B (the scope sits below it).
  ASSERT_EQ((*fb)[0].topFunctions.size(), 1u);
  EXPECT_EQ(functionKeyName(on, (*fb)[0].topFunctions[0].key), "game.exe!B");
  EXPECT_EQ((*fb)[0].topFunctions[0].exclusiveTicks, (*fo)[0].topFunctions[0].exclusiveTicks);

  // A scope above the leaf takes the exclusive time in the frame summary.
  Probe q;
  q.sc.begin(0, "Leafy", 0x1250);
  q.sc.end(100000);
  q.samples(q.h.stack({0x1010, 0x1110, 0x1210}), 10);
  q.finish();
  q.h.m.frameMarks.push_back(model::FrameMark{0, ThreadIdx(0), q.h.m.strings.intern("Frame"), 0});
  q.h.m.frameMarks.push_back(model::FrameMark{3000, ThreadIdx(0), q.h.m.strings.intern("Frame"), 1});
  QueryContext on2(q.h.m, *q.h.svc, blendOn());
  auto f2 = frames(on2, {});
  ASSERT_TRUE(f2.ok());
  ASSERT_EQ((*f2)[0].topFunctions.size(), 1u);
  EXPECT_EQ(functionKeyName(on2, (*f2)[0].topFunctions[0].key), "scope:Leafy");
}

TEST(ScopeBlend, ScopeOpeningInsideAFunctionDoesNotSplitItsInstance) {
  Probe p;
  // B runs for 10 samples; a scope that matches nothing (so it sits above the root frame and shifts B's depth
  // in the blended stack) is open for the middle ones only. Still a single instance of B.
  p.sc.begin(1000, "Mid", 0x9000);
  p.sc.end(1600);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 10);  // ts 0..1800 step 200
  p.finish();
  AggregationOptions on = blendOn();
  QueryContext ctx(p.h.m, *p.h.svc, on);
  QueryContext off(p.h.m, *p.h.svc);
  auto a = functionStats(ctx, {}, FunctionKey{ModuleIdx(0), kB});
  auto b = functionStats(off, {}, FunctionKey{ModuleIdx(0), kB});
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(b->count, 1u);
  EXPECT_EQ(a->count, 1u);
  EXPECT_EQ(a->totalTicks, b->totalTicks);
}

TEST(ScopeBlend, InlineFramesOfTheMatchedFrameSitBelowTheScope) {
  // With inline expansion the scope goes between the function key and its inline keys. The TableProvider has
  // no inlines, so only check that expansion on/off keeps the placement.
  Probe p;
  p.sc.begin(0, "S", 0x1150);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110, 0x1210}), 3);
  p.finish();
  AggregationOptions o = blendOn();
  o.expandInline = true;
  QueryContext ctx(p.h.m, *p.h.svc, o);
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(pathsOf(ctx, *tree).at("Main/A/scope:S/B"), 300u);
}

TEST(ScopeBlend, KernelFrameHandlingKeepsScopesAboveRootOrUserFrames) {
  Probe p;
  const uint64_t k1 = 0xFFFF800000001000ull;
  const CallstackIdx s = p.h.pushAbs(p.h.stack({0x1010, 0x1110}), k1);
  p.sc.begin(0, "S", 0x1150);
  p.sc.end(100000);
  p.samples(s, 4);
  p.finish();
  AggregationOptions o = blendOn();
  o.kernelFrames = KernelFrames::Hide;
  QueryContext ctx(p.h.m, *p.h.svc, o);
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(pathsOf(ctx, *tree).at("Main/A/scope:S"), 400u);
  o.kernelFrames = KernelFrames::Group;
  QueryContext ctx2(p.h.m, *p.h.svc, o);
  auto tree2 = topDownTree(ctx2, {});
  ASSERT_TRUE(tree2.ok());
  EXPECT_EQ(pathsOf(ctx2, *tree2).at("Main/A/scope:S/<kernel>"), 400u);
}

TEST(ScopeBlend, ThreadsAreBlendedWithTheirOwnScopes) {
  HandModel h(2);
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  h.state(1, 0, 100000, ThreadState::Running);
  sc.begin(0, "OnT0", 0x1150, /*tid=*/100);
  sc.end(100000, 100);
  sc.begin(0, "OnT1", 0x1150, /*tid=*/101);
  sc.end(100000, 101);
  const CallstackIdx s = h.stack({0x1010, 0x1110});
  for (int i = 0; i < 5; ++i) {
    h.sample(0, i * 200, s);
    h.sample(1, i * 200, s);
  }
  sc.sort();
  h.finish(kFunctions);
  QueryContext ctx(h.m, *h.svc, blendOn());
  Filter f0;
  f0.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  auto t0 = topDownTree(ctx, f0);
  ASSERT_TRUE(t0.ok());
  EXPECT_EQ(pathsOf(ctx, *t0).at("Main/A/scope:OnT0"), 500u);
  Filter f1;
  f1.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto t1 = topDownTree(ctx, f1);
  ASSERT_TRUE(t1.ok());
  EXPECT_EQ(pathsOf(ctx, *t1).at("Main/A/scope:OnT1"), 500u);
  auto all = topDownTree(ctx, {});
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all->inclusiveTicks, 1000u);
}

TEST(ScopeBlend, ResultsAreCachedPerOptionsAndDoNotMix) {
  Probe p;
  p.sc.begin(0, "S", 0x1150);
  p.sc.end(100000);
  p.samples(p.h.stack({0x1010, 0x1110}), 3);
  p.finish();
  QueryContext on(p.h.m, *p.h.svc, blendOn());
  auto a = topDownTreeShared(on, {});
  auto a2 = topDownTreeShared(on, {});
  ASSERT_TRUE(a.ok() && a2.ok());
  EXPECT_EQ(a->get(), a2->get());  // second call is a cache hit
  EXPECT_EQ(on.cacheStats().resultHits, 1u);
  QueryContext off(p.h.m, *p.h.svc);
  auto b = topDownTreeShared(off, {});
  ASSERT_TRUE(b.ok());
  EXPECT_NE(pathsOf(on, **a), pathsOf(off, **b));
  // The cache key distinguishes the options even within one cache.
  QueryKey k1, k2;
  k1.options = optionBits(blendOn());
  EXPECT_FALSE(k1 == k2);
  EXPECT_NE(hashKey(k1), hashKey(k2));
}

TEST(ScopeBlend, BlendIndexSegmentsFollowTheOpenScopes) {
  Probe p;
  p.sc.begin(100, "A", 0x1150);
  p.sc.begin(200, "B", 0x1150);
  p.sc.end(300);  // B
  p.sc.begin(300, "C", 0x1150);
  p.sc.end(400);  // C
  p.sc.end(500);  // A
  p.sc.begin(700, "Zero", 0x1150);
  p.sc.end(700);
  p.samples(p.h.stack({0x1010}), 1);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc, blendOn());
  auto idx = blendIndexFor(ctx);
  ASSERT_TRUE(idx.ok());
  const auto& seg = (*idx)->byThread[0];
  ASSERT_EQ(seg.size(), 4u);  // A [100,200), A/B [200,300), A/C [300,400), A [400,500)
  EXPECT_EQ(seg[0].begin, 100u);
  EXPECT_EQ(seg[0].end, 200u);
  EXPECT_EQ(seg[1].end, 300u);
  EXPECT_EQ(seg[3].begin, 400u);
  EXPECT_EQ(seg[3].end, 500u);
  EXPECT_EQ(seg[0].stack, seg[3].stack);
  EXPECT_EQ((*idx)->stackOf(seg[1].stack).size(), 2u);
  ScopeCursor c(seg);
  EXPECT_EQ(c.stackAt(99), 0u);
  EXPECT_EQ(c.stackAt(100), seg[0].stack);
  EXPECT_EQ(c.stackAt(250), seg[1].stack);
  EXPECT_EQ(c.stackAt(150), seg[0].stack);  // backwards
  EXPECT_EQ(c.stackAt(500), 0u);
  EXPECT_EQ(c.stackAt(9999), 0u);
  EXPECT_EQ(c.stackAt(350), seg[2].stack);
}

TEST(ScopeBlend, BlendingNeedsNoScopeEventsAndStaysCorrect) {
  Probe p;
  p.samples(p.h.stack({0x1010, 0x1110}), 3);
  p.finish();
  QueryContext on(p.h.m, *p.h.svc, blendOn());
  auto tree = topDownTree(on, {});
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(pathsOf(on, *tree).at("Main/A"), 300u);
}

}  // namespace
}  // namespace tachyon::analysis
