// Hand-built edge cases for buildThreadStates and stateAt.
#include "analysis/thread_states.h"

#include "analysis/wait_reasons.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace tachyon::analysis {
namespace {

using model::Thread;
using model::ThreadState;
using model::ThreadStateInterval;
using model::TraceModel;

constexpr uint64_t kStart = 1000;
constexpr uint64_t kEnd = 2000;
constexpr uint8_t kStateReady = 1;
constexpr uint8_t kStateWaiting = 5;
constexpr uint8_t kStateTerminated = 4;

struct Iv {
  uint64_t start, end;
  ThreadState state;
};

class Builder {
 public:
  explicit Builder(std::initializer_list<uint32_t> tids, uint32_t platform = trace::kPlatformWindows) {
    m.header.platform = platform;
    m.header.start_ts = kStart;
    m.header.end_ts = kEnd;
    m.header.clock_frequency_hz = 1'000'000'000;
    for (uint32_t tid : tids) {
      Thread t;
      t.tid = tid;
      m.tidToThread.emplace(tid, ThreadIdx(static_cast<uint32_t>(m.threads.size())));
      m.threads.push_back(t);
    }
    m.samplesByThread.resize(m.threads.size());
    m.statesByThread.resize(m.threads.size());
    m.scopesByThread.resize(m.threads.size());
  }

  // Thread `tid` switches out to idle.
  Builder& out(uint64_t ts, uint32_t tid, uint8_t oldState = kStateWaiting, uint8_t reason = 6,
               uint32_t stack = 7, uint32_t cpu = 0) {
    return sw(ts, cpu, tid, 0, oldState, reason, stack);
  }
  // Thread `tid` switches in from idle.
  Builder& in(uint64_t ts, uint32_t tid, uint32_t cpu = 0) { return sw(ts, cpu, 0, tid, 0, 0, kInvalidIdx); }

  Builder& sw(uint64_t ts, uint32_t cpu, uint32_t oldTid, uint32_t newTid, uint8_t oldState, uint8_t reason,
              uint32_t stack) {
    trace::ContextSwitchRec r{};
    r.ts = ts;
    r.cpu = cpu;
    r.old_tid = oldTid;
    r.new_tid = newTid;
    r.old_state = oldState;
    r.old_wait_reason = reason;
    r.old_thread_stack = CallstackIdx(stack);
    m.contextSwitches.push_back(r);
    return *this;
  }

  Builder& ready(uint64_t ts, uint32_t readied, uint32_t readying = 0, uint32_t stack = kInvalidIdx) {
    trace::ReadyRec r{};
    r.ts = ts;
    r.readied_tid = readied;
    r.readying_tid = readying;
    r.readying_stack = CallstackIdx(stack);
    m.readyEvents.push_back(r);
    return *this;
  }

  TraceModel& build(unsigned threads = 1) {
    std::stable_sort(
        m.contextSwitches.begin(), m.contextSwitches.end(),
        [](const trace::ContextSwitchRec& a, const trace::ContextSwitchRec& b) { return a.ts < b.ts; });
    std::stable_sort(m.readyEvents.begin(), m.readyEvents.end(),
                     [](const trace::ReadyRec& a, const trace::ReadyRec& b) { return a.ts < b.ts; });
    BuildOptions o;
    o.threads = threads;
    EXPECT_TRUE(buildThreadStates(m, o).ok());
    return m;
  }

  const std::vector<ThreadStateInterval>& states(uint32_t tid) const {
    return m.statesByThread[m.tidToThread.at(tid).get()];
  }

