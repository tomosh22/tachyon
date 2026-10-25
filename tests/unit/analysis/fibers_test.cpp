// WP-53: fiber-aware scope pairing, per-fiber lanes with pieces, group-by-fiber blending, scopeStats and lock
// summary under fibers; all on hand-built events (the `fibers` fixture is covered in
// scope_blend_fixture_test).
#include "analysis/fibers.h"
#include "aggregation_test_util.h"
#include "analysis/query.h"
#include "analysis/scope_blend.h"
#include "scope_test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::ScopeBuilder;
using testutil::TestFunction;

constexpr uint8_t kReg = trace::kFiberRegister, kUnreg = trace::kFiberUnregister,
                  kSwBegin = trace::kFiberSwitchBegin, kSwEnd = trace::kFiberSwitchEnd;
const std::vector<TestFunction> kFunctions = {{"Main", 0x1000, 0x100}, {"A", 0x1100, 0x100}};

// Two threads (tid 100, 101). Fiber 1 starts on thread 0, begins "Job" there at 100, leaves at 300 (thread 0
// then runs fiber 3 from 320 and has a thread-context gap in between), is resumed on thread 1 at 500 (fiber 2
// left it at 480) and ends "Job" there at 700. Fiber 2 runs on thread 1 from 0 and has "Other" in [50, 400).
struct Scenario {
  HandModel h{2};
  ScopeBuilder sc{h};
  Scenario() {
    h.state(0, 0, 100000, ThreadState::Running);
    h.state(1, 0, 100000, ThreadState::Running);
    for (uint64_t f : {1, 2, 3}) sc.fiber(0, 0, kReg, f);
    sc.fiber(0, 0, kSwEnd, 1);
    sc.fiber(0, 1, kSwEnd, 2);
    sc.begin(100, "Job", 0x1150, 100);
    sc.begin(50, "Other", 0x1150, 101);
    sc.end(400, 101);
    sc.fiber(300, 0, kSwBegin, 1, 3);
    sc.begin(305, "Sched", 0x1150, 100);
    sc.end(315, 100);
    sc.fiber(320, 0, kSwEnd, 3);
    sc.fiber(480, 1, kSwBegin, 2, 1);
    sc.fiber(500, 1, kSwEnd, 1, 2);
    sc.end(700, 101);
    sc.fiber(900, 1, kSwBegin, 1, 2);
    sc.sort();
  }
  void samplesAndFinish() {
    const CallstackIdx s = h.stack({0x1010, 0x1110});
    h.sample(0, 200, s);  // fiber 1: Job
    h.sample(0, 310, s);  // thread context: Sched
    h.sample(0, 400, s);  // fiber 3: nothing
    h.sample(1, 100, s);  // fiber 2: Other
    h.sample(1, 490, s);  // between fibers: nothing
    h.sample(1, 600, s);  // fiber 1: Job
    h.sample(1, 800, s);  // fiber 1: Job has ended
    h.finish(kFunctions);
  }
};

const FiberLane* laneOf(const PairedFibers& p, uint64_t fiber, ThreadIdx thread = {}) {
  for (const FiberLane& l : p.lanes)
    if (l.fiberId == fiber && (fiber != 0 || l.thread == thread)) return &l;
  return nullptr;
}

