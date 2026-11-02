// WP-34: linesForFunction on hand-built models with a symbol provider that has line tables and inline chains.
#include "lines_test_util.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>

namespace tachyon::analysis {
namespace {

using namespace linetest;

TEST(LinesTest, LeafSamplesLandOnTheirLines) {
  Probe p;
  p.samples(p.h.stack({kMain + 0x10, kF + 0x05}), 3);   // line 10
  p.samples(p.h.stack({kMain + 0x10, kF + 0x15}), 2);   // line 20
  p.samples(p.h.stack({kMain + 0x10, kF + 0x200}), 4);  // outside F: other function (unresolved)
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto lines = linesForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(lines.ok()) << lines.error().toString();
  ASSERT_EQ(lines->size(), 2u);
  EXPECT_EQ((*lines)[0].line, 10u);
  EXPECT_EQ((*lines)[0].runningTicks, 300u);
  EXPECT_EQ((*lines)[0].ticks, 300u);
  EXPECT_EQ((*lines)[0].readyTicks, 0u);
  EXPECT_EQ((*lines)[0].waitingTicks, 0u);
  EXPECT_EQ((*lines)[1].line, 20u);
  EXPECT_EQ((*lines)[1].runningTicks, 200u);

  // The caller Main is on its own lines.
  auto main = linesForFunction(ctx, {}, fk(kMain));
  ASSERT_TRUE(main.ok());
  ASSERT_EQ(main->size(), 1u);
  EXPECT_EQ((*main)[0].line, 100u);
  EXPECT_EQ((*main)[0].runningTicks, 900u);  // all 9 samples pass through Main

  auto ins = instructionsForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(ins.ok());
  const auto m = byRva(*ins);
  EXPECT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at(kF + 0x05), 300u);
  EXPECT_EQ(m.at(kF + 0x15), 200u);
}

TEST(LinesTest, ReturnAddressRuleMovesTheLookupButNotTheInstruction) {
  Probe p;
  // Return address 0x1030 is the first byte of the line-30 range; the call itself is at 0x102f (line 20).
  p.samples(p.h.stack({kMain + 0x10, kF + 0x30, kG + 0x04}), 5);
  // Return address 0x1031 is inside line 30 (call at 0x1030).
  p.samples(p.h.stack({kMain + 0x10, kF + 0x31, kG + 0x04}), 2);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto lines = linesForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(lines.ok());
  const auto m = byLine(*lines);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at(20).runningTicks, 500u);
  EXPECT_EQ(m.at(30).runningTicks, 200u);
  // G is the leaf: exact address, its single line.
  auto g = linesForFunction(ctx, {}, fk(kG));
  ASSERT_TRUE(g.ok());
  ASSERT_EQ(g->size(), 1u);
  EXPECT_EQ((*g)[0].line, 5u);

  auto ins = instructionsForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(ins.ok());
  const auto r = byRva(*ins);
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r.at(kF + 0x30), 500u);  // the return address itself, i.e. the instruction after the call
  EXPECT_EQ(r.at(kF + 0x31), 200u);
}

TEST(LinesTest, KernelLeafSampleUsesTheExactAddressOfTheInterruptedFrame) {
  Probe p;
  const CallstackIdx s = p.h.pushAbs(p.h.stack({kMain + 0x10, kF + 0x30}), kKernelAddr);
  p.samples(s, 3, 0, trace::kSampleFlagKernelLeaf);  // F@0x1030 exact: line 30
  p.samples(s, 1, 0, 0);                             // F@0x1030 as a return address: line 20
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto lines = linesForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(lines.ok());
  const auto m = byLine(*lines);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at(30).runningTicks, 300u);
  EXPECT_EQ(m.at(20).runningTicks, 100u);
  auto ins = instructionsForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(ins.ok());
  ASSERT_EQ(ins->size(), 1u);
  EXPECT_EQ((*ins)[0].rva, kF + 0x30);
  EXPECT_EQ((*ins)[0].ticks, 400u);
}

