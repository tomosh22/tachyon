// WP-71: conversion of synthetic scheduler event streams (tracepoint samples and the PERF_RECORD_SWITCH
// fallback) into ContextSwitchRec / ReadyRec, loaded through the model loader and turned into thread-state
// intervals by buildThreadStates.
#include "analysis/thread_states.h"
#include "capture/linux_perf/perf_converter.h"
#include "capture/linux_perf/sched_setup.h"
#include "linux_perf_test_util.h"
#include "model/model.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;

using ptest::TempDir;
using Id = RecordBuilder::Id;
using model::ThreadState;

constexpr uint64_t kStart = 1'000'000;
constexpr uint64_t kEnd = 9'000'000;
constexpr uint64_t kKernelA = 0xffffffff81100000ull;  // kernel text addresses of the synthetic stacks
constexpr uint64_t kKernelB = 0xffffffff81200000ull;
constexpr uint64_t kUserWait = 0x401100ull;  // user frames (unmapped: absolute)
constexpr uint64_t kUserUnlock = 0x402200ull;
constexpr uint64_t kUserMain = 0x403300ull;

constexpr uint32_t kHolder = 100;  // the target process leader, the "holder" thread
constexpr uint32_t kWaiter = 101;
constexpr uint32_t kOther = 200;  // another process

fs::path fixture(const std::string& name) {
  return fs::path(TACHYON_FIXTURE_DIR) / "perf" / "sched" / name;
}

