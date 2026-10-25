// WP-53: lockSummary, exact on a hand-built lock scenario (contended and uncontended acquires of three locks
// by several threads), plus address parsing, filters and caching.
#include "analysis/locks.h"
#include "aggregation_test_util.h"
#include "analysis/query.h"
#include "scope_test_util.h"

#include <gtest/gtest.h>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::ScopeBuilder;

constexpr uint32_t kLock = 0x4C4F43FFu;
constexpr const char* kReserved = "Tachyon.LockAcquire";

TEST(ParseLockAddress, AcceptsHexWithOrWithoutPrefix) {
  uint64_t a = 0;
  EXPECT_TRUE(parseLockAddress("0x1F2e", a));
  EXPECT_EQ(a, 0x1f2eu);
  EXPECT_TRUE(parseLockAddress("0XABCDEF0123456789", a));
  EXPECT_EQ(a, 0xABCDEF0123456789ull);
  EXPECT_TRUE(parseLockAddress("  7ffe0010  ", a));
  EXPECT_EQ(a, 0x7ffe0010u);
  EXPECT_TRUE(parseLockAddress("0x0", a));
  EXPECT_EQ(a, 0u);
  a = 99;
  EXPECT_FALSE(parseLockAddress("", a));
  EXPECT_FALSE(parseLockAddress("0x", a));
  EXPECT_FALSE(parseLockAddress("0xZZ", a));
  EXPECT_FALSE(parseLockAddress("0x12 34", a));
  EXPECT_FALSE(parseLockAddress("0x12345678901234567", a));  // 17 digits
  EXPECT_FALSE(parseLockAddress("name", a));
  EXPECT_EQ(a, 99u);
}

// Three threads (tid 100..102). Lock 0x1000 "Mutex.Scene" is waited for by all three, lock 0x2000 (reserved
// id, no name) by two, lock 0x3000 once. One non-lock scope and one lock scope without an address are mixed
// in.
struct LockScenario {
  HandModel h{3};
  ScopeBuilder sc{h};
  LockScenario() {
    for (uint32_t t = 0; t < 3; ++t) h.state(t, 0, 100000, ThreadState::Running);
    // 0x1000 / Mutex.Scene: t0 waits 100 + 300, t1 waits 50, t2 waits 700.
    sc.begin(1000, "Mutex.Scene", 0x10, 100, "0x1000", kLock);
    sc.end(1100, 100);
    sc.begin(2000, "Mutex.Scene", 0x10, 100, "0x1000", kLock);
    sc.end(2300, 100);
    sc.begin(1500, "Mutex.Scene", 0x10, 101, "0x1000", kLock);
    sc.end(1550, 101);
    sc.begin(3000, "Mutex.Scene", 0x10, 102, "0x1000", kLock);
    sc.end(3700, 102);
    // 0x2000 / reserved id: t0 waits 20, t1 waits 20 (a tie), nested inside a plain scope.
    sc.begin(4000, "Work", 0x10, 100);
    sc.begin(4010, kReserved, 0x10, 100, "0x2000", kLock);
    sc.end(4030, 100);
    sc.end(4100, 100);
    sc.begin(4010, kReserved, 0x10, 101, "0x2000", kLock);
    sc.end(4030, 101);
    // 0x3000: one wait on t2, 1000 ticks.
    sc.begin(5000, "Mutex.Audio", 0x10, 102, "0x3000", kLock);
    sc.end(6000, 102);
    // A lock scope with a broken address, and a normal scope whose data looks like an address.
    sc.begin(7000, "Mutex.Bad", 0x10, 100, "not-an-address", kLock);
    sc.end(7100, 100);
    sc.begin(7000, "Plain", 0x10, 101, "0x1000", 0xFFFFFFFFu);
    sc.end(9000, 101);
    sc.sort();
    h.finish();
  }
};

