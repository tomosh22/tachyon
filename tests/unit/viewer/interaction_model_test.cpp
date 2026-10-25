// WP-46: wait episodes, longest-waits ranking, stack formatting, tooltip lines and navigation targets
// (pure logic, no ImGui).
#include "viewer/interaction/interaction_model.h"
#include "analysis/thread_states.h"
#include "viewer/timeline/timeline_data.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

using namespace tachyon;
using namespace tachyon::viewer::interaction;
using model::ThreadState;
using model::ThreadStateInterval;

namespace fs = std::filesystem;

namespace {

ThreadStateInterval iv(uint64_t start, uint64_t end, ThreadState state) {
  ThreadStateInterval i;
  i.start = start;
  i.end = end;
  i.state = state;
  return i;
}

// Thread 0 (tid 10) is the interesting one; thread 1 (tid 20) is the waker and only runs.
//  [0,100) Running
//  [100,250) Waiting + [250,300) Ready   wake-up at 250 by tid 20, blocking 2, unblocking 3
//  [300,400) Running
//  [400,500) Waiting, no ReadyRec
//  [500,600) Running
//  [600,650) Ready, preempted, no ReadyRec (ready_ts = start)
//  [650,700) Running
//  [700,760) Ready, preempted, ReadyRec at 730 by tid 20
//  [760,800) Unknown
//  [800,900) Waiting, no ReadyRec (trace ends while waiting)
model::TraceModel makeModel() {
  model::TraceModel m;
  m.header.start_ts = 0;
  m.header.end_ts = 900;
  m.header.clock_frequency_hz = 1'000'000'000;
  m.header.platform = 2;  // Linux
  m.threads.resize(3);
  m.threads[0].tid = 10;
  m.threads[0].name = "Main";
  m.threads[1].tid = 20;
  m.threads[1].name = "Worker";
  m.threads[2].tid = 30;  // never woken, never waits
  for (uint32_t i = 0; i < 3; ++i) m.tidToThread[m.threads[i].tid] = ThreadIdx(i);
  m.statesByThread.resize(3);
  m.samplesByThread.resize(3);
  m.scopesByThread.resize(3);

  auto& a = m.statesByThread[0];
  a.push_back(iv(0, 100, ThreadState::Running));
  ThreadStateInterval w = iv(100, 250, ThreadState::Waiting);
  w.wait_reason = 1;
  w.cpu = 3;
  w.blocking_stack = CallstackIdx(2);
  w.unblocking_stack = CallstackIdx(3);
  w.woke_by_tid = 20;
  w.ready_ts = 250;
  a.push_back(w);
  ThreadStateInterval r = w;
  r.start = 250;
  r.end = 300;
  r.state = ThreadState::Ready;
  a.push_back(r);
  a.push_back(iv(300, 400, ThreadState::Running));
  ThreadStateInterval w2 = iv(400, 500, ThreadState::Waiting);
  w2.wait_reason = 2;
  w2.blocking_stack = CallstackIdx(2);
  a.push_back(w2);
  a.push_back(iv(500, 600, ThreadState::Running));
  ThreadStateInterval p = iv(600, 650, ThreadState::Ready);
  p.ready_ts = 600;
  p.blocking_stack = CallstackIdx(2);
  a.push_back(p);
  a.push_back(iv(650, 700, ThreadState::Running));
  ThreadStateInterval p2 = iv(700, 760, ThreadState::Ready);
  p2.ready_ts = 730;
  p2.woke_by_tid = 20;
  p2.unblocking_stack = CallstackIdx(3);
  p2.blocking_stack = CallstackIdx(2);
  a.push_back(p2);
  a.push_back(iv(760, 800, ThreadState::Unknown));
  ThreadStateInterval w3 = iv(800, 900, ThreadState::Waiting);
  w3.blocking_stack = CallstackIdx(2);
  a.push_back(w3);
  m.statesByThread[1].push_back(iv(0, 900, ThreadState::Running));
  m.statesByThread[2].push_back(iv(0, 900, ThreadState::Running));

  // Callstacks: 0 = root; 1 = main; 2 = main > Job::wait > NtWait (blocking); 3 = main > signal (unblocking).
  m.modules.resize(1);
  m.modules[0].path = "/usr/lib/libengine.so";
  m.frames.push_back({ModuleIdx(0), 0x1000});              // 0 main
  m.frames.push_back({ModuleIdx(0), 0x2000});              // 1 wait
  m.frames.push_back({ModuleIdx(0), 0x3000});              // 2 sched leaf
  m.frames.push_back({ModuleIdx(0), 0x4000});              // 3 signal
  m.frames.push_back({ModuleIdx::invalid(), 0x7ffe0000});  // 4 unmapped
  m.nodes.resize(7);
  m.nodes[0] = {CallstackIdx::invalid(), FrameIdx::invalid()};
  m.nodes[1] = {CallstackIdx(0), FrameIdx(0)};
  m.nodes[2] = {CallstackIdx(1), FrameIdx(1)};
  m.nodes[3] = {CallstackIdx(1), FrameIdx(3)};
  m.nodes[4] = {CallstackIdx(2), FrameIdx(2)};  // leaf of the blocking stack
  m.nodes[5] = {CallstackIdx(3), FrameIdx(4)};
  m.nodes[6] = {CallstackIdx(5), FrameIdx(1)};
  return m;
}

}  // namespace