std::string readText(const fs::path& p) {
  std::ifstream in(p);
  EXPECT_TRUE(in.good()) << p;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A tracepoint payload in the wide-state sched_switch layout (fixture sched_switch_wide_state.format).
std::vector<uint8_t> switchRaw(uint32_t prevPid, int64_t prevState, uint32_t nextPid,
                               int32_t nextPrio = 120) {
  std::vector<uint8_t> b(64, 0);
  const uint16_t type = 316;
  std::memcpy(b.data(), &type, 2);
  std::memcpy(b.data() + 24, &prevPid, 4);
  const int32_t prio = 120;
  std::memcpy(b.data() + 28, &prio, 4);
  std::memcpy(b.data() + 32, &prevState, 8);
  std::memcpy(b.data() + 56, &nextPid, 4);
  std::memcpy(b.data() + 60, &nextPrio, 4);
  return b;
}

std::vector<uint8_t> wakeRaw(uint32_t pid, int32_t cpu) {
  std::vector<uint8_t> b(36, 0);
  const uint16_t type = 317;
  std::memcpy(b.data(), &type, 2);
  std::memcpy(b.data() + 24, &pid, 4);
  const int32_t prio = 120;
  std::memcpy(b.data() + 28, &prio, 4);
  std::memcpy(b.data() + 32, &cpu, 4);
  return b;
}

class SchedSpool {
 public:
  explicit SchedSpool(const std::string& mode) {
    m_info.startTs = kStart;
    m_info.endTs = kEnd;
    m_info.samplingHz = 8000;
    m_info.mode = "launch";
    m_info.plan = "system-wide";
    m_info.cpuCount = 4;
    m_info.finished = true;
    m_info.maxSampleRate = 100000;
    if (mode == "tracepoint") {
      m_info.layout = RecordLayout{kSchedSampleType, true};
      m_info.sched.mode = "tracepoint";
      m_info.sched.switches = m_info.sched.wakeups = true;
      m_info.sched.switchFormat = readText(fixture("sched_switch_wide_state.format"));
      m_info.sched.wakeFormat = readText(fixture("sched_waking.format"));
      m_info.sched.wakeEvent = "sched_waking";
    } else {
      m_info.layout = RecordLayout{kBackendSampleType, true};
      m_info.sched.mode = mode;
      m_info.sched.switches = true;
    }
  }

  RecordBuilder& meta(uint32_t ring = 0) { return builder(m_meta, ring); }
  RecordBuilder& data(uint32_t ring = 0) { return builder(m_data, ring); }
  SessionInfo& info() { return m_info; }
  fs::path out() const { return m_dir.path() / "out.tachyon"; }
  const fs::path& dir() const { return m_dir.path(); }

  // The target process 100 with the threads 100 and 101, and an unrelated process 200 with thread 200.
  void processes() {
    meta().fork(kHolder, 1, kHolder, 1, kStart + 10, id(1, 1, kStart + 10));
    meta().comm(kHolder, kHolder, "prog", true, id(kHolder, kHolder, kStart + 20));
    meta().fork(kHolder, kHolder, kWaiter, kHolder, kStart + 30, id(kHolder, kHolder, kStart + 30));
    meta().comm(kHolder, kWaiter, "waiter", false, id(kHolder, kWaiter, kStart + 40));
    meta().fork(kOther, 1, kOther, 1, kStart + 10, id(1, 1, kStart + 10));
    meta().comm(kOther, kOther, "other", true, id(kOther, kOther, kStart + 20));
  }

  static Id id(uint32_t pid, uint32_t tid, uint64_t t, uint32_t cpu = 0) {
    return Id{pid, tid, t, cpu, 0, 0};
  }

  // A sched_switch tracepoint sample taken by (pid, tid) on `cpu`.
  void switchSample(uint32_t pid, uint32_t tid, uint64_t t, uint32_t cpu, uint32_t prev, int64_t prevState,
                    uint32_t next, std::vector<uint64_t> chain = {}, uint32_t ring = 0) {
    if (chain.empty()) chain = {kContextKernel, kKernelA, kContextUser, kUserWait, kUserMain};
    RecordBuilder::Sample s;
    s.misc = kMiscKernel;
    s.id = id(pid, tid, t, cpu);
    s.chain = std::move(chain);
    s.ip = s.chain[1];
    s.raw = switchRaw(prev, prevState, next);
    data(ring).sample(s);
  }
  void wakeSample(uint32_t pid, uint32_t tid, uint64_t t, uint32_t cpu, uint32_t woken, int32_t targetCpu,
                  std::vector<uint64_t> chain = {}, uint32_t ring = 0) {
    if (chain.empty()) chain = {kContextKernel, kKernelB, kContextUser, kUserUnlock, kUserMain};
    RecordBuilder::Sample s;
    s.misc = kMiscKernel;
    s.id = id(pid, tid, t, cpu);
    s.chain = std::move(chain);
    s.ip = s.chain[1];
    s.raw = wakeRaw(woken, targetCpu);
    data(ring).sample(s);
  }

  SpoolSet build() {
    {
      auto w = SpoolWriter::create(spoolMetaPath(m_dir.path()), SpoolKind::Meta, m_info.layout);
      for (auto& [ring, b] : m_meta) EXPECT_TRUE(w->append(ring, b.bytes()).ok());
      EXPECT_TRUE(w->close().ok());
    }
    {
      auto w = SpoolWriter::create(spoolDataPath(m_dir.path(), 0), SpoolKind::Data, m_info.layout);
      for (auto& [ring, b] : m_data) EXPECT_TRUE(w->append(ring, b.bytes()).ok());
      EXPECT_TRUE(w->close().ok());
    }
    EXPECT_TRUE(writeSessionInfo(m_dir.path(), m_info).ok());
    auto set = SpoolSet::openDirectory(m_dir.path());
    EXPECT_TRUE(set.ok());
    return *set;
  }

 private:
  RecordBuilder& builder(std::map<uint32_t, RecordBuilder>& m, uint32_t ring) {
    return m.try_emplace(ring, RecordBuilder(m_info.layout)).first->second;
  }
  TempDir m_dir;
  SessionInfo m_info;
  std::map<uint32_t, RecordBuilder> m_meta, m_data;
};

ConvertOptions target(uint32_t pid) {
  ConvertOptions o;
  o.rootPids = {pid};
  o.procRoot = "/nonexistent-proc";
  return o;
}

ConvertOptions systemWide() {
  ConvertOptions o;
  o.procRoot = "/nonexistent-proc";
  return o;
}

// Frames of a callstack of the loaded trace, leaf first, as absolute addresses.
std::vector<uint64_t> stackAddresses(model::TraceModel& m, CallstackIdx leaf) {
  std::vector<uint64_t> out;
  uint32_t node = leaf.get();
  while (node != 0 && node < m.nodes.size()) {
    out.push_back(m.frames[m.nodes[node].frame.get()].addr_or_rva);
    node = m.nodes[node].parent.get();
  }
  return out;
}

const std::vector<model::ThreadStateInterval>& statesOf(const model::TraceModel& m, uint32_t tid) {
  static const std::vector<model::ThreadStateInterval> kNone;
  auto it = m.tidToThread.find(tid);
  if (it == m.tidToThread.end()) return kNone;
  return m.statesByThread[it->second.get()];
}

struct Built {
  model::TraceModel model;
};

Built loadModel(const fs::path& path) {
  auto m = model::TraceModel::load(path);
  EXPECT_TRUE(m.ok()) << (m.ok() ? "" : m.error().toString());
  Built b{std::move(*m)};
  EXPECT_TRUE(analysis::buildThreadStates(b.model).ok());
  return b;
}

// The lock-like scenario of the privileged test, synthetic: the waiter blocks on cpu 1, the holder wakes it
// from cpu 0, the waiter is switched in on cpu 1.
void lockStream(SchedSpool& f) {
  f.processes();
  f.switchSample(kHolder, kWaiter, kStart + 1000, 1, kWaiter, 1, 0);  // waiter sleeps (S), cpu goes idle
  f.wakeSample(kHolder, kHolder, kStart + 2000, 0, kWaiter, 1);       // holder wakes the waiter
  f.switchSample(0, 0, kStart + 2100, 1, 0, 0, kWaiter, {kContextKernel, kKernelA});  // idle -> waiter
}

TEST(SchedConvert, TracepointStreamGivesWaitingBlockingStackAndWokenByHolder) {
  SchedSpool f("tracepoint");
  lockStream(f);
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok()) << stats.error().toString();
  EXPECT_EQ(stats->schedSwitchEvents, 2u);
  EXPECT_EQ(stats->schedWakeEvents, 1u);
  EXPECT_EQ(stats->switchesWritten, 2u);
  EXPECT_EQ(stats->readiesWritten, 1u);
  EXPECT_EQ(stats->samplesWritten, 0u) << "tracepoint samples are not CPU samples";

  Built b = loadModel(f.out());
  const auto& st = statesOf(b.model, kWaiter);
  // Unknown until the first switch, Waiting [1000, 2000), Ready [2000, 2100), Running from 2100.
  ASSERT_EQ(st.size(), 4u);
  EXPECT_EQ(st[0].state, ThreadState::Unknown);
  EXPECT_EQ(st[0].end, kStart + 1000);
  EXPECT_EQ(st[1].state, ThreadState::Waiting);
  EXPECT_EQ(st[1].start, kStart + 1000);
  EXPECT_EQ(st[1].end, kStart + 2000);
  EXPECT_EQ(st[1].wait_reason, 1) << "prev_state S";
  EXPECT_EQ(st[1].woke_by_tid, kHolder);
  EXPECT_EQ(st[2].state, ThreadState::Ready);
  EXPECT_EQ(st[2].start, kStart + 2000);
  EXPECT_EQ(st[2].end, kStart + 2100);
  EXPECT_EQ(st[2].woke_by_tid, kHolder);
  EXPECT_EQ(st[2].ready_ts, kStart + 2000);
  EXPECT_EQ(st[3].state, ThreadState::Running);
  EXPECT_EQ(st[3].start, kStart + 2100);
  EXPECT_EQ(st[3].cpu, 1u);

  // Blocking stack: the waiter's own callchain (kernel part, then user part), leaf first, markers stripped.
  EXPECT_EQ(stackAddresses(b.model, st[1].blocking_stack),
            (std::vector<uint64_t>{kKernelA, kUserWait, kUserMain}));
  EXPECT_EQ(stackAddresses(b.model, st[2].blocking_stack),
            (std::vector<uint64_t>{kKernelA, kUserWait, kUserMain}));
  // Unblocking stack: the holder's callchain at sched_waking.
  ASSERT_TRUE(st[1].unblocking_stack.valid());
  EXPECT_EQ(stackAddresses(b.model, st[1].unblocking_stack),
            (std::vector<uint64_t>{kKernelB, kUserUnlock, kUserMain}));
  EXPECT_EQ(st[1].unblocking_stack, st[2].unblocking_stack);

  // The records themselves.
  auto reader = trace::TraceReader::open(f.out());
  ASSERT_TRUE(reader.ok());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  ASSERT_EQ(sw->size(), 2u);
  EXPECT_EQ((*sw)[0].ts, kStart + 1000);
  EXPECT_EQ((*sw)[0].cpu, 1u);
  EXPECT_EQ((*sw)[0].old_tid, kWaiter);
  EXPECT_EQ((*sw)[0].new_tid, 0u);
  EXPECT_EQ((*sw)[0].old_state, 5);
  EXPECT_EQ((*sw)[0].old_wait_reason, 1);
  EXPECT_EQ((*sw)[0].new_priority, 120);
  EXPECT_EQ((*sw)[0].reserved, 0u);
  EXPECT_EQ((*sw)[1].old_tid, 0u);
  EXPECT_EQ((*sw)[1].new_tid, kWaiter);
  EXPECT_EQ((*sw)[1].old_thread_stack.get(), 0u) << "the idle task has no stack of interest";
  auto rd = reader->readReadyEvents();
  ASSERT_TRUE(rd.ok());
  ASSERT_EQ(rd->size(), 1u);
  EXPECT_EQ((*rd)[0].ts, kStart + 2000);
  EXPECT_EQ((*rd)[0].cpu, 0u);
  EXPECT_EQ((*rd)[0].readied_tid, kWaiter);
  EXPECT_EQ((*rd)[0].readying_tid, kHolder);
  EXPECT_EQ((*rd)[0].reserved, 0u);

  // CaptureInfo says which path was used.
  auto info = reader->readCaptureInfo();
  ASSERT_TRUE(info.ok());
  const auto doc = nlohmann::json::parse(*info);
  EXPECT_EQ(doc["sched"]["path"], "tracepoint");
  EXPECT_EQ(doc["sched"]["context_switches_written"], 2u);
  EXPECT_EQ(doc["sched"]["ready_events_written"], 1u);
  EXPECT_TRUE(doc["sched"]["wait_reasons"].get<bool>());
  EXPECT_TRUE(doc["sched"]["wakers"].get<bool>());
}