  TraceModel m;
};

void expectShape(const std::vector<ThreadStateInterval>& got, const std::vector<Iv>& want) {
  ASSERT_EQ(got.size(), want.size());
  for (size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(got[i].start, want[i].start) << i;
    EXPECT_EQ(got[i].end, want[i].end) << i;
    EXPECT_EQ(got[i].state, want[i].state) << i;
  }
}

constexpr ThreadState R = ThreadState::Running;
constexpr ThreadState Rd = ThreadState::Ready;
constexpr ThreadState W = ThreadState::Waiting;
constexpr ThreadState U = ThreadState::Unknown;

TEST(ThreadStatesEdge, ThreadWithNoEventsIsUnknownThroughout) {
  Builder b({10, 11});
  b.in(1100, 11);
  b.build();
  expectShape(b.states(10), {{kStart, kEnd, U}});
}

TEST(ThreadStatesEdge, SwitchInAtStartIsRunningFromStart) {
  Builder b({10});
  b.in(kStart, 10, 3).out(1500, 10);
  b.build();
  expectShape(b.states(10), {{kStart, 1500, R}, {1500, kEnd, W}});
  EXPECT_EQ(b.states(10)[0].cpu, 3u);
}

TEST(ThreadStatesEdge, RunningAtTraceStartWithoutSwitchInIsUnknownUntilFirstEvent) {
  Builder b({10});
  b.out(1300, 10, kStateWaiting, 9, 42, 2).in(1600, 10, 1);
  b.build();
  expectShape(b.states(10), {{kStart, 1300, U}, {1300, 1600, W}, {1600, kEnd, R}});
  const ThreadStateInterval& w = b.states(10)[1];
  EXPECT_EQ(w.wait_reason, 9);
  EXPECT_EQ(w.blocking_stack, CallstackIdx(42));
  EXPECT_FALSE(w.unblocking_stack.valid());
  EXPECT_EQ(w.woke_by_tid, 0u);
  EXPECT_EQ(w.ready_ts, 0u);
  EXPECT_EQ(w.cpu, 2u);
  EXPECT_EQ(b.states(10)[2].cpu, 1u);
  EXPECT_EQ(b.states(10)[0].cpu, 0u);
  EXPECT_FALSE(b.states(10)[0].blocking_stack.valid());
}

TEST(ThreadStatesEdge, ReadySplitCarriesTheSameMetadataOnBothPieces) {
  Builder b({10, 20});
  b.in(kStart, 10).out(1200, 10, kStateWaiting, 6, 5).ready(1350, 10, 20, 8).in(1400, 10, 1);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1200, R}, {1200, 1350, W}, {1350, 1400, Rd}, {1400, kEnd, R}});
  for (int i : {1, 2}) {
    EXPECT_EQ(v[i].blocking_stack, CallstackIdx(5)) << i;
    EXPECT_EQ(v[i].unblocking_stack, CallstackIdx(8)) << i;
    EXPECT_EQ(v[i].woke_by_tid, 20u) << i;
    EXPECT_EQ(v[i].ready_ts, 1350u) << i;
    EXPECT_EQ(v[i].wait_reason, 6) << i;
  }
  // Ready latency is end - ready_ts.
  EXPECT_EQ(v[2].end - v[2].ready_ts, 50u);
}

TEST(ThreadStatesEdge, OnlyTheFirstReadyRecOfAWaitCounts) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10).ready(1300, 10, 1, 1).ready(1350, 10, 2, 2).in(1400, 10);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1200, R}, {1200, 1300, W}, {1300, 1400, Rd}, {1400, kEnd, R}});
  EXPECT_EQ(v[2].woke_by_tid, 1u);
  EXPECT_EQ(v[2].unblocking_stack, CallstackIdx(1));
}

TEST(ThreadStatesEdge, ReadyRecWithUnknownWakerStack) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10).ready(1300, 10, 99).in(1400, 10);
  b.build();
  const auto& v = b.states(10);
  EXPECT_EQ(v[1].woke_by_tid, 99u);
  EXPECT_FALSE(v[1].unblocking_stack.valid());
}

TEST(ThreadStatesEdge, PreemptedSwitchOutIsOneReadyInterval) {
  for (uint8_t st : {uint8_t(1), uint8_t(3), uint8_t(7)}) {
    Builder b({10});
    b.in(kStart, 10).out(1200, 10, st, 32, 11).in(1260, 10);
    b.build();
    const auto& v = b.states(10);
    expectShape(v, {{kStart, 1200, R}, {1200, 1260, Rd}, {1260, kEnd, R}});
    EXPECT_EQ(v[1].blocking_stack, CallstackIdx(11));
    EXPECT_FALSE(v[1].unblocking_stack.valid());
    EXPECT_EQ(v[1].woke_by_tid, 0u);
    EXPECT_EQ(v[1].ready_ts, 1200u);  // runnable since the switch-out
  }
}