TEST(InteractionEpisode, SplitWaitCoversWaitingAndReadyPieces) {
  const model::TraceModel m = makeModel();
  for (const size_t index : {size_t{1}, size_t{2}}) {  // either piece gives the same episode
    const auto e = episodeForInterval(m, ThreadIdx(0), index);
    ASSERT_TRUE(e) << index;
    EXPECT_EQ(e->firstInterval, 1u);
    EXPECT_EQ(e->lastInterval, 2u);
    EXPECT_EQ(e->originState, ThreadState::Waiting);
    EXPECT_EQ(e->switchOutTs, 100u);
    EXPECT_EQ(e->switchInTs, 300u);
    EXPECT_TRUE(e->hasReadyRec);
    EXPECT_EQ(e->readyTs, 250u);
    EXPECT_EQ(e->waitTicks, 150u);
    EXPECT_EQ(e->readyLatencyTicks, 50u);
    EXPECT_TRUE(e->readyLatencyKnown);
    EXPECT_TRUE(e->hasWaker);
    EXPECT_EQ(e->wakerTid, 20u);
    EXPECT_EQ(e->wakerThread, ThreadIdx(1));
    EXPECT_EQ(e->blockingStack, CallstackIdx(2));
    EXPECT_EQ(e->unblockingStack, CallstackIdx(3));
    EXPECT_EQ(e->waitReason, 1);
    EXPECT_EQ(e->cpu, 3u);
  }
}

TEST(InteractionEpisode, WaitWithoutReadyRecHasNoWakerAndUnknownLatency) {
  const model::TraceModel m = makeModel();
  const auto e = episodeAt(m, ThreadIdx(0), 450);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->switchOutTs, 400u);
  EXPECT_EQ(e->switchInTs, 500u);
  EXPECT_FALSE(e->hasReadyRec);
  EXPECT_FALSE(e->hasWaker);
  EXPECT_EQ(e->waitTicks, 100u);  // switch-out -> switch-in
  EXPECT_EQ(e->readyLatencyTicks, 0u);
  EXPECT_FALSE(e->readyLatencyKnown);
  EXPECT_FALSE(e->unblockingStack.valid());
  EXPECT_FALSE(e->wakerThread.valid());
}

TEST(InteractionEpisode, PreemptedIntervalWithoutReadyRecIsRunnableFromTheSwitchOut) {
  const model::TraceModel m = makeModel();
  const auto e = episodeAt(m, ThreadIdx(0), 620);
  ASSERT_TRUE(e);
  EXPECT_FALSE(e->isWait());
  EXPECT_EQ(e->originState, ThreadState::Ready);
  EXPECT_EQ(e->switchOutTs, 600u);
  EXPECT_EQ(e->switchInTs, 650u);
  EXPECT_FALSE(e->hasWaker);
  EXPECT_FALSE(e->hasReadyRec);
  EXPECT_EQ(e->readyTs, 600u);
  EXPECT_EQ(e->waitTicks, 0u);
  EXPECT_EQ(e->readyLatencyTicks, 50u);
  EXPECT_TRUE(e->readyLatencyKnown);
}