TEST(SchedConvert, PreemptedThreadsAreReadyAndExitingThreadsEndTheirLife) {
  SchedSpool f("tracepoint");
  f.processes();
  // Holder is preempted by a thread of the other process (TASK_REPORT_MAX), comes back later.
  f.switchSample(kHolder, kHolder, kStart + 3000, 0, kHolder, 0x100, kOther);
  f.switchSample(kOther, kOther, kStart + 3500, 0, kOther, 0, kHolder);
  // The waiter exits (EXIT_ZOMBIE).
  f.switchSample(kHolder, kWaiter, kStart + 4000, 1, kWaiter, 0x20, 0);
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->switchesWritten, 3u)
      << "the other process's switch-out is kept because it switches the target in";

  auto reader = trace::TraceReader::open(f.out());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  ASSERT_EQ(sw->size(), 3u);
  EXPECT_EQ((*sw)[0].old_state, 1) << "preempted: Ready";
  EXPECT_EQ((*sw)[0].old_wait_reason, 0);
  EXPECT_EQ((*sw)[1].old_tid, kOther);
  EXPECT_EQ((*sw)[1].old_thread_stack.get(), 0u) << "a non-target thread's stack is not resolved";
  EXPECT_EQ((*sw)[2].old_state, 4) << "exit";
  EXPECT_EQ((*sw)[2].old_wait_reason, 0x20);

  Built b = loadModel(f.out());
  const auto& holder = statesOf(b.model, kHolder);
  bool sawReady = false;
  for (const auto& iv : holder)
    if (iv.state == ThreadState::Ready && iv.start == kStart + 3000) {
      sawReady = true;
      EXPECT_EQ(iv.end, kStart + 3500);
      EXPECT_FALSE(iv.unblocking_stack.valid()) << "a preempted thread has no waker";
      EXPECT_TRUE(iv.blocking_stack.valid());
    }
  EXPECT_TRUE(sawReady);
  // The exited waiter has no state after its last switch-out.
  const auto& waiter = statesOf(b.model, kWaiter);
  ASSERT_FALSE(waiter.empty());
  EXPECT_LE(waiter.back().end, kStart + 4000 + 1);
}