TEST(LockSummary, PerLockTotalsAreExact) {
  LockScenario s;
  QueryContext ctx(s.h.m, *s.h.svc);
  auto r = lockSummary(ctx, {});
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->unparsedWaits, 1u);
  ASSERT_EQ(r->locks.size(), 3u);
  // Sorted by total wait: 0x1000 (1150), 0x3000 (1000), 0x2000 (40).
  const LockRow& scene = r->locks[0];
  EXPECT_EQ(scene.address, 0x1000u);
  EXPECT_EQ(scene.contentionCount, 4u);
  EXPECT_EQ(scene.totalWaitTicks, 1150u);
  EXPECT_EQ(scene.maxWaitTicks, 700u);
  EXPECT_EQ(scene.unterminated, 0u);
  EXPECT_EQ(s.h.m.strings.get(scene.name), "Mutex.Scene");
  // Top waiting threads: t2 700 (1 wait), t0 400 (2 waits), t1 50.
  ASSERT_EQ(scene.topThreads.size(), 3u);
  EXPECT_EQ(scene.topThreads[0].thread, ThreadIdx(2));
  EXPECT_EQ(scene.topThreads[0].waitTicks, 700u);
  EXPECT_EQ(scene.topThreads[0].waits, 1u);
  EXPECT_EQ(scene.topThreads[1].thread, ThreadIdx(0));
  EXPECT_EQ(scene.topThreads[1].waitTicks, 400u);
  EXPECT_EQ(scene.topThreads[1].waits, 2u);
  EXPECT_EQ(scene.topThreads[2].thread, ThreadIdx(1));

  const LockRow& audio = r->locks[1];
  EXPECT_EQ(audio.address, 0x3000u);
  EXPECT_EQ(audio.contentionCount, 1u);
  EXPECT_EQ(audio.totalWaitTicks, 1000u);
  EXPECT_EQ(audio.maxWaitTicks, 1000u);

  const LockRow& anon = r->locks[2];
  EXPECT_EQ(anon.address, 0x2000u);
  EXPECT_FALSE(anon.name.valid());  // only the reserved id
  EXPECT_EQ(anon.contentionCount, 2u);
  EXPECT_EQ(anon.totalWaitTicks, 40u);
  ASSERT_EQ(anon.topThreads.size(), 2u);  // tie on ticks and waits: lower thread first
  EXPECT_EQ(anon.topThreads[0].thread, ThreadIdx(0));
  EXPECT_EQ(anon.topThreads[1].thread, ThreadIdx(1));
}

TEST(LockSummary, FiltersByTimeAndThreadOnTheBeginOfTheWait) {
  LockScenario s;
  QueryContext ctx(s.h.m, *s.h.svc);
  Filter f;
  f.t0 = 2000;
  f.t1 = 3001;  // Begin in [2000, 3001): t0's second wait and t2's first
  auto r = lockSummary(ctx, f);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->locks.size(), 1u);
  EXPECT_EQ(r->locks[0].contentionCount, 2u);
  EXPECT_EQ(r->locks[0].totalWaitTicks, 1000u);

  Filter t1;
  t1.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto r1 = lockSummary(ctx, t1);
  ASSERT_TRUE(r1.ok());
  ASSERT_EQ(r1->locks.size(), 2u);  // 0x1000 (50) and 0x2000 (20); the Plain scope is not a lock
  EXPECT_EQ(r1->locks[0].totalWaitTicks, 50u);
  EXPECT_EQ(r1->locks[1].totalWaitTicks, 20u);

  Filter empty;
  empty.t0 = 10;
  empty.t1 = 10;
  auto re = lockSummary(ctx, empty);
  ASSERT_TRUE(re.ok());
  EXPECT_TRUE(re->locks.empty());
}

TEST(LockSummary, UnterminatedWaitEndsAtTheTraceEndAndIsFlagged) {
  HandModel h;
  ScopeBuilder sc(h);
  h.state(0, 0, 100000, ThreadState::Running);
  sc.begin(90000, "Mutex.Stuck", 0x10, 100, "0xdead", kLock);
  sc.sort();
  h.finish();
  QueryContext ctx(h.m, *h.svc);
  auto r = lockSummary(ctx, {});
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->locks.size(), 1u);
  EXPECT_EQ(r->locks[0].totalWaitTicks, h.m.header.end_ts - 90000);
  EXPECT_EQ(r->locks[0].unterminated, 1u);
}

TEST(LockSummary, KeepsOnlyTheFiveTopWaiters) {
  HandModel h{8};
  ScopeBuilder sc(h);
  for (uint32_t t = 0; t < 8; ++t) {
    h.state(t, 0, 100000, ThreadState::Running);
    sc.begin(100, "L", 0x10, 100 + t, "0x40", kLock);
    sc.end(100 + 10 * (t + 1), 100 + t);
  }
  sc.sort();
  h.finish();
  QueryContext ctx(h.m, *h.svc);
  auto r = lockSummary(ctx, {});
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->locks.size(), 1u);
  EXPECT_EQ(r->locks[0].contentionCount, 8u);
  ASSERT_EQ(r->locks[0].topThreads.size(), kMaxLockWaiters);
  EXPECT_EQ(r->locks[0].topThreads[0].thread, ThreadIdx(7));
  EXPECT_EQ(r->locks[0].topThreads[0].waitTicks, 80u);
  EXPECT_EQ(r->locks[0].topThreads[4].thread, ThreadIdx(3));
}

TEST(LockSummary, SharedResultIsCachedAndTraceWithoutLocksIsEmpty) {
  LockScenario s;
  QueryContext ctx(s.h.m, *s.h.svc);
  auto a = lockSummaryShared(ctx, {});
  auto b = lockSummaryShared(ctx, {});
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->get(), b->get());

  HandModel h;
  h.state(0, 0, 100000, ThreadState::Running);
  h.finish();
  QueryContext empty(h.m, *h.svc);
  auto r = lockSummary(empty, {});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->locks.empty());
  EXPECT_EQ(r->unparsedWaits, 0u);
}

}  // namespace
}  // namespace tachyon::analysis