TEST(InteractionEpisode, PreemptedIntervalWithReadyRecKeepsTheWaker) {
  const model::TraceModel m = makeModel();
  const auto e = episodeAt(m, ThreadIdx(0), 705);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->originState, ThreadState::Ready);
  EXPECT_EQ(e->firstInterval, e->lastInterval);
  EXPECT_TRUE(e->hasWaker);
  EXPECT_TRUE(e->hasReadyRec);
  EXPECT_EQ(e->readyTs, 730u);
  EXPECT_EQ(e->waitTicks, 30u);
  EXPECT_EQ(e->readyLatencyTicks, 30u);
  EXPECT_EQ(e->wakerThread, ThreadIdx(1));
}

TEST(InteractionEpisode, WaitRunningAtTheTraceEndHasNoLatency) {
  const model::TraceModel m = makeModel();
  const auto e = episodeAt(m, ThreadIdx(0), 850);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->switchInTs, 900u);  // end of coverage
  EXPECT_EQ(e->waitTicks, 100u);
  EXPECT_FALSE(e->readyLatencyKnown);
}

TEST(InteractionEpisode, RunningUnknownAndOutOfRangeHaveNoEpisode) {
  const model::TraceModel m = makeModel();
  EXPECT_FALSE(episodeAt(m, ThreadIdx(0), 50));    // Running
  EXPECT_FALSE(episodeAt(m, ThreadIdx(0), 780));   // Unknown gap
  EXPECT_FALSE(episodeAt(m, ThreadIdx(0), 5000));  // beyond coverage
  EXPECT_FALSE(episodeAt(m, ThreadIdx(), 450));
  EXPECT_FALSE(episodeAt(m, ThreadIdx(9), 450));
  EXPECT_FALSE(episodeForInterval(m, ThreadIdx(0), 99));
}

TEST(InteractionEpisode, StartingAtFindsOnlyTheSwitchOut) {
  const model::TraceModel m = makeModel();
  const auto e = episodeStartingAt(m, ThreadIdx(0), 100);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->switchInTs, 300u);
  EXPECT_FALSE(episodeStartingAt(m, ThreadIdx(0), 250));  // the Ready piece does not start an episode
  EXPECT_FALSE(episodeStartingAt(m, ThreadIdx(0), 101));
  EXPECT_FALSE(episodeStartingAt(m, ThreadIdx(0), 300));  // Running
  ASSERT_TRUE(episodeStartingAt(m, ThreadIdx(0), 600));
}

TEST(InteractionEpisode, DetailsFormatEveryField) {
  const model::TraceModel m = makeModel();
  const EpisodeDetails d = describeEpisode(m, *episodeAt(m, ThreadIdx(0), 120), 1e9);
  EXPECT_EQ(d.woken, "Main (tid 10)");
  EXPECT_EQ(d.waker, "Worker (tid 20)");
  EXPECT_EQ(d.waitReason, "Interruptible (S)");
  EXPECT_EQ(d.waitCategory, "Synchronization");
  EXPECT_EQ(d.waitDuration, "150 ns");
  EXPECT_EQ(d.readyLatency, "50 ns");
  const EpisodeDetails none = describeEpisode(m, *episodeAt(m, ThreadIdx(0), 450), 1e9);
  EXPECT_EQ(none.waker, "unknown (no wake-up record)");
  EXPECT_EQ(none.readyLatency, "n/a");
  const EpisodeDetails pre = describeEpisode(m, *episodeAt(m, ThreadIdx(0), 620), 1e9);
  EXPECT_EQ(pre.waitReason, "Preempted");
}

// ---- longest waits ----