TEST(SchedConvert, OnlyEventsOfTargetThreadsAreRecordedInLaunchMode) {
  SchedSpool f("tracepoint");
  f.processes();
  f.meta().fork(kOther, kOther, 201, kOther, kStart + 50, SchedSpool::id(kOther, kOther, kStart + 50));
  // Both sides foreign: dropped.
  f.switchSample(kOther, kOther, kStart + 1000, 2, kOther, 1, 201);
  f.switchSample(kOther, 201, kStart + 1100, 2, 201, 0x100, kOther);
  // A foreign thread wakes a foreign thread: dropped. A foreign thread wakes the target waiter: kept.
  f.wakeSample(kOther, kOther, kStart + 1200, 2, 201, 2);
  f.wakeSample(kOther, kOther, kStart + 1300, 2, kWaiter, 1);
  // Idle (pid 0) switching to the target is kept; idle to a foreign thread is not.
  f.switchSample(0, 0, kStart + 1400, 1, 0, 0, kWaiter, {kContextKernel, kKernelA});
  f.switchSample(0, 0, kStart + 1500, 2, 0, 0, kOther, {kContextKernel, kKernelA});
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->schedSwitchEvents, 4u);
  EXPECT_EQ(stats->switchesWritten, 1u);
  EXPECT_EQ(stats->schedWakeEvents, 2u);
  EXPECT_EQ(stats->readiesWritten, 1u);
  EXPECT_EQ(stats->schedDroppedNotTarget, 4u);

  auto reader = trace::TraceReader::open(f.out());
  auto rd = reader->readReadyEvents();
  ASSERT_TRUE(rd.ok());
  ASSERT_EQ(rd->size(), 1u);
  EXPECT_EQ((*rd)[0].readying_tid, kOther);
  // A foreign waker contributes only its kernel frames: no modules of the other process are created.
  Built b = loadModel(f.out());
  EXPECT_EQ(stackAddresses(b.model, (*rd)[0].readying_stack), (std::vector<uint64_t>{kKernelB}));
  EXPECT_TRUE(b.model.modules.empty());
}