TEST(FiberPairing, ScopeMigratingBetweenThreadsIsPairedPerFiberAndSplitIntoPieces) {
  Scenario sc;
  sc.samplesAndFinish();
  auto r = pairScopesByFiber(sc.h.m);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_TRUE(r->fiberAware);
  const FiberLane* f1 = laneOf(*r, 1);
  ASSERT_NE(f1, nullptr);
  ASSERT_EQ(f1->scopes.size(), 1u);
  EXPECT_EQ(sc.h.m.strings.get(f1->scopes[0].id), "Job");
  EXPECT_EQ(f1->scopes[0].begin, 100u);
  EXPECT_EQ(f1->scopes[0].end, 700u);
  EXPECT_FALSE(f1->scopes[0].unterminated);
  EXPECT_EQ(f1->scopes[0].depth, 0u);
  ASSERT_EQ(f1->pieces.size(), 2u);
  EXPECT_EQ(f1->pieces[0].thread, ThreadIdx(0));
  EXPECT_EQ(f1->pieces[0].begin, 100u);
  EXPECT_EQ(f1->pieces[0].end, 300u);
  EXPECT_EQ(f1->pieces[1].thread, ThreadIdx(1));
  EXPECT_EQ(f1->pieces[1].begin, 500u);
  EXPECT_EQ(f1->pieces[1].end, 700u);
  // Runs of the fiber: [0,300) on thread 0, [500,900) on thread 1.
  ASSERT_EQ(f1->runs.size(), 2u);
  EXPECT_EQ(f1->runs[1].thread, ThreadIdx(1));
  EXPECT_EQ(f1->runs[1].end, 900u);

  const FiberLane* f2 = laneOf(*r, 2);
  ASSERT_NE(f2, nullptr);
  ASSERT_EQ(f2->pieces.size(), 1u);
  EXPECT_EQ(f2->pieces[0].begin, 50u);
  EXPECT_EQ(f2->pieces[0].end, 400u);  // fiber 2 left at 480, after the scope ended
  const FiberLane* f3 = laneOf(*r, 3);
  ASSERT_NE(f3, nullptr);
  EXPECT_TRUE(f3->scopes.empty());  // registered, no scopes

  // The scope that begins while no fiber runs belongs to thread 0's thread context.
  const FiberLane* tc = laneOf(*r, 0, ThreadIdx(0));
  ASSERT_NE(tc, nullptr);
  ASSERT_EQ(tc->pieces.size(), 1u);
  EXPECT_EQ(tc->pieces[0].begin, 305u);
  EXPECT_EQ(tc->pieces[0].end, 315u);

  // Grouped by the thread of the Begin: Job began on thread 0.
  ASSERT_EQ(r->scopes.byThread[0].size(), 2u);
  EXPECT_EQ(sc.h.m.strings.get(r->scopes.byThread[0][0].id), "Job");
  EXPECT_EQ(r->scopes.byThread[0][0].end, 700u);
  EXPECT_EQ(r->scopes.unterminatedCount, 0u);
  EXPECT_TRUE(r->scopes.orphanEndTs[0].empty() && r->scopes.orphanEndTs[1].empty());
}

TEST(FiberPairing, PlainPairingMispairsTheMigration) {
  Scenario sc;
  sc.samplesAndFinish();
  auto r = pairScopes(sc.h.m);
  ASSERT_TRUE(r.ok());
  // Per thread: the Begin on thread 0 never sees its End; the End on thread 1 is an orphan.
  EXPECT_EQ(r->unterminatedCount, 1u);
  EXPECT_EQ(r->orphanEndTs[1].size(), 1u);
}

TEST(FiberPairing, WithoutFiberEventsItIsThePlainPairing) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  sc.begin(10, "A", 0x1000);
  sc.begin(20, "B", 0x1000);
  sc.end(30);
  sc.end(40);
  sc.end(50);  // orphan
  sc.sort();
  h.finish();
  auto f = pairScopesByFiber(h.m);
  auto p = pairScopes(h.m);
  ASSERT_TRUE(f.ok() && p.ok());
  EXPECT_FALSE(f->fiberAware);
  EXPECT_TRUE(f->lanes.empty());
  ASSERT_EQ(f->scopes.byThread[0].size(), p->byThread[0].size());
  for (size_t i = 0; i < p->byThread[0].size(); ++i) {
    EXPECT_EQ(f->scopes.byThread[0][i].begin, p->byThread[0][i].begin);
    EXPECT_EQ(f->scopes.byThread[0][i].end, p->byThread[0][i].end);
    EXPECT_EQ(f->scopes.byThread[0][i].depth, p->byThread[0][i].depth);
  }
  EXPECT_EQ(f->scopes.orphanEndTs, p->orphanEndTs);
}