namespace {

// Brute force over the raw intervals: the Waiting intervals overlapping [t0, t1), longest first.
std::vector<std::pair<uint32_t, uint64_t>> bruteForce(const model::TraceModel& m, uint64_t t0, uint64_t t1,
                                                      const std::optional<std::vector<ThreadIdx>>& threads,
                                                      size_t n, uint64_t* total = nullptr) {
  struct Row {
    uint64_t ticks, start;
    uint32_t thread;
  };
  std::vector<Row> rows;
  for (size_t t = 0; t < m.statesByThread.size(); ++t) {
    if (threads &&
        std::find(threads->begin(), threads->end(), ThreadIdx(static_cast<uint32_t>(t))) == threads->end()) {
      continue;
    }
    for (const ThreadStateInterval& s : m.statesByThread[t]) {
      if (s.state != ThreadState::Waiting) continue;
      if (s.end <= t0 || s.start >= t1) continue;
      rows.push_back({s.end - s.start, s.start, static_cast<uint32_t>(t)});
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.ticks != b.ticks) return a.ticks > b.ticks;
    if (a.start != b.start) return a.start < b.start;
    return a.thread < b.thread;
  });
  if (total) *total = rows.size();
  if (rows.size() > n) rows.resize(n);
  std::vector<std::pair<uint32_t, uint64_t>> out;
  for (const Row& r : rows) out.emplace_back(r.thread, r.start);
  return out;
}

std::vector<std::pair<uint32_t, uint64_t>> asKeys(const WaitScanResult& r) {
  std::vector<std::pair<uint32_t, uint64_t>> out;
  for (const WaitEpisode& e : r.rows) out.emplace_back(e.thread.get(), e.switchOutTs);
  return out;
}

}  // namespace

TEST(InteractionLongestWaits, RanksByWaitDurationOfTheSyntheticTrace) {
  const model::TraceModel m = makeModel();
  WaitQuery q;
  q.t0 = 0;
  const WaitScanResult r = longestWaits(m, q, nullptr);
  ASSERT_EQ(r.rows.size(), 3u);
  EXPECT_EQ(r.totalWaits, 3u);
  // 150 (split wait, wait ends at the wake-up), 100 (no ReadyRec, at 400), 100 (trace end, at 800).
  EXPECT_EQ(r.rows[0].switchOutTs, 100u);
  EXPECT_EQ(r.rows[0].waitTicks, 150u);
  EXPECT_EQ(r.rows[1].switchOutTs, 400u);  // tie on 100: the earlier switch-out first
  EXPECT_EQ(r.rows[2].switchOutTs, 800u);
  for (const WaitEpisode& e : r.rows) EXPECT_TRUE(e.isWait());  // preemptions are not waits
}

TEST(InteractionLongestWaits, RespectsRangeThreadsAndLimit) {
  const model::TraceModel m = makeModel();
  WaitQuery q;
  q.t0 = 0;
  q.t1 = 400;  // [t0, t1): the wait that starts at 400 is out
  EXPECT_EQ(longestWaits(m, q, nullptr).totalWaits, 1u);
  q.t0 = 249;  // overlaps the split wait's Waiting piece [100,250) by one tick
  EXPECT_EQ(longestWaits(m, q, nullptr).totalWaits, 1u);
  q.t0 = 250;  // the Waiting piece ended at the wake-up; only the Ready piece covers 250
  EXPECT_EQ(longestWaits(m, q, nullptr).totalWaits, 0u);
  q.t0 = 0;
  q.t1 = UINT64_MAX;
  q.maxResults = 1;
  const WaitScanResult one = longestWaits(m, q, nullptr);
  ASSERT_EQ(one.rows.size(), 1u);
  EXPECT_EQ(one.totalWaits, 3u);
  q.maxResults = 0;
  EXPECT_TRUE(longestWaits(m, q, nullptr).rows.empty());
  q.maxResults = 50;
  q.threads = std::vector<ThreadIdx>{ThreadIdx(1), ThreadIdx(2)};  // threads that never wait
  EXPECT_EQ(longestWaits(m, q, nullptr).totalWaits, 0u);
  q.threads = std::vector<ThreadIdx>{ThreadIdx(0), ThreadIdx(0), ThreadIdx(77)};  // duplicates, bad index
  EXPECT_EQ(longestWaits(m, q, nullptr).totalWaits, 3u);
}

TEST(InteractionLongestWaits, CancelledScanReturnsNothing) {
  // Needs enough intervals to reach the cancel check (every 4096).
  model::TraceModel m;
  m.threads.resize(1);
  m.statesByThread.resize(1);
  for (uint64_t i = 0; i < 20000; ++i) {
    m.statesByThread[0].push_back(iv(i * 2, i * 2 + 1, ThreadState::Running));
    m.statesByThread[0].push_back(iv(i * 2 + 1, i * 2 + 2, ThreadState::Waiting));
  }
  std::atomic<bool> cancel{true};
  const WaitScanResult r = longestWaits(m, WaitQuery{}, &cancel);
  EXPECT_TRUE(r.cancelled);
  EXPECT_TRUE(r.rows.empty());
  cancel = false;
  EXPECT_EQ(longestWaits(m, WaitQuery{}, &cancel).totalWaits, 20000u);
}