TEST(LinesTest, RecursionCountsOnceAtTheOutermostOccurrence) {
  Probe p;
  // F@0x1015 -> F@0x1052 (return) -> F@0x1090 (leaf). Outermost: the first, looked up at 0x1014: line 20.
  p.samples(p.h.stack({kF + 0x15, kF + 0x52, kF + 0x90}), 4);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto lines = linesForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(lines.ok());
  ASSERT_EQ(lines->size(), 1u);
  EXPECT_EQ((*lines)[0].line, 20u);
  EXPECT_EQ((*lines)[0].runningTicks, 400u);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  uint64_t incl = 0;
  for (const FunctionRow& r : *rows)
    if (r.key == fk(kF)) incl = r.inclusiveTicks;
  EXPECT_EQ(incl, 400u);
  auto ins = instructionsForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(ins.ok());
  ASSERT_EQ(ins->size(), 1u);
  EXPECT_EQ((*ins)[0].rva, kF + 0x15);
}

TEST(LinesTest, WaitingAndReadyTimeThroughBlockingStacks) {
  Probe p;
  p.h.m.statesByThread[0].clear();
  p.h.state(0, 0, 1000, ThreadState::Running);
  p.h.state(0, 1000, 1700, ThreadState::Waiting);
  p.h.state(0, 1700, 2000, ThreadState::Ready);
  p.h.state(0, 2000, 3000, ThreadState::Running);
  // Blocked in G, called from F at a return address on line 30, called from Main; kernel leaf frame.
  const CallstackIdx blocking = p.h.pushAbs(p.h.stack({kMain + 0x10, kF + 0x31, kG + 0x04}), kKernelAddr);
  p.h.m.statesByThread[0][1].blocking_stack = blocking;
  p.h.m.statesByThread[0][2].blocking_stack = blocking;
  // Another wait without a blocking stack (ignored), at 3000..3500 would need more states: keep simple.
  p.samples(p.h.stack({kMain + 0x10, kF + 0x05}), 2);  // two running samples on line 10
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);

  auto lines = linesForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(lines.ok()) << lines.error().toString();
  auto m = byLine(*lines);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.at(10).runningTicks, 200u);
  EXPECT_EQ(m.at(10).waitingTicks, 0u);
  EXPECT_EQ(m.at(30).runningTicks, 0u);
  EXPECT_EQ(m.at(30).waitingTicks, 700u);
  EXPECT_EQ(m.at(30).readyTicks, 300u);
  EXPECT_EQ(m.at(30).ticks, 1000u);

  // Time range clips the intervals (and the samples): [1200, 1800) -> waiting 500, ready 100.
  Filter f;
  f.t0 = 1200;
  f.t1 = 1800;
  auto clipped = linesForFunction(ctx, f, fk(kF));
  ASSERT_TRUE(clipped.ok());
  ASSERT_EQ(clipped->size(), 1u);
  EXPECT_EQ((*clipped)[0].line, 30u);
  EXPECT_EQ((*clipped)[0].waitingTicks, 500u);
  EXPECT_EQ((*clipped)[0].readyTicks, 100u);
  EXPECT_EQ((*clipped)[0].runningTicks, 0u);

  // A function that is not in the blocking stack gets nothing; the leaf function G does (line 5).
  auto gl = linesForFunction(ctx, {}, fk(kG));
  ASSERT_TRUE(gl.ok());
  ASSERT_EQ(gl->size(), 1u);
  EXPECT_EQ((*gl)[0].line, 5u);
  EXPECT_EQ((*gl)[0].waitingTicks, 700u);

  // Unknown or Waiting intervals do not appear in instructionsForFunction (running samples only).
  auto ins = instructionsForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(ins.ok());
  ASSERT_EQ(ins->size(), 1u);
  EXPECT_EQ((*ins)[0].rva, kF + 0x05);
}

TEST(LinesTest, InlineCalleeCodeGoesToTheCallSiteLine) {
  Probe p;
  p.samples(p.h.stack({kMain + 0x10, kF + 0x60}), 3);  // inside the inlined Callee; the table says line 40
  p.samples(p.h.stack({kMain + 0x10, kH + 0x60}), 2);  // callee.h line 7 -> call site approximated by line 30
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto f = linesForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(f.ok());
  ASSERT_EQ(f->size(), 1u);
  EXPECT_EQ((*f)[0].line, 40u);
  EXPECT_EQ((*f)[0].runningTicks, 300u);
  auto h = linesForFunction(ctx, {}, fk(kH));
  ASSERT_TRUE(h.ok());
  ASSERT_EQ(h->size(), 1u);
  EXPECT_EQ((*h)[0].line, 30u);
  EXPECT_EQ((*h)[0].runningTicks, 200u);

  // The inline key reports the position inside the inlined function (the chain's line), with or without
  // expandInline on the context.
  const StringIdx callee = p.h.svc->symbolStrings().find("Callee");
  ASSERT_TRUE(callee.valid());
  const FunctionKey inlineKey{ModuleIdx(0), symbols::kInlineKeyBit | callee.get()};
  for (const bool expand : {false, true}) {
    AggregationOptions opts;
    opts.expandInline = expand;
    QueryContext c2(p.h.m, *p.h.svc, opts);
    auto il = linesForFunction(c2, {}, inlineKey);
    ASSERT_TRUE(il.ok());
    const auto m = byLine(*il);
    ASSERT_EQ(m.size(), 1u) << expand;
    EXPECT_EQ(m.at(7).runningTicks, 500u);  // both F and H inline "Callee"; both chains say line 7
    // The physical-function view is unchanged by the option.
    auto f2 = linesForFunction(c2, {}, fk(kF));
    ASSERT_TRUE(f2.ok());
    ASSERT_EQ(f2->size(), 1u);
    EXPECT_EQ((*f2)[0].line, 40u);
  }
}