TEST(FiberPairing, ScopeSpanningSeveralSwitchesGetsOnePiecePerRun) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  sc.fiber(0, 0, kSwEnd, 7);
  sc.begin(10, "Long", 0x1000);
  sc.begin(20, "Inner", 0x1000);
  sc.fiber(100, 0, kSwBegin, 7, 8);
  sc.fiber(110, 0, kSwEnd, 8);
  sc.fiber(200, 0, kSwBegin, 8, 7);
  sc.fiber(205, 0, kSwEnd, 7);
  sc.end(300);  // Inner
  sc.fiber(400, 0, kSwBegin, 7, 8);
  sc.fiber(410, 0, kSwEnd, 7);
  sc.end(500);  // Long
  sc.sort();
  h.finish();
  auto r = pairScopesByFiber(h.m);
  ASSERT_TRUE(r.ok());
  const FiberLane* l = laneOf(*r, 7);
  ASSERT_NE(l, nullptr);
  ASSERT_EQ(l->scopes.size(), 2u);
  EXPECT_EQ(l->scopes[1].depth, 1u);
  // Long: [10,100) [205,400) [410,500); Inner: [20,100) [205,300).
  std::vector<std::tuple<uint64_t, uint64_t, uint32_t>> got;
  for (const ScopePiece& p : l->pieces) got.emplace_back(p.begin, p.end, p.scope);
  // Pieces are per scope and sorted by (begin, depth).
  std::vector<std::tuple<uint64_t, uint64_t, uint32_t>> wantPerScope = {
      {10, 100, 0}, {20, 100, 1}, {205, 400, 0}, {205, 300, 1}, {410, 500, 0}};
  EXPECT_EQ(got, wantPerScope);
}

TEST(FiberPairing, EndAtTheSwitchInstantBelongsToTheLeavingFiberAndBeginToTheEntering) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  sc.fiber(0, 0, kSwEnd, 1);
  sc.begin(10, "X", 0x1000);
  sc.end(100);  // same instant as the switch away
  sc.fiber(100, 0, kSwBegin, 1, 2);
  sc.fiber(100, 0, kSwEnd, 2);
  sc.begin(100, "Y", 0x1000);  // same instant as the switch in
  sc.end(200);
  sc.fiber(200, 0, kSwBegin, 2, 1);
  sc.sort();
  h.finish();
  auto r = pairScopesByFiber(h.m);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->scopes.unterminatedCount, 0u);
  EXPECT_TRUE(r->scopes.orphanEndTs[0].empty());
  ASSERT_NE(laneOf(*r, 1), nullptr);
  ASSERT_NE(laneOf(*r, 2), nullptr);
  EXPECT_EQ(h.m.strings.get(laneOf(*r, 1)->scopes[0].id), "X");
  EXPECT_EQ(h.m.strings.get(laneOf(*r, 2)->scopes[0].id), "Y");
  EXPECT_EQ(laneOf(*r, 2)->pieces[0].begin, 100u);
  EXPECT_EQ(laneOf(*r, 2)->pieces[0].end, 200u);
}

TEST(FiberPairing, UnregisterEndsOpenScopesAndOrphanEndsAreCounted) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  sc.fiber(0, 0, kReg, 5);
  sc.fiber(0, 0, kSwEnd, 5);
  sc.begin(10, "Left", 0x1000);
  sc.fiber(50, 0, kSwBegin, 5, 0);
  sc.fiber(60, 0, kUnreg, 5);
  sc.end(70);  // thread context, nothing open: orphan
  sc.sort();
  h.finish();
  auto r = pairScopesByFiber(h.m);
  ASSERT_TRUE(r.ok());
  const FiberLane* l = laneOf(*r, 5);
  ASSERT_NE(l, nullptr);
  ASSERT_EQ(l->scopes.size(), 1u);
  EXPECT_TRUE(l->scopes[0].unterminated);
  EXPECT_EQ(l->scopes[0].end, 60u);  // ends at the unregister
  ASSERT_EQ(l->pieces.size(), 1u);
  EXPECT_EQ(l->pieces[0].end, 50u);
  EXPECT_EQ(r->scopes.unterminatedCount, 1u);
  ASSERT_EQ(r->scopes.orphanEndTs[0].size(), 1u);
  EXPECT_EQ(r->scopes.orphanEndTs[0][0], 70u);
}