namespace {

model::TraceModel loadFixture(const char* name) {
  auto loaded = model::TraceModel::load(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name);
  EXPECT_TRUE(loaded.ok()) << (loaded.ok() ? "" : loaded.error().toString());
  model::TraceModel m = std::move(loaded).value();
  EXPECT_TRUE(analysis::buildThreadStates(m).ok());
  return m;
}

void checkAgainstBruteForce(const model::TraceModel& m) {
  const uint64_t a = m.header.start_ts, b = m.header.end_ts;
  const uint64_t span = b - a;
  struct Case {
    uint64_t t0, t1;
    std::optional<std::vector<ThreadIdx>> threads;
    size_t n;
  };
  std::vector<Case> cases = {
      {a, UINT64_MAX, std::nullopt, 50},
      {a, UINT64_MAX, std::nullopt, 5},
      {a + span / 4, a + span / 2, std::nullopt, 50},
      {a + span / 3, a + span / 3 + 1, std::nullopt, 50},
      {b, b + 100, std::nullopt, 50},
      {a, UINT64_MAX, std::vector<ThreadIdx>{ThreadIdx(0)}, 1000},
      {a + span / 5, a + 4 * span / 5, std::vector<ThreadIdx>{ThreadIdx(0), ThreadIdx(1)}, 20},
  };
  for (size_t i = 0; i < cases.size(); ++i) {
    const Case& c = cases[i];
    WaitQuery q;
    q.t0 = c.t0;
    q.t1 = c.t1;
    q.threads = c.threads;
    q.maxResults = c.n;
    const WaitScanResult r = longestWaits(m, q, nullptr);
    uint64_t total = 0;
    EXPECT_EQ(asKeys(r), bruteForce(m, c.t0, c.t1, c.threads, c.n, &total)) << "case " << i;
    EXPECT_EQ(r.totalWaits, total) << "case " << i;
    for (size_t k = 1; k < r.rows.size(); ++k) EXPECT_GE(r.rows[k - 1].waitTicks, r.rows[k].waitTicks);
  }
}

}  // namespace

TEST(InteractionLongestWaits, MatchesBruteForceOnTiny) {
  const model::TraceModel m = loadFixture("tiny.tachyon");
  checkAgainstBruteForce(m);
  // Known answer: the longest wait of `tiny` is the 20 ms wait of "Main" (tid 101) without a ReadyRec... or a
  // pattern wait; the ranking is checked against the brute force above, here we pin the scenario's explicit
  // intervals.
  WaitQuery q;
  q.t0 = m.header.start_ts;
  const WaitScanResult r = longestWaits(m, q, nullptr);
  ASSERT_FALSE(r.rows.empty());
  EXPECT_GE(r.rows.front().waitTicks, 200000u);  // 20 ms at 10 MHz
  EXPECT_GT(r.totalWaits, 10u);
}

TEST(InteractionLongestWaits, MatchesBruteForceOnMedium) {
  const model::TraceModel m = loadFixture("medium.tachyon");
  checkAgainstBruteForce(m);
}