TEST(SchedConvert, SystemModeKeepsEverythingAndWritesTheThreads) {
  SchedSpool f("tracepoint");
  f.info().mode = "system";
  f.processes();
  f.switchSample(kOther, kOther, kStart + 1000, 2, kOther, 1, 0);
  f.switchSample(0, 0, kStart + 1100, 2, 0, 0, kHolder, {kContextKernel, kKernelA});
  f.wakeSample(kHolder, kHolder, kStart + 1200, 0, kOther, 2);
  auto stats = convertSpool(f.build(), f.out(), systemWide());
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->switchesWritten, 2u);
  EXPECT_EQ(stats->readiesWritten, 1u);
  EXPECT_EQ(stats->schedDroppedNotTarget, 0u);
  Built b = loadModel(f.out());
  EXPECT_EQ(b.model.processes.size(), 2u);
  ASSERT_EQ(b.model.tidToThread.count(kOther), 1u)
      << "a thread with scheduler events only still gets a record";
  const auto& st = statesOf(b.model, kOther);
  bool waiting = false;
  for (const auto& iv : st) waiting |= iv.state == ThreadState::Waiting || iv.state == ThreadState::Ready;
  EXPECT_TRUE(waiting);
}

TEST(SchedConvert, PlainSamplesStillConvertInATracepointSpool) {
  SchedSpool f("tracepoint");
  f.processes();
  RecordBuilder::Sample s;
  s.misc = kMiscUser;
  s.id = SchedSpool::id(kHolder, kHolder, kStart + 500, 0);
  s.chain = {kContextUser, kUserMain};
  s.ip = kUserMain;  // raw empty: the 4-byte area of a software event
  f.data().sample(s);
  f.switchSample(kHolder, kWaiter, kStart + 1000, 1, kWaiter, 1, 0);
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->samplesWritten, 1u);
  EXPECT_EQ(stats->switchesWritten, 1u);
  EXPECT_EQ(stats->recordsMalformed, 0u);
}

TEST(SchedConvert, RecordsAreSortedAcrossRings) {
  SchedSpool f("tracepoint");
  f.processes();
  // cpu 1 (ring 1) holds the earlier switch-out; cpu 0 (ring 0) the later wake: merged by time.
  f.wakeSample(kHolder, kHolder, kStart + 2000, 0, kWaiter, 1, {}, 0);
  f.switchSample(kHolder, kWaiter, kStart + 1000, 1, kWaiter, 1, 0, {}, 1);
  f.switchSample(0, 0, kStart + 2100, 1, 0, 0, kWaiter, {kContextKernel, kKernelA}, 1);
  ASSERT_TRUE(convertSpool(f.build(), f.out(), target(kHolder)).ok());
  Built b = loadModel(f.out());
  const auto& st = statesOf(b.model, kWaiter);
  ASSERT_EQ(st.size(), 4u);
  EXPECT_EQ(st[1].state, ThreadState::Waiting);
  EXPECT_EQ(st[2].state, ThreadState::Ready);
}

TEST(SchedConvert, SwitchWithMismatchedSampleTaskSetsTheOtherThreadFlag) {
  SchedSpool f("tracepoint");
  f.processes();
  // The callchain is attributed to the holder although the waiter is the one switching out.
  f.switchSample(kHolder, kHolder, kStart + 1000, 1, kWaiter, 1, 0);
  ASSERT_TRUE(convertSpool(f.build(), f.out(), target(kHolder)).ok());
  auto reader = trace::TraceReader::open(f.out());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  ASSERT_EQ(sw->size(), 1u);
  EXPECT_EQ((*sw)[0].reserved & trace::kSwitchFlagStackFromOtherThread,
            trace::kSwitchFlagStackFromOtherThread);
}

TEST(SchedConvert, AnEventMergedAheadOfItsThreadsForkDoesNotSplitTheThreadRecord) {
  SchedSpool f("tracepoint");
  f.meta().fork(kHolder, 1, kHolder, 1, kStart + 10, SchedSpool::id(1, 1, kStart + 10));
  f.meta().comm(kHolder, kHolder, "prog", true, SchedSpool::id(kHolder, kHolder, kStart + 20));
  // The waiter's first switch-out is merged before the parent's FORK record for it (a few ns apart).
  f.switchSample(kHolder, kWaiter, kStart + 1000, 1, kWaiter, 1, 0);
  f.meta().fork(kHolder, kHolder, kWaiter, kHolder, kStart + 1005,
                SchedSpool::id(kHolder, kHolder, kStart + 1005));
  f.switchSample(0, 0, kStart + 2100, 1, 0, 0, kWaiter, {kContextKernel, kKernelA});
  ASSERT_TRUE(convertSpool(f.build(), f.out(), target(kHolder)).ok());
  Built b = loadModel(f.out());
  size_t records = 0;
  for (const auto& t : b.model.threads) records += t.tid == kWaiter ? 1 : 0;
  EXPECT_EQ(records, 1u) << "one thread record per live thread";
  bool waiting = false;
  for (const auto& iv : statesOf(b.model, kWaiter)) waiting |= iv.state == ThreadState::Waiting;
  EXPECT_TRUE(waiting);
}

