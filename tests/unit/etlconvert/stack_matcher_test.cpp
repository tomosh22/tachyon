#include "etlconvert/stack_matcher.h"

#include "test_util.h"

namespace tachyon::etlconvert {
namespace {

using namespace testing;

constexpr uint64_t kK = 0xFFFFF80000000000ull;  // kernel-mode frame base

struct Done {
  ParentKind kind;
  uint64_t ts;
  bool hasStack;
  bool twoPart;
  uint32_t stackThread;
  MatchRole role;
  std::vector<uint64_t> frames;
};

struct Recorder : ParentListener {
  std::vector<Done> done;
  void onParentDone(PendingParent& p) override {
    done.push_back({p.kind, p.ts, p.hasStack(), p.twoPart, p.stackThread, p.role, p.frames});
  }
};

StackWalkPayload walk(ByteVec& storage, uint64_t parentTs, uint32_t thread,
                      const std::vector<uint64_t>& frames) {
  storage = stackWalkPayload(parentTs, 1, thread, frames);
  return *decodeStackWalk(storage);
}

void addSample(StackMatcher& m, uint64_t ts, uint32_t payloadTid, uint32_t headerTid) {
  const PendingParent::Candidate c[2] = {{payloadTid, MatchRole::PayloadThread},
                                         {headerTid, MatchRole::HeaderThread}};
  m.add(ParentKind::Sample, ts, c, 2);
}

TEST(StackMatcher, MatchesByTimestampAndThread) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 100, 5, 5);
  addSample(m, 100, 6, 6);  // same timestamp, other thread
  ByteVec s;
  auto r = m.onStack(walk(s, 100, 6, {0x10, 0x20}));
  EXPECT_EQ(r.kind, StackMatchResult::Kind::Matched);
  EXPECT_EQ(r.parent, ParentKind::Sample);
  r = m.onStack(walk(s, 100, 5, {0x30}));
  EXPECT_EQ(r.kind, StackMatchResult::Kind::Matched);
  m.advance(100);
  ASSERT_EQ(rec.done.size(), 2u);
  EXPECT_EQ(rec.done[0].frames, (std::vector<uint64_t>{0x30}));  // arrival order, not match order
  EXPECT_EQ(rec.done[1].frames, (std::vector<uint64_t>{0x10, 0x20}));
  EXPECT_EQ(m.pending(), 0u);
}

TEST(StackMatcher, UnmatchedStackIsReported) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 100, 5, 5);
  ByteVec s;
  EXPECT_EQ(m.onStack(walk(s, 101, 5, {0x10})).kind, StackMatchResult::Kind::Unmatched);  // wrong timestamp
  EXPECT_EQ(m.onStack(walk(s, 100, 9, {0x10})).kind, StackMatchResult::Kind::Unmatched);  // wrong thread
  // A second stack for an already completed parent finds nothing either.
  EXPECT_EQ(m.onStack(walk(s, 100, 5, {0x10})).kind, StackMatchResult::Kind::Matched);
  EXPECT_EQ(m.onStack(walk(s, 100, 5, {0x11})).kind, StackMatchResult::Kind::Unmatched);
}

TEST(StackMatcher, RolesFollowTheMatchingCandidate) {
  Recorder rec;
  StackMatcher m(1000, rec);
  // CSwitch: header 7, new 8, old 9.
  const PendingParent::Candidate c[3] = {
      {7, MatchRole::HeaderThread}, {8, MatchRole::PayloadThread}, {9, MatchRole::PayloadOldThread}};
  m.add(ParentKind::CSwitch, 50, c, 3);
  m.add(ParentKind::CSwitch, 60, c, 3);
  ByteVec s;
  auto r = m.onStack(walk(s, 50, 9, {0x1}));
  EXPECT_EQ(r.role, MatchRole::PayloadOldThread);
  EXPECT_EQ(r.parent, ParentKind::CSwitch);
  r = m.onStack(walk(s, 60, 7, {0x1}));
  EXPECT_EQ(r.role, MatchRole::HeaderThread);
  m.advance(60);
  ASSERT_EQ(rec.done.size(), 2u);
  EXPECT_EQ(rec.done[0].role, MatchRole::PayloadOldThread);
  EXPECT_EQ(rec.done[0].stackThread, 9u);
}

TEST(StackMatcher, DuplicateCandidateThreadsAreCollapsed) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 10, 5, 5);  // header == payload
  ByteVec s;
  EXPECT_EQ(m.onStack(walk(s, 10, 5, {0x1})).role, MatchRole::PayloadThread);  // first candidate wins
  m.advance(10);
  EXPECT_EQ(rec.done.size(), 1u);
}

TEST(StackMatcher, KernelThenUserStacksAreConcatenated) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 100, 5, 5);
  ByteVec s;
  EXPECT_EQ(m.onStack(walk(s, 100, 5, {kK + 1, kK + 2})).kind, StackMatchResult::Kind::Matched);
  m.advance(100);
  EXPECT_TRUE(rec.done.empty());  // kernel-only: still waiting for the user half
  EXPECT_EQ(m.onStack(walk(s, 100, 5, {0x401000, 0x402000})).kind, StackMatchResult::Kind::MatchedSecondPart);
  m.advance(100);
  ASSERT_EQ(rec.done.size(), 1u);
  EXPECT_TRUE(rec.done[0].twoPart);
  EXPECT_EQ(rec.done[0].frames, (std::vector<uint64_t>{kK + 1, kK + 2, 0x401000, 0x402000}));
}