TEST(ThreadStatesEdge, ReadyRecOnPreemptedIntervalAttachesWithoutSplitting) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10, kStateReady, 0, 11).ready(1230, 10, 5, 6).in(1260, 10);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1200, R}, {1200, 1260, Rd}, {1260, kEnd, R}});
  EXPECT_EQ(v[1].unblocking_stack, CallstackIdx(6));
  EXPECT_EQ(v[1].woke_by_tid, 5u);
  EXPECT_EQ(v[1].ready_ts, 1230u);
}

TEST(ThreadStatesEdge, DoubleSwitchInIsALostEventGap) {
  Builder b({10});
  b.in(1100, 10, 1).in(1500, 10, 2).out(1700, 10);
  b.build();
  // The interval from the last known change (1100) to the second switch-in is Unknown, merged with the
  // prefix.
  expectShape(b.states(10), {{kStart, 1500, U}, {1500, 1700, R}, {1700, kEnd, W}});
  EXPECT_EQ(b.states(10)[1].cpu, 2u);
}

TEST(ThreadStatesEdge, DoubleSwitchOutIsALostEventGap) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10, kStateWaiting, 6, 1).out(1500, 10, kStateWaiting, 9, 2).in(1800, 10);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1200, R}, {1200, 1500, U}, {1500, 1800, W}, {1800, kEnd, R}});
  EXPECT_EQ(v[2].blocking_stack, CallstackIdx(2));
  EXPECT_EQ(v[2].wait_reason, 9);
  EXPECT_FALSE(v[1].blocking_stack.valid());
}

TEST(ThreadStatesEdge, GapDropsPendingReadyInformation) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10).ready(1300, 10, 5, 5).out(1500, 10).in(1700, 10);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1200, R}, {1200, 1500, U}, {1500, 1700, W}, {1700, kEnd, R}});
  EXPECT_EQ(v[2].woke_by_tid, 0u);
  EXPECT_EQ(v[2].ready_ts, 0u);
}

TEST(ThreadStatesEdge, ReadyRecWithNoLaterSwitchInEndsTheTraceReady) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10).ready(1500, 10, 20, 3);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1200, R}, {1200, 1500, W}, {1500, kEnd, Rd}});
  EXPECT_EQ(v[2].woke_by_tid, 20u);
  EXPECT_EQ(v[2].ready_ts, 1500u);
}

TEST(ThreadStatesEdge, WaitWithoutReadyRecRunsToTraceEnd) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10);
  b.build();
  expectShape(b.states(10), {{kStart, 1200, R}, {1200, kEnd, W}});
}

TEST(ThreadStatesEdge, ReadyRecWhileRunningOrUnknownIsIgnored) {
  Builder b({10});
  b.ready(1050, 10, 20, 1)  // Unknown prefix
      .in(1100, 10)
      .ready(1200, 10, 20, 2)  // Running
      .out(1300, 10)
      .in(1400, 10);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1100, U}, {1100, 1300, R}, {1300, 1400, W}, {1400, kEnd, R}});
  EXPECT_EQ(v[2].woke_by_tid, 0u);
  EXPECT_FALSE(v[2].unblocking_stack.valid());
}

TEST(ThreadStatesEdge, SameTimestampOrdering) {
  Builder b({10});
  // ReadyRec at the switch-out ts is applied after the switch-out (whole interval is Ready);
  // a ReadyRec at the switch-in ts is applied before it (no zero-length pieces).
  b.in(kStart, 10)
      .out(1200, 10)
      .ready(1200, 10, 7, 1)
      .in(1300, 10)
      .out(1400, 10)
      .ready(1500, 10, 8, 2)
      .in(1500, 10);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1200, R}, {1200, 1300, Rd}, {1300, 1400, R}, {1400, 1500, W}, {1500, kEnd, R}});
  EXPECT_EQ(v[1].woke_by_tid, 7u);
  EXPECT_EQ(v[3].woke_by_tid, 8u);
  EXPECT_EQ(v[3].ready_ts, 1500u);
}

TEST(ThreadStatesEdge, TerminatedSwitchOutEndsTheThread) {
  Builder b({10});
  b.in(kStart, 10).out(1400, 10, kStateTerminated).in(1600, 10).out(1700, 10);
  b.build();
  expectShape(b.states(10), {{kStart, 1400, R}});
}