// The kernel's cpu-wide records for one switch A -> B on a cpu: a switch-out record made by A (tid A, next B)
void cpuWideSwitch(SchedSpool& f, uint32_t ring, uint64_t t, uint32_t cpu, uint32_t fromPid, uint32_t fromTid,
                   uint32_t toPid, uint32_t toTid, bool preempt = false, bool withOutRecord = true) {
  if (withOutRecord)
    f.data(ring).switchCpuWide(true, toPid, toTid, SchedSpool::id(fromPid, fromTid, t, cpu), preempt);
  f.data(ring).switchCpuWide(false, fromPid, fromTid, SchedSpool::id(toPid, toTid, t + 5, cpu));
}

// One ThreadRec per tid, and no thread for the kernel's "unhashed task" tid.
void expectOneRecordPerTid(const model::TraceModel& m) {
  std::map<uint32_t, int> count;
  for (const auto& t : m.threads) ++count[t.tid];
  for (const auto& [tid, n] : count) EXPECT_EQ(n, 1) << "tid " << tid << " has " << n << " thread records";
  EXPECT_EQ(count.count(0xFFFFFFFFu), 0u) << "no thread for the unnamed tid";
}

TEST(SchedConvert, EventsAfterAnExitRecordStayWithTheExitedThread) {
  SchedSpool f("tracepoint");
  f.processes();
  f.switchSample(kHolder, kWaiter, kStart + 1000, 1, kWaiter, 1, 0);
  f.wakeSample(kHolder, kHolder, kStart + 2000, 0, kWaiter, 1);
  f.switchSample(0, 0, kStart + 2100, 1, 0, 0, kWaiter, {kContextKernel, kKernelA});
  f.meta().exit(kHolder, kHolder, kWaiter, kHolder, kStart + 3000,
                SchedSpool::id(kHolder, kWaiter, kStart + 3000));
  // After the EXIT record: a wakeup by the dying task, and its final switch-out, which the kernel reports
  // with the tid -1 of a task that is no longer pid_alive (prev_pid in the payload still names it).
  f.wakeSample(kHolder, kWaiter, kStart + 3010, 1, kHolder, 0);
  f.switchSample(kHolder, 0xFFFFFFFFu, kStart + 3020, 1, kWaiter, 0x10, 0);
  ASSERT_TRUE(convertSpool(f.build(), f.out(), target(kHolder)).ok());
  Built b = loadModel(f.out());
  expectOneRecordPerTid(b.model);
  auto reader = trace::TraceReader::open(f.out());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  bool sawExit = false;
  for (const auto& c : *sw) {
    EXPECT_NE(c.old_tid, 0xFFFFFFFFu);
    EXPECT_NE(c.new_tid, 0xFFFFFFFFu);
    sawExit |= c.old_tid == kWaiter && c.old_state == 4;
  }
  EXPECT_TRUE(sawExit) << "the final switch-out is attributed to prev_pid";
  auto rd = reader->readReadyEvents();
  ASSERT_TRUE(rd.ok());
  for (const auto& r : *rd) {
    EXPECT_NE(r.readying_tid, 0xFFFFFFFFu);
    EXPECT_NE(r.readied_tid, 0xFFFFFFFFu);
  }
  const auto& st = statesOf(b.model, kWaiter);
  ASSERT_FALSE(st.empty());
  bool waiting = false;
  for (const auto& iv : st) waiting |= iv.state == ThreadState::Waiting;
  EXPECT_TRUE(waiting) << "the thread's states are not lost to a second record";
}

TEST(SchedConvert, ACpuClockSampleAfterTheExitRecordDoesNotCreateAThread) {
  SchedSpool f("tracepoint");
  f.processes();
  f.meta().exit(kHolder, kHolder, kWaiter, kHolder, kStart + 3000,
                SchedSpool::id(kHolder, kWaiter, kStart + 3000));
  RecordBuilder::Sample s;
  s.misc = kMiscUser;
  s.id = SchedSpool::id(kHolder, kWaiter, kStart + 3005, 1);
  s.chain = {kContextUser, kUserMain};
  s.ip = kUserMain;
  f.data().sample(s);
  s.id = SchedSpool::id(kHolder, 0xFFFFFFFFu, kStart + 3006, 1);  // an unnamed task: dropped
  f.data().sample(s);
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->samplesWritten, 1u);
  Built b = loadModel(f.out());
  expectOneRecordPerTid(b.model);
}