TEST(LinesTest, FiltersThreadsRangeSubtreeAndFunction) {
  Probe p(2);
  const CallstackIdx viaMain = p.h.stack({kMain + 0x10, kF + 0x05});
  const CallstackIdx viaG = p.h.stack({kG + 0x10, kF + 0x15});
  p.samples(viaMain, 2, 0);  // ts 0, 200
  p.samples(viaG, 3, 1);     // ts 400, 600, 800
  p.samples(viaMain, 1, 1);  // ts 1000
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);

  Filter t0only;
  t0only.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  auto a = linesForFunction(ctx, t0only, fk(kF));
  ASSERT_TRUE(a.ok());
  auto am = byLine(*a);
  ASSERT_EQ(am.size(), 1u);
  EXPECT_EQ(am.at(10).runningTicks, 200u);

  Filter range;
  range.t0 = 400;
  range.t1 = 1000;  // half-open: ts 1000 excluded
  auto b = linesForFunction(ctx, range, fk(kF));
  ASSERT_TRUE(b.ok());
  auto bm = byLine(*b);
  ASSERT_EQ(bm.size(), 1u);
  EXPECT_EQ(bm.at(20).runningTicks, 300u);

  // Subtree: only stacks below the G node.
  Filter sub;
  sub.subtreeRoot = p.h.m.nodes[viaG.get()].parent;  // the G@0x10 node
  auto c = linesForFunction(ctx, sub, fk(kF));
  ASSERT_TRUE(c.ok());
  auto cm = byLine(*c);
  ASSERT_EQ(cm.size(), 1u);
  EXPECT_EQ(cm.at(20).runningTicks, 300u);
  // The cut hides frames above the subtree root: Main is not part of any stack below G.
  auto mainLines = linesForFunction(ctx, sub, fk(kMain));
  ASSERT_TRUE(mainLines.ok());
  EXPECT_TRUE(mainLines->empty());
  // ... but the subtree root's own function matches at the root.
  auto gLines = linesForFunction(ctx, sub, fk(kG));
  ASSERT_TRUE(gLines.ok());
  ASSERT_EQ(gLines->size(), 1u);
  EXPECT_EQ((*gLines)[0].runningTicks, 300u);

  // Function filter: only stacks containing Main.
  Filter fn;
  fn.function = ctx.functionIdx(fk(kMain));
  auto d = linesForFunction(ctx, fn, fk(kF));
  ASSERT_TRUE(d.ok());
  auto dm = byLine(*d);
  ASSERT_EQ(dm.size(), 1u);
  EXPECT_EQ(dm.at(10).runningTicks, 300u);

  // Foreign handle -> InvalidArgument.
  Filter bad;
  bad.function = FunctionIdx(12345);
  auto e = linesForFunction(ctx, bad, fk(kF));
  ASSERT_FALSE(e.ok());
  EXPECT_EQ(e.error().code, ErrorCode::InvalidArgument);
}

TEST(LinesTest, LinesSumToTheFunctionsInclusiveTime) {
  Probe p;
  p.samples(p.h.stack({kMain + 0x10, kF + 0x05, kG + 0x04}), 3);
  p.samples(p.h.stack({kMain + 0x10, kF + 0x55}), 4);
  p.samples(p.h.stack({kMain + 0x90, kG + 0x04}), 5);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  for (const FunctionRow& r : *rows) {
    auto lines = linesForFunction(ctx, {}, r.key);
    ASSERT_TRUE(lines.ok());
    uint64_t sum = 0;
    for (const LineAgg& l : *lines) sum += l.runningTicks;
    EXPECT_EQ(sum, r.inclusiveTicks) << functionKeyName(ctx, r.key);
  }
}