TEST(FiberPairing, FiberRunningAtTheEndOfTheTraceEndsThere) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  sc.fiber(0, 0, kSwEnd, 9);
  sc.begin(10, "Open", 0x1000);
  sc.sort();
  h.finish();
  auto r = pairScopesByFiber(h.m);
  ASSERT_TRUE(r.ok());
  const FiberLane* l = laneOf(*r, 9);
  ASSERT_NE(l, nullptr);
  EXPECT_TRUE(l->scopes[0].unterminated);
  EXPECT_EQ(l->scopes[0].end, h.m.header.end_ts);
  ASSERT_EQ(l->pieces.size(), 1u);
  EXPECT_EQ(l->pieces[0].end, h.m.header.end_ts);
}

TEST(FiberPairing, CancellationIsObservedWhileScanningEvents) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000000, ThreadState::Running);
  sc.fiber(0, 0, kSwEnd, 1);
  for (uint64_t i = 0; i < 40000; ++i) {  // 80 000 scope events: more than the 65 536 polling stride
    sc.begin(10 + 4 * i, "S", 0x1000);
    sc.end(12 + 4 * i);
  }
  sc.sort();
  h.m.header.end_ts = 100000000;
  h.finish();
  std::atomic<bool> flag{true};
  auto r = pairScopesByFiber(h.m, Cancel{&flag});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
  flag = false;
  auto ok = pairScopesByFiber(h.m, Cancel{&flag});
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(ok->lanes.size(), 1u);
  EXPECT_EQ(ok->lanes[0].pieces.size(), 40000u);
}

TEST(FiberBlend, CancelledBlendedQueriesReportCancelled) {
  Scenario sc;
  sc.samplesAndFinish();
  AggregationOptions o;
  o.blendScopes = true;
  o.groupByFiber = true;
  QueryContext ctx(sc.h.m, *sc.h.svc, o);
  std::atomic<bool> flag{true};
  auto t = topDownTree(ctx, {}, Cancel{&flag});
  ASSERT_FALSE(t.ok());
  EXPECT_EQ(t.error().code, ErrorCode::Cancelled);
  auto rows = bottomUpFunctions(ctx, {}, Cancel{&flag});
  ASSERT_FALSE(rows.ok());
  EXPECT_EQ(rows.error().code, ErrorCode::Cancelled);
  auto lk = lockSummary(ctx, {}, Cancel{&flag});
  EXPECT_TRUE(lk.ok() || lk.error().code == ErrorCode::Cancelled);
  flag = false;  // nothing was cached from the cancelled runs
  auto again = topDownTree(ctx, {});
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again->inclusiveTicks, 700u);
}

TEST(FiberBlend, GroupByFiberUsesTheFibersOwnScopeStack) {
  Scenario sc;
  sc.samplesAndFinish();
  AggregationOptions o;
  o.blendScopes = true;
  o.groupByFiber = true;
  QueryContext ctx(sc.h.m, *sc.h.svc, o);
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok()) << tree.error().toString();
  const auto paths = testutil::pathsOf(ctx, *tree);
  EXPECT_EQ(paths.at("Main/A/scope:Job"), 200u);    // t0@200, t1@600
  EXPECT_EQ(paths.at("Main/A/scope:Sched"), 100u);  // t0@310
  EXPECT_EQ(paths.at("Main/A/scope:Other"), 100u);  // t1@100
  EXPECT_EQ(paths.at("Main/A"), 300u);              // t0@400, t1@490, t1@800
  EXPECT_EQ(tree->inclusiveTicks, 700u);

  auto idx = blendIndexFor(ctx);
  ASSERT_TRUE(idx.ok());
  EXPECT_TRUE((*idx)->byFiber);
}