TEST(InteractionEpisode, TinyScenarioKnownAnswers) {
  // tiny.json: Main (tid 101) is Unknown until its first switch-out at +40 ms; that wait is 8 ms with a
  // ReadyRec 7.5 ms in from Worker (tid 102) and a 0.5 ms ready latency (10 MHz clock).
  const model::TraceModel m = loadFixture("tiny.tachyon");
  ASSERT_EQ(m.header.clock_frequency_hz, 10'000'000u);
  const auto it = m.tidToThread.find(101);
  ASSERT_NE(it, m.tidToThread.end());
  const uint64_t out = m.header.start_ts + 400'000;  // 40 ms
  const auto e = episodeStartingAt(m, it->second, out);
  ASSERT_TRUE(e);
  EXPECT_TRUE(e->isWait());
  EXPECT_EQ(e->waitReason, 6);
  EXPECT_EQ(e->waitTicks, 75'000u);
  EXPECT_EQ(e->readyLatencyTicks, 5'000u);
  EXPECT_EQ(e->switchInTs - e->switchOutTs, 80'000u);
  EXPECT_TRUE(e->hasWaker);
  EXPECT_EQ(e->wakerTid, 102u);
  EXPECT_EQ(e->wakerThread, m.tidToThread.at(102));
  // Blocking stack: wait_block = start > main > Job::wait > NtWait, so the innermost of 4 frames first.
  const auto blocking = describeStack(m, nullptr, e->blockingStack);
  EXPECT_EQ(blocking.size(), 4u);
  const auto unblocking = describeStack(m, nullptr, e->unblockingStack);
  EXPECT_EQ(unblocking.size(), 2u);  // wake = RtlUserThreadStart > Sys::signal
  // The 20 ms wait at +348.1 ms has no ReadyRec.
  const auto second = episodeStartingAt(m, it->second, m.header.start_ts + 3'481'000);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->waitTicks, 200'000u);
  EXPECT_FALSE(second->hasWaker);
  EXPECT_FALSE(second->readyLatencyKnown);
}

// ---- stacks ----

TEST(InteractionStack, InnermostFirstWithModuleAndAddress) {
  const model::TraceModel m = makeModel();
  // Node 4 = main(0x1000) > wait(0x2000) > sched(0x3000).
  const auto lines = describeStack(m, nullptr, CallstackIdx(4));
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].text, "libengine.so!0x3000");
  EXPECT_EQ(lines[1].text, "libengine.so!0x2000");
  EXPECT_EQ(lines[2].text, "libengine.so!0x1000");
  EXPECT_TRUE(lines[0].exactAddress);  // the leaf is exact, outer frames are return addresses
  EXPECT_FALSE(lines[1].exactAddress);
  EXPECT_FALSE(lines[2].exactAddress);
  EXPECT_EQ(lines[0].position, 0u);
  EXPECT_EQ(lines[2].position, 2u);
  ASSERT_TRUE(lines[0].key);
  EXPECT_EQ(lines[0].key->module, ModuleIdx(0));
  EXPECT_EQ(lines[0].key->value, 0x3000u);
}

TEST(InteractionStack, UnmappedFramesAndLimits) {
  const model::TraceModel m = makeModel();
  const auto lines = describeStack(m, nullptr, CallstackIdx(6));  // wait > unmapped > signal > main
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[1].text, "0x7ffe0000");
  EXPECT_TRUE(lines[1].key && lines[1].key->isUnmapped());
  EXPECT_EQ(describeStack(m, nullptr, CallstackIdx(6), 2).size(), 2u);
  EXPECT_TRUE(describeStack(m, nullptr, CallstackIdx()).empty());
  EXPECT_TRUE(describeStack(m, nullptr, CallstackIdx(0)).empty());  // the root is the empty stack
  EXPECT_TRUE(describeStack(m, nullptr, CallstackIdx(1234)).empty());
}

// ---- tooltip ----

TEST(InteractionTooltip, ListsWakerTimingsAndTopFramesOfBothStacks) {
  const model::TraceModel m = makeModel();
  const auto e = episodeAt(m, ThreadIdx(0), 120);
  ASSERT_TRUE(e);
  // Make the blocking stack deep (node 4 has 3 frames; cap at 2 to see the ellipsis).
  model::TraceModel deep = m;
  deep.statesByThread[0][1].blocking_stack = CallstackIdx(4);
  deep.statesByThread[0][2].blocking_stack = CallstackIdx(4);
  const auto e2 = episodeAt(deep, ThreadIdx(0), 120);
  const auto lines = interactionTooltipLines(deep, nullptr, *e2, 1e9, 2);
  ASSERT_GE(lines.size(), 8u);
  EXPECT_EQ(lines[0], "Woken by: Worker (tid 20)");
  EXPECT_NE(lines[1].find("Wait: 150 ns"), std::string::npos);
  EXPECT_NE(lines[1].find("Ready latency: 50 ns"), std::string::npos);
  EXPECT_EQ(lines[2], "Blocking stack:");
  EXPECT_EQ(lines[3], "  libengine.so!0x3000");
  EXPECT_EQ(lines[4], "  libengine.so!0x2000");
  EXPECT_EQ(lines[5], "  ...");
  EXPECT_EQ(lines[6], "Unblocking stack:");
  EXPECT_EQ(lines[7], "  libengine.so!0x4000");
}