TEST(ThreadStatesEdge, LinuxExitEncodingEndsTheThread) {
  Builder b({10}, trace::kPlatformLinux);
  b.in(kStart, 10).out(1400, 10, kStateTerminated, 0x20);
  b.build();
  expectShape(b.states(10), {{kStart, 1400, R}});
}

TEST(ThreadStatesEdge, ThreadRecEndTimeEndsTheThreadAndLaterEventsAreIgnored) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10).in(1300, 10).out(1800, 10);
  // ThreadRec::end_ts 1900: later than every event, so it is an exit time and not an event-derived last_ts.
  b.m.threads[0].last_ts = 1900;
  b.build();
  expectShape(b.states(10), {{kStart, 1200, R}, {1200, 1300, W}, {1300, 1800, R}, {1800, 1900, W}});
}

TEST(ThreadStatesEdge, EventDerivedLastTsIsNotAnExit) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10);
  b.m.threads[0].last_ts = 1200;  // what the loader sets when the ThreadRec has no end time
  b.build();
  expectShape(b.states(10), {{kStart, 1200, R}, {1200, kEnd, W}});
}

TEST(ThreadStatesEdge, ExitWhileRunningClosesTheRunningInterval) {
  Builder b({10});
  b.in(kStart, 10);
  b.m.threads[0].last_ts = 1600;
  b.build();
  expectShape(b.states(10), {{kStart, 1600, R}});
}

TEST(ThreadStatesEdge, InterleavedCpusAndDirectSwitches) {
  Builder b({10, 20});
  // Direct switches with both sides non-zero, on two CPUs.
  b.in(kStart, 10, 0).out(1050, 20, kStateWaiting, 6, 9);
  b.sw(1200, 0, 10, 20, kStateReady, 0, 4);    // cpu0: 10 preempted by 20 (20 was ready, no ReadyRec)
  b.sw(1300, 1, 20, 10, kStateWaiting, 6, 5);  // cpu1: 20 waits, 10 comes back on cpu1
  b.sw(1500, 1, 10, 0, kStateWaiting, 6, 6);
  b.in(1600, 20, 0);
  b.build(2);
  expectShape(b.states(10), {{kStart, 1200, R}, {1200, 1300, Rd}, {1300, 1500, R}, {1500, kEnd, W}});
  EXPECT_EQ(b.states(10)[0].cpu, 0u);
  EXPECT_EQ(b.states(10)[2].cpu, 1u);
  EXPECT_EQ(b.states(10)[1].blocking_stack, CallstackIdx(4));
  expectShape(b.states(20),
              {{kStart, 1050, U}, {1050, 1200, W}, {1200, 1300, R}, {1300, 1600, W}, {1600, kEnd, R}});
  EXPECT_EQ(b.states(20)[2].cpu, 0u);
}

TEST(ThreadStatesEdge, RecordWithSameOldAndNewTidIsIgnored) {
  Builder b({10});
  b.in(kStart, 10).sw(1200, 0, 10, 10, kStateWaiting, 6, 1);
  b.build();
  expectShape(b.states(10), {{kStart, kEnd, R}});
}

TEST(ThreadStatesEdge, EventsOutsideTheTraceRangeAreClamped) {
  Builder b({10});
  b.in(900, 10).out(1100, 10).in(kEnd + 50, 10);
  b.build();
  expectShape(b.states(10), {{kStart, 1100, R}, {1100, kEnd, W}});
}

TEST(ThreadStatesEdge, LinuxEncodingKeepsPrevStateInWaitReason) {
  Builder b({10}, trace::kPlatformLinux);
  b.in(kStart, 10)
      .out(1100, 10, kStateWaiting, 0x01, 1)
      .in(1200, 10)
      .out(1300, 10, kStateWaiting, 0x02, 2)
      .in(1400, 10);
  b.out(1500, 10, kStateReady, 0x00, 3);
  b.build();
  const auto& v = b.states(10);
  expectShape(v, {{kStart, 1100, R},
                  {1100, 1200, W},
                  {1200, 1300, R},
                  {1300, 1400, W},
                  {1400, 1500, R},
                  {1500, kEnd, Rd}});
  EXPECT_EQ(v[1].wait_reason, 0x01);
  EXPECT_EQ(v[3].wait_reason, 0x02);
  EXPECT_EQ(v[5].blocking_stack, CallstackIdx(3));
  EXPECT_EQ(waitReasonCategory(trace::kPlatformLinux, v[1].wait_reason), WaitCategory::Synchronization);
  EXPECT_EQ(waitReasonCategory(trace::kPlatformLinux, v[3].wait_reason), WaitCategory::IO);
}