TEST(LinesTest, UnresolvedScopeAndPseudoKeysAreHandled) {
  Probe p;
  p.samples(p.h.stack({kMain + 0x10, kF + 0x05}), 3);
  p.samples(p.h.stack({kMain + 0x10, 0x9000}), 2);  // unresolved frame {module 0, rva 0x9000}
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  for (const FunctionKey k : {FunctionKey::scope(StringIdx(1)), kRootKey, kKernelKey, fk(0x7777)}) {
    auto l = linesForFunction(ctx, {}, k);
    ASSERT_TRUE(l.ok());
    EXPECT_TRUE(l->empty());
    auto i = instructionsForFunction(ctx, {}, k);
    ASSERT_TRUE(i.ok());
    EXPECT_TRUE(i->empty());
  }
  // An unresolved {module, rva} key matches its frames: line 0, instruction = the rva.
  auto l = linesForFunction(ctx, {}, fk(0x9000));
  ASSERT_TRUE(l.ok());
  ASSERT_EQ(l->size(), 1u);
  EXPECT_EQ((*l)[0].line, 0u);
  EXPECT_EQ((*l)[0].runningTicks, 200u);
  auto i = instructionsForFunction(ctx, {}, fk(0x9000));
  ASSERT_TRUE(i.ok());
  ASSERT_EQ(i->size(), 1u);
  EXPECT_EQ((*i)[0].rva, 0x9000u);
}

TEST(LinesTest, EmptyRangeCancelAndCaching) {
  Probe p;
  p.samples(p.h.stack({kMain + 0x10, kF + 0x05}), 3);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  Filter empty;
  empty.t0 = 50;
  empty.t1 = 50;
  auto e = linesForFunction(ctx, empty, fk(kF));
  ASSERT_TRUE(e.ok());
  EXPECT_TRUE(e->empty());

  std::atomic<bool> flag{true};
  auto cancelled = linesForFunction(ctx, {}, fk(kF), Cancel{&flag});
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
  EXPECT_FALSE(instructionsForFunction(ctx, {}, fk(kF), Cancel{&flag}).ok());

  auto a = linesForFunctionShared(ctx, {}, fk(kF));
  auto b = linesForFunctionShared(ctx, {}, fk(kF));
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a->get(), b->get());  // a hit returns the cache's copy
  auto i1 = instructionsForFunctionShared(ctx, {}, fk(kF));
  auto i2 = instructionsForFunctionShared(ctx, {}, fk(kF));
  ASSERT_TRUE(i1.ok());
  EXPECT_EQ(i1->get(), i2->get());
}

TEST(LinesTest, NoLineRowGivesLineZero) {
  Probe p;
  p.samples(p.h.stack({kMain + 0x10, kN + 0x20}), 3);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto lines = linesForFunction(ctx, {}, fk(kN));
  ASSERT_TRUE(lines.ok());
  ASSERT_EQ(lines->size(), 1u);
  EXPECT_EQ((*lines)[0].line, 0u);
  EXPECT_EQ((*lines)[0].runningTicks, 300u);
}

// DWARF/DIA convention: inlines[0] is at the line-table position, outer frames carry positions inside their
// own (inlined) function. The physical function's call-site line is not in ResolvedAddress, so the fallback
// (nearest preceding own-file row) applies, and the outer inline frame's line is never mistaken for it.
TEST(LinesTest, DwarfStyleInlineChainFallsBackToTheNearestOwnRow) {
  Probe p;
  p.samples(p.h.stack({kMain + 0x10, kD + 0x40}), 4);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto lines = linesForFunction(ctx, {}, fk(kD));
  ASSERT_TRUE(lines.ok());
  ASSERT_EQ(lines->size(), 1u);
  EXPECT_EQ((*lines)[0].line, 10u);
  EXPECT_EQ((*lines)[0].runningTicks, 400u);
  // The inline keys still report the chain's own lines.
  const StringIdx mid = p.h.svc->symbolStrings().find("Mid");
  ASSERT_TRUE(mid.valid());
  auto il = linesForFunction(ctx, {}, FunctionKey{ModuleIdx(0), symbols::kInlineKeyBit | mid.get()});
  ASSERT_TRUE(il.ok());
  ASSERT_EQ(il->size(), 1u);
  EXPECT_EQ((*il)[0].line, 33u);
}

}  // namespace
}  // namespace tachyon::analysis