TEST(SchedConvert, ADuplicateForkFromAnotherRingDoesNotCreateAThread) {
  SchedSpool f("tracepoint");
  f.meta().fork(kHolder, 1, kHolder, 1, kStart + 10, SchedSpool::id(1, 1, kStart + 10));
  f.meta().fork(kHolder, kHolder, kWaiter, kHolder, kStart + 1000,
                SchedSpool::id(kHolder, kHolder, kStart + 1000));
  f.meta(1).fork(kHolder, kHolder, kWaiter, kHolder, kStart + 1001,
                 SchedSpool::id(kHolder, kHolder, kStart + 1001));
  f.meta(1).fork(kHolder, 1, kHolder, 1, kStart + 11, SchedSpool::id(1, 1, kStart + 11));  // the process too
  f.switchSample(kHolder, kWaiter, kStart + 2000, 1, kWaiter, 1, 0);
  ASSERT_TRUE(convertSpool(f.build(), f.out(), target(kHolder)).ok());
  Built b = loadModel(f.out());
  expectOneRecordPerTid(b.model);
  EXPECT_EQ(b.model.processes.size(), 1u);
}

TEST(SchedConvert, CpuWideSwitchOutOfAnUnnamedTaskIsAttributedToTheLastRunningThread) {
  SchedSpool f("context_switch");
  f.processes();
  cpuWideSwitch(f, 1, kStart + 1000, 1, 0, 0, kHolder, kWaiter, false, false);  // the waiter gets cpu 1
  // The waiter exits: its switch-out record has the tid -1.
  f.data(1).switchCpuWide(true, 0, 0, SchedSpool::id(kHolder, 0xFFFFFFFFu, kStart + 2000, 1));
  f.data(1).switchCpuWide(false, kHolder, kWaiter, SchedSpool::id(0, 0, kStart + 2005, 1));
  ASSERT_TRUE(convertSpool(f.build(), f.out(), target(kHolder)).ok());
  auto reader = trace::TraceReader::open(f.out());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  ASSERT_EQ(sw->size(), 2u);
  EXPECT_EQ((*sw)[1].old_tid, kWaiter);
  for (const auto& c : *sw) {
    EXPECT_NE(c.old_tid, 0xFFFFFFFFu);
    EXPECT_NE(c.new_tid, 0xFFFFFFFFu);
  }
  Built b = loadModel(f.out());
  expectOneRecordPerTid(b.model);
}

TEST(SchedConvert, WindowDropsOlderSchedulerEvents) {
  SchedSpool f("tracepoint");
  lockStream(f);
  ConvertOptions o = target(kHolder);
  o.windowStartTs = kStart + 2050;
  auto stats = convertSpool(f.build(), f.out(), o);
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->switchesWritten, 1u);
  EXPECT_EQ(stats->readiesWritten, 0u);
  EXPECT_EQ(stats->schedDroppedWindow, 2u);
}

TEST(SchedConvert, UnusableFormatInTheSpoolIsReportedAndSkipsThoseEvents) {
  SchedSpool f("tracepoint");
  f.info().sched.switchFormat = "not a format";
  lockStream(f);
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->switchesWritten, 0u);
  EXPECT_EQ(stats->readiesWritten, 1u);
  bool warned = false;
  for (const std::string& w : stats->warnings) warned |= w.find("sched_switch format") != std::string::npos;
  EXPECT_TRUE(warned);
  EXPECT_GE(stats->recordsUnsupported, 2u);
}

// ---------------------------------------------------------------------------------------------------------
// the PERF_RECORD_SWITCH fallback

TEST(SchedConvert, FallbackCpuWideSwitchesUsePreemptFlagForReadyAndWaiting) {
  SchedSpool f("context_switch");
  f.processes();
  // cpu 1: the waiter blocks (not runnable) and the idle task runs; the idle task is switched out again at
  // 2100 (no switch-out record for it: only the waiter's switch-in record tells). cpu 0: the holder is
  // preempted by `other` and gets the cpu back.
  cpuWideSwitch(f, 1, kStart + 1000, 1, kHolder, kWaiter, 0, 0);
  cpuWideSwitch(f, 1, kStart + 2100, 1, 0, 0, kHolder, kWaiter, false, /*withOutRecord=*/false);
  cpuWideSwitch(f, 0, kStart + 3000, 0, kHolder, kHolder, kOther, kOther, true);
  cpuWideSwitch(f, 0, kStart + 3500, 0, kOther, kOther, kHolder, kHolder);
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok()) << stats.error().toString();
  EXPECT_EQ(stats->switchesWritten, 4u) << "one record per switch";
  EXPECT_EQ(stats->readiesWritten, 0u);

  auto reader = trace::TraceReader::open(f.out());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  ASSERT_EQ(sw->size(), 4u);
  EXPECT_EQ((*sw)[0].old_tid, kWaiter);
  EXPECT_EQ((*sw)[0].new_tid, 0u);
  EXPECT_EQ((*sw)[0].old_state, 5) << "not preempted: waiting";
  EXPECT_EQ((*sw)[0].old_wait_reason, 0) << "reason unknown without the tracepoint";
  EXPECT_EQ((*sw)[0].old_thread_stack.get(), 0u);
  EXPECT_EQ((*sw)[1].old_tid, 0u);
  EXPECT_EQ((*sw)[1].new_tid, kWaiter);
  EXPECT_EQ((*sw)[1].ts, kStart + 2105) << "taken from the switch-in record";
  EXPECT_EQ((*sw)[2].old_tid, kHolder);
  EXPECT_EQ((*sw)[2].new_tid, kOther);
  EXPECT_EQ((*sw)[2].old_state, 1) << "PREEMPT flag: Ready";
  EXPECT_EQ((*sw)[3].old_tid, kOther);
  EXPECT_EQ((*sw)[3].new_tid, kHolder);

  Built b = loadModel(f.out());
  const auto& waiter = statesOf(b.model, kWaiter);
  ASSERT_GE(waiter.size(), 3u);
  EXPECT_EQ(waiter[1].state, ThreadState::Waiting);
  EXPECT_EQ(waiter[1].start, kStart + 1000);
  EXPECT_EQ(waiter[1].end, kStart + 2105);
  EXPECT_EQ(waiter[1].woke_by_tid, 0u) << "no wakeup information in the fallback";
  EXPECT_FALSE(waiter[1].blocking_stack.valid() && waiter[1].blocking_stack.get() != 0);
  EXPECT_EQ(waiter[2].state, ThreadState::Running);
  bool holderReady = false;
  for (const auto& iv : statesOf(b.model, kHolder))
    if (iv.state == ThreadState::Ready && iv.start == kStart + 3000) {
      holderReady = true;
      EXPECT_EQ(iv.end, kStart + 3500);
    }
  EXPECT_TRUE(holderReady);

  auto info = reader->readCaptureInfo();
  const auto doc = nlohmann::json::parse(*info);
  EXPECT_EQ(doc["sched"]["path"], "context_switch");
  EXPECT_FALSE(doc["sched"]["wait_reasons"].get<bool>());
  EXPECT_FALSE(doc["sched"]["wakers"].get<bool>());
}