TEST(ThreadStatesEdge, DegenerateTraceRangeGivesNoIntervals) {
  Builder b({10});
  b.m.header.end_ts = b.m.header.start_ts;
  b.in(kStart, 10);
  b.build();
  EXPECT_TRUE(b.states(10).empty());
}

TEST(ThreadStatesEdge, RebuildReplacesPreviousIntervals) {
  Builder b({10});
  b.in(kStart, 10).out(1200, 10);
  b.build();
  b.build();
  expectShape(b.states(10), {{kStart, 1200, R}, {1200, kEnd, W}});
}

TEST(ThreadStatesEdge, StateAtBinarySearch) {
  Builder b({10, 11});
  b.in(kStart, 10).out(1200, 10).in(1300, 10).out(1400, 10);
  b.build();
  const ThreadIdx t = b.m.tidToThread.at(10);
  EXPECT_EQ(stateAt(b.m, t, kStart - 1), nullptr);
  EXPECT_EQ(stateAt(b.m, t, kStart)->state, R);
  EXPECT_EQ(stateAt(b.m, t, 1199)->state, R);
  EXPECT_EQ(stateAt(b.m, t, 1200)->state, W);
  EXPECT_EQ(stateAt(b.m, t, 1299)->state, W);
  EXPECT_EQ(stateAt(b.m, t, 1300)->state, R);
  EXPECT_EQ(stateAt(b.m, t, 1400)->state, W);
  EXPECT_EQ(stateAt(b.m, t, kEnd - 1)->state, W);
  EXPECT_EQ(stateAt(b.m, t, kEnd), nullptr);
  EXPECT_EQ(stateAt(b.m, ThreadIdx(99), 1100), nullptr);
  EXPECT_EQ(stateAt(b.m, b.m.tidToThread.at(11), 1100)->state, U);
  // Every timestamp of the range maps to exactly the interval that contains it.
  for (uint64_t ts = kStart; ts < kEnd; ts += 7) {
    const ThreadStateInterval* iv = stateAt(b.m, t, ts);
    ASSERT_NE(iv, nullptr);
    EXPECT_LE(iv->start, ts);
    EXPECT_LT(ts, iv->end);
  }
}

TEST(ThreadStatesEdge, IdenticalResultsForAnyWorkerCount) {
  auto make = [](unsigned threads) {
    Builder b({10, 11, 12, 13, 14});
    for (uint32_t i = 0; i < 5; ++i) {
      const uint32_t tid = 10 + i;
      for (uint64_t t = kStart + i * 3; t + 40 < kEnd; t += 100) {
        b.in(t, tid, i).out(t + 30, tid, (t / 100) % 3 == 0 ? kStateReady : kStateWaiting, 6, i);
        if ((t / 100) % 2 == 0) b.ready(t + 35, tid, 10 + (i + 1) % 5, i);
      }
    }
    b.build(threads);
    return b.m.statesByThread;
  };
  const auto a = make(1);
  for (unsigned n : {2u, 3u, 8u}) {
    const auto c = make(n);
    ASSERT_EQ(a.size(), c.size());
    for (size_t t = 0; t < a.size(); ++t) {
      ASSERT_EQ(a[t].size(), c[t].size());
      for (size_t i = 0; i < a[t].size(); ++i) {
        EXPECT_EQ(a[t][i].start, c[t][i].start);
        EXPECT_EQ(a[t][i].end, c[t][i].end);
        EXPECT_EQ(a[t][i].state, c[t][i].state);
        EXPECT_EQ(a[t][i].cpu, c[t][i].cpu);
        EXPECT_EQ(a[t][i].wait_reason, c[t][i].wait_reason);
        EXPECT_EQ(a[t][i].blocking_stack, c[t][i].blocking_stack);
        EXPECT_EQ(a[t][i].unblocking_stack, c[t][i].unblocking_stack);
        EXPECT_EQ(a[t][i].woke_by_tid, c[t][i].woke_by_tid);
        EXPECT_EQ(a[t][i].ready_ts, c[t][i].ready_ts);
      }
    }
  }
}

}  // namespace
}  // namespace tachyon::analysis