TEST(FiberBlend, PerThreadBlendingAttributesTheMigratedScopeToTheWrongThread) {
  Scenario sc;
  sc.samplesAndFinish();
  AggregationOptions o;
  o.blendScopes = true;  // groupByFiber off: everything per thread, as before
  QueryContext ctx(sc.h.m, *sc.h.svc, o);
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  const auto paths = testutil::pathsOf(ctx, *tree);
  // Thread 0's Job never ends in the per-thread view, so the samples at 200 and 400 see it (and 310 sees
  // Job/Sched); the sample at 600 on thread 1 does not.
  EXPECT_EQ(paths.at("Main/A/scope:Job"), 200u);
  EXPECT_EQ(paths.at("Main/A/scope:Job/scope:Sched"), 100u);
}

TEST(FiberBlend, TraceWithoutFiberEventsIgnoresGroupByFiber) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  sc.begin(0, "S", 0x1150);
  sc.end(100000);
  const CallstackIdx s = h.stack({0x1010, 0x1110});
  for (int i = 0; i < 3; ++i) h.sample(0, i * 200, s);
  sc.sort();
  h.finish(kFunctions);
  AggregationOptions a, b;
  a.blendScopes = b.blendScopes = true;
  b.groupByFiber = true;
  QueryContext ca(h.m, *h.svc, a), cb(h.m, *h.svc, b);
  auto ta = topDownTree(ca, {});
  auto tb = topDownTree(cb, {});
  ASSERT_TRUE(ta.ok() && tb.ok());
  EXPECT_EQ(testutil::pathsOf(ca, *ta), testutil::pathsOf(cb, *tb));
}

TEST(FiberStats, ScopeStatsUsesTheFiberAwarePairingWhenGrouped) {
  Scenario sc;
  sc.samplesAndFinish();
  const StringIdx job = sc.h.m.strings.find("Job");
  QueryContext plain(sc.h.m, *sc.h.svc);
  auto p = scopeStats(plain, {}, job);
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p->count, 1u);
  EXPECT_EQ(p->unterminated, 1u);  // per thread: never closed
  EXPECT_EQ(p->orphanEnds, 1u);

  AggregationOptions o;
  o.groupByFiber = true;
  QueryContext grouped(sc.h.m, *sc.h.svc, o);
  auto g = scopeStats(grouped, {}, job);
  ASSERT_TRUE(g.ok());
  EXPECT_EQ(g->count, 1u);
  EXPECT_EQ(g->unterminated, 0u);
  EXPECT_EQ(g->orphanEnds, 0u);
  EXPECT_EQ(g->min, 600.0);  // wall time 100..700
  EXPECT_EQ(g->totalTicks, 600u);
}

TEST(FiberLocks, LockWaitMigratingThreadsIsOneWaitWhenGrouped) {
  HandModel h{2};
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  h.state(1, 0, 100000, ThreadState::Running);
  sc.fiber(0, 0, kSwEnd, 1);
  sc.begin(100, "Tachyon.LockAcquire", 0x1000, 100, "0x1234", 0x4C4F43FFu);
  sc.fiber(200, 0, kSwBegin, 1, 2);
  sc.fiber(400, 1, kSwEnd, 1, 2);
  sc.end(500, 101);
  sc.sort();
  h.finish();
  QueryContext plain(h.m, *h.svc);
  auto lp = lockSummary(plain, {});
  ASSERT_TRUE(lp.ok());
  ASSERT_EQ(lp->locks.size(), 1u);
  EXPECT_EQ(lp->locks[0].unterminated, 1u);  // per thread: unterminated, ends at the trace end
  AggregationOptions o;
  o.groupByFiber = true;
  QueryContext grouped(h.m, *h.svc, o);
  auto lg = lockSummary(grouped, {});
  ASSERT_TRUE(lg.ok());
  ASSERT_EQ(lg->locks.size(), 1u);
  EXPECT_EQ(lg->locks[0].address, 0x1234u);
  EXPECT_EQ(lg->locks[0].contentionCount, 1u);
  EXPECT_EQ(lg->locks[0].totalWaitTicks, 400u);
  EXPECT_EQ(lg->locks[0].unterminated, 0u);
  ASSERT_EQ(lg->locks[0].topThreads.size(), 1u);
  EXPECT_EQ(lg->locks[0].topThreads[0].thread, ThreadIdx(0));  // the thread of the Begin
}

}  // namespace
}  // namespace tachyon::analysis