TEST(SchedConvert, CpuWideSwitchInRecordsRepeatingASwitchOutAreNotCountedTwice) {
  // The same stream, but this kernel does emit a switch-out record for the idle task as well.
  SchedSpool f("context_switch");
  f.processes();
  cpuWideSwitch(f, 1, kStart + 1000, 1, kHolder, kWaiter, 0, 0);
  cpuWideSwitch(f, 1, kStart + 2100, 1, 0, 0, kHolder, kWaiter, false, /*withOutRecord=*/true);
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->switchesWritten, 2u);
  Built b = loadModel(f.out());
  const auto& waiter = statesOf(b.model, kWaiter);
  ASSERT_GE(waiter.size(), 3u);
  EXPECT_EQ(waiter[1].state, ThreadState::Waiting);
  EXPECT_EQ(waiter[1].end, kStart + 2100)
      << "the switch-out record's time; no Unknown gap from a second switch-in";
  EXPECT_EQ(waiter[2].state, ThreadState::Running);
}

TEST(SchedConvert, FallbackPerTaskSwitchRecordsGiveOneSidedSwitches) {
  SchedSpool f("context_switch");
  f.processes();
  // Per-task events (the unprivileged plan): PERF_RECORD_SWITCH out / in for the waiter.
  f.data(0).switchTask(true, SchedSpool::id(kHolder, kWaiter, kStart + 1000, 1));
  f.data(0).switchTask(false, SchedSpool::id(kHolder, kWaiter, kStart + 2000, 1));
  f.data(0).switchTask(true, SchedSpool::id(kHolder, kWaiter, kStart + 2500, 1), true);
  f.data(0).switchTask(false, SchedSpool::id(kHolder, kWaiter, kStart + 2600, 2));
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->switchesWritten, 4u);
  Built b = loadModel(f.out());
  const auto& st = statesOf(b.model, kWaiter);
  ASSERT_EQ(st.size(), 5u);
  EXPECT_EQ(st[1].state, ThreadState::Waiting);
  EXPECT_EQ(st[1].end, kStart + 2000);
  EXPECT_EQ(st[2].state, ThreadState::Running);
  EXPECT_EQ(st[2].end, kStart + 2500);
  EXPECT_EQ(st[3].state, ThreadState::Ready) << "preempted";
  EXPECT_EQ(st[3].end, kStart + 2600);
  EXPECT_EQ(st[4].state, ThreadState::Running);
  EXPECT_EQ(st[4].cpu, 2u);
}

TEST(SchedConvert, SwitchRecordsAreIgnoredWhenTheSessionDidNotAskForThem) {
  SchedSpool f("none");
  f.processes();
  f.data(0).switchCpuWide(true, 0, 0, SchedSpool::id(kHolder, kWaiter, kStart + 1000, 1));
  auto stats = convertSpool(f.build(), f.out(), target(kHolder));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->switchesWritten, 0u);
  auto reader = trace::TraceReader::open(f.out());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  EXPECT_TRUE(sw->empty());
}

}  // namespace
}  // namespace tachyon::capture::perf