TEST(InteractionTooltip, TopEightFramesByDefaultAndMissingStacks) {
  model::TraceModel m = makeModel();
  // A 12-deep stack: chain of nodes on frame 0.
  const uint32_t base = static_cast<uint32_t>(m.nodes.size());
  for (uint32_t i = 0; i < 12; ++i) {
    m.nodes.push_back({i == 0 ? CallstackIdx(0) : CallstackIdx(base + i - 1), FrameIdx(i % 4)});
  }
  m.statesByThread[0][4].blocking_stack = CallstackIdx(base + 11);
  const auto e = episodeAt(m, ThreadIdx(0), 450);
  ASSERT_TRUE(e);
  const auto lines = interactionTooltipLines(m, nullptr, *e, 1e9);
  EXPECT_EQ(lines[0], "Woken by: unknown (no wake-up record)");
  EXPECT_NE(lines[1].find("Ready latency: n/a"), std::string::npos);
  size_t frames = 0;
  bool ellipsis = false, none = false;
  for (const std::string& l : lines) {
    if (l.rfind("  libengine.so!", 0) == 0) ++frames;
    if (l == "  ...") ellipsis = true;
    if (l == "  (none)") none = true;
  }
  EXPECT_EQ(frames, 8u);  // top 8 of 12
  EXPECT_TRUE(ellipsis);
  EXPECT_TRUE(none);  // no unblocking stack
}

TEST(InteractionTooltip, TooltipLinesAppendTheInteractionOnlyWhenAskedAndOnWaitingIntervals) {
  const model::TraceModel m = makeModel();
  viewer::timeline::TooltipInput in;
  in.model = &m;
  in.thread = ThreadIdx(0);
  in.ts = 120;
  in.clockHz = 1e9;
  in.relTimeNs = 120;
  const auto plain = viewer::timeline::tooltipLines(in);
  in.showInteraction = true;
  const auto withStacks = viewer::timeline::tooltipLines(in);
  EXPECT_GT(withStacks.size(), plain.size());
  EXPECT_TRUE(std::equal(plain.begin(), plain.end(), withStacks.begin()));  // the plain lines are unchanged
  EXPECT_NE(std::find(withStacks.begin(), withStacks.end(), "Blocking stack:"), withStacks.end());
  EXPECT_NE(std::find(withStacks.begin(), withStacks.end(), "Unblocking stack:"), withStacks.end());
  in.ts = 50;  // Running: nothing to add
  const auto running = viewer::timeline::tooltipLines(in);
  in.showInteraction = false;
  EXPECT_EQ(running, viewer::timeline::tooltipLines(in));
}

// ---- navigation ----

TEST(InteractionNavigation, WakerTargetCentersOnTheWakeUpAndCoversBothEnds) {
  const model::TraceModel m = makeModel();
  const auto e = *episodeAt(m, ThreadIdx(0), 120);
  const auto t = wakerTarget(e, 1e5);  // a slow clock: the 10 us minimum half-width is only a few ticks
  ASSERT_TRUE(t);
  EXPECT_EQ(t->thread, ThreadIdx(1));
  EXPECT_LE(t->t0, e.switchOutTs);  // the switch-out is in view too
  EXPECT_GE(t->t1, e.switchInTs);
  EXPECT_EQ((t->t0 + t->t1) / 2, e.readyTs);
  const auto none = *episodeAt(m, ThreadIdx(0), 450);
  EXPECT_FALSE(wakerTarget(none, 1e9));
}

TEST(InteractionNavigation, WokenTargetIsTheWholeEpisodeOnTheWokenThread) {
  const model::TraceModel m = makeModel();
  const auto e = *episodeAt(m, ThreadIdx(0), 120);
  const NavTarget t = wokenTarget(e, 1e9);
  EXPECT_EQ(t.thread, ThreadIdx(0));
  EXPECT_EQ(t.t0, 100u);
  EXPECT_GE(t.t1, 300u);
}