TEST(StackMatcher, KernelOnlyStackIsReleasedByTheWindow) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 100, 5, 5);
  ByteVec s;
  m.onStack(walk(s, 100, 5, {kK + 1}));
  m.advance(1100);
  EXPECT_TRUE(rec.done.empty());
  m.advance(1101);
  ASSERT_EQ(rec.done.size(), 1u);
  EXPECT_TRUE(rec.done[0].hasStack);
  EXPECT_FALSE(rec.done[0].twoPart);
  EXPECT_EQ(rec.done[0].frames, (std::vector<uint64_t>{kK + 1}));
  // Keys are gone: a late user half is unmatched.
  EXPECT_EQ(m.onStack(walk(s, 100, 5, {0x1})).kind, StackMatchResult::Kind::Unmatched);
}

TEST(StackMatcher, StacklessParentLeavesAfterTheWindowWithoutFrames) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 100, 5, 5);
  m.advance(1100);
  EXPECT_TRUE(rec.done.empty());
  m.advance(1101);
  ASSERT_EQ(rec.done.size(), 1u);
  EXPECT_FALSE(rec.done[0].hasStack);
  EXPECT_TRUE(rec.done[0].frames.empty());
}

TEST(StackMatcher, OutputKeepsArrivalOrderBehindAStalledParent) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 100, 5, 5);  // never gets a stack
  addSample(m, 110, 6, 6);
  ByteVec s;
  m.onStack(walk(s, 110, 6, {0x1}));
  m.advance(500);
  EXPECT_TRUE(rec.done.empty());  // 110 is complete but 100 is first in line
  m.advance(1200);
  ASSERT_EQ(rec.done.size(), 2u);
  EXPECT_EQ(rec.done[0].ts, 100u);
  EXPECT_EQ(rec.done[1].ts, 110u);
}

TEST(StackMatcher, FlushReleasesEverythingInOrder) {
  Recorder rec;
  StackMatcher m(1'000'000, rec);
  for (uint64_t i = 0; i < 5; ++i)
    addSample(m, 100 + i, static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i + 1));
  ByteVec s;
  m.onStack(walk(s, 102, 3, {0x1}));
  m.flush();
  ASSERT_EQ(rec.done.size(), 5u);
  for (size_t i = 0; i < 5; ++i) EXPECT_EQ(rec.done[i].ts, 100 + i);
  EXPECT_TRUE(rec.done[2].hasStack);
  EXPECT_FALSE(rec.done[3].hasStack);
  EXPECT_EQ(m.pending(), 0u);
}

TEST(StackMatcher, ManyPendingParentsGrowRingAndTable) {
  Recorder rec;
  StackMatcher m(~uint64_t{0} / 4, rec);
  constexpr uint32_t kN = 20'000;
  for (uint32_t i = 0; i < kN; ++i) addSample(m, 1000 + i / 4, i, i);  // timestamps collide in groups of 4
  EXPECT_EQ(m.pending(), kN);
  ByteVec s;
  // Answer in reverse order; every stack must find exactly its own parent.
  for (uint32_t i = kN; i-- > 0;) {
    auto r = m.onStack(walk(s, 1000 + i / 4, i, {0x1000 + i}));
    ASSERT_EQ(r.kind, StackMatchResult::Kind::Matched) << i;
  }
  m.advance(5000);
  ASSERT_EQ(rec.done.size(), kN);
  for (uint32_t i = 0; i < kN; ++i) {
    ASSERT_EQ(rec.done[i].frames.size(), 1u);
    EXPECT_EQ(rec.done[i].frames[0], 0x1000u + i);
  }
}

TEST(StackMatcher, RingStaysBoundedInSteadyState) {
  Recorder rec;
  StackMatcher m(100, rec);
  ByteVec s;
  for (uint64_t ts = 0; ts < 50'000; ++ts) {
    addSample(m, ts, 1, 1);
    m.onStack(walk(s, ts, 1, {0x1}));
    m.advance(ts);
  }
  m.flush();
  EXPECT_EQ(rec.done.size(), 50'000u);
  for (uint64_t i = 0; i < rec.done.size(); ++i) ASSERT_EQ(rec.done[i].ts, i);
}

TEST(StackMatcher, EmptyStackCompletesTheParent) {
  Recorder rec;
  StackMatcher m(1000, rec);
  addSample(m, 10, 1, 1);
  ByteVec s;
  EXPECT_EQ(m.onStack(walk(s, 10, 1, {})).kind, StackMatchResult::Kind::Matched);
  m.advance(10);
  ASSERT_EQ(rec.done.size(), 1u);
  EXPECT_TRUE(rec.done[0].hasStack);
  EXPECT_TRUE(rec.done[0].frames.empty());
}

}  // namespace
}  // namespace tachyon::etlconvert
