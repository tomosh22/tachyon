// WP-65a: the perf backend's live source. LiveDecoder turns synthetic perf record streams into ILiveSink
// calls (processes, threads, modules, samples with raw callchains, scheduler events); LiveSource adds the
// bounded queue and the thread.
#include "capture/linux_perf/live_source.h"
#include "linux_perf_test_util.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;
using ptest::ElfSegment;
using ptest::ElfSpec;
using ptest::TempDir;
using Id = RecordBuilder::Id;

constexpr uint64_t kStart = 1'000'000;
constexpr uint64_t kKernelText = 0xffffffff81000000ull;
constexpr uint32_t kProg = 100;

// Records everything the decoder tells the sink, in order.
class RecordingSink final : public ILiveSink {
 public:
  struct Sample {
    uint64_t ts;
    uint32_t pid, tid, cpu;
    uint16_t flags;
    std::vector<uint64_t> ips;
  };
  struct Switch {
    LiveContextSwitch cs;
    std::vector<uint64_t> stack;
  };
  struct Ready {
    LiveReady rd;
    std::vector<uint64_t> stack;
  };

  void onSessionInfo(const LiveSessionInfo& i) override { info = i; }
  void onProcess(const LiveProcess& p) override {
    std::lock_guard lock(m);
    processes.push_back(p);
    order.push_back("proc:" + std::to_string(p.pid));
  }
  void onThread(const LiveThread& t) override {
    std::lock_guard lock(m);
    threads.push_back(t);
    order.push_back("thread:" + std::to_string(t.tid));
  }
  void onModule(const LiveModule& mod) override {
    std::lock_guard lock(m);
    modules.push_back(mod);
    order.push_back(std::string(mod.unloadTs != 0 ? "unload:" : "module:") + mod.path);
  }
  void onSample(uint64_t ts, uint32_t pid, uint32_t tid, uint32_t cpu, uint16_t flags,
                std::span<const uint64_t> ips) override {
    std::lock_guard lock(m);
    samples.push_back({ts, pid, tid, cpu, flags, std::vector<uint64_t>(ips.begin(), ips.end())});
    order.push_back("sample:" + std::to_string(ts));
  }
  void onContextSwitch(const LiveContextSwitch& c) override {
    std::lock_guard lock(m);
    switches.push_back({c, std::vector<uint64_t>(c.oldStack.begin(), c.oldStack.end())});
  }
  void onReady(const LiveReady& r) override {
    std::lock_guard lock(m);
    readies.push_back({r, std::vector<uint64_t>(r.readyingStack.begin(), r.readyingStack.end())});
  }
  void onTimeAdvance(uint64_t ts) override {
    std::lock_guard lock(m);
    timeAdvances.push_back(ts);
  }
  void onDropped(uint64_t n) override {
    std::lock_guard lock(m);
    dropped += n;
  }

  size_t sampleCount() {
    std::lock_guard lock(m);
    return samples.size();
  }
  size_t indexOf(const std::string& what) {
    for (size_t i = 0; i < order.size(); ++i)
      if (order[i] == what) return i;
    return SIZE_MAX;
  }

  std::mutex m;
  LiveSessionInfo info;
  std::vector<LiveProcess> processes;
  std::vector<LiveThread> threads;
  std::vector<LiveModule> modules;
  std::vector<Sample> samples;
  std::vector<Switch> switches;
  std::vector<Ready> readies;
  std::vector<uint64_t> timeAdvances;
  uint64_t dropped = 0;
  std::vector<std::string> order;
};

Id id(uint32_t pid, uint32_t tid, uint64_t t, uint32_t cpu = 0) {
  return Id{pid, tid, t, cpu, 0, 0};
}

struct Fixture {
  TempDir dir;
  std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
  LiveDecoderConfig config;
  fs::path elf;

  Fixture() {
    // A PIE-like image: one executable PT_LOAD at vaddr 0, 0x2000 bytes; GNU build-id aa bb cc dd.
    elf = dir.path() / "prog.elf";
    ptest::writeElf(elf, ElfSpec{{0xAA, 0xBB, 0xCC, 0xDD}, {ElfSegment{0, 0, 0x2000, 0x2000, 5}}, 3});
    config.sink = sink;
    config.layout = RecordLayout{};
    config.startTs = kStart;
    config.procRoot = "/nonexistent-proc";
    config.fileMapper = [this](uint32_t, std::string_view) { return elf; };
  }
  std::unique_ptr<LiveDecoder> decoder() { return std::make_unique<LiveDecoder>(config); }

  // A program process with one thread and the ELF mapped at 0x400000.
  void program(RecordBuilder& b, uint32_t pid = kProg, uint64_t t = kStart + 10) {
    b.fork(pid, 1, pid, 1, t, id(1, 1, t));
    b.comm(pid, pid, "prog", true, id(pid, pid, t + 5));
    RecordBuilder::Mmap2 mm;
    mm.pid = pid;
    mm.tid = pid;
    mm.addr = 0x400000;
    mm.len = 0x2000;
    mm.pgoff = 0;
    mm.name = "/bin/prog";
    mm.inode = 77;
    mm.id = id(pid, pid, t + 6);
    b.mmap2(mm);
  }

  static void sample(RecordBuilder& b, uint32_t pid, uint32_t tid, uint64_t t, std::vector<uint64_t> chain,
                     uint16_t misc = kMiscUser, uint32_t cpu = 0) {
    RecordBuilder::Sample s;
    s.misc = misc;
    s.id = id(pid, tid, t, cpu);
    s.chain = std::move(chain);
    s.ip = s.chain.empty() ? 0 : (s.chain.size() > 1 && s.chain[0] >= kContextMax ? s.chain[1] : s.chain[0]);
    b.sample(s);
  }
};

std::span<const uint8_t> view(const RecordBuilder& b) {
  return b.bytes();
}

TEST(LiveDecoder, ReportsProcessThreadModuleAndRawSamples) {
  Fixture f;
  RecordBuilder b;
  f.program(b);
  Fixture::sample(b, kProg, kProg, kStart + 100, {kContextUser, 0x401234, 0x400100, 0x400020});
  auto dec = f.decoder();
  dec->decode(view(b));
  RecordingSink& s = *f.sink;

  ASSERT_EQ(s.processes.size(), 1u);
  EXPECT_EQ(s.processes[0].pid, kProg);
  EXPECT_EQ(s.processes[0].name, "prog");
  EXPECT_EQ(s.processes[0].ppid, 1u);
  ASSERT_EQ(s.threads.size(), 1u);
  EXPECT_EQ(s.threads[0].tid, kProg);
  EXPECT_EQ(s.threads[0].name, "prog");

  ASSERT_EQ(s.modules.size(), 1u);
  EXPECT_EQ(s.modules[0].path, "/bin/prog");
  EXPECT_EQ(s.modules[0].pid, kProg);
  EXPECT_EQ(s.modules[0].base, 0x400000u);  // the ELF rule: bias + lowest PT_LOAD
  EXPECT_EQ(s.modules[0].size, 0x2000u);
  EXPECT_EQ(s.modules[0].idKind, trace::kModuleIdGnuBuildId);
  ASSERT_EQ(s.modules[0].idLen, 4);
  EXPECT_EQ(s.modules[0].id[0], 0xAA);
  EXPECT_EQ(s.modules[0].unloadTs, 0u);

  ASSERT_EQ(s.samples.size(), 1u);
  EXPECT_EQ(s.samples[0].ts, kStart + 100);
  EXPECT_EQ(s.samples[0].tid, kProg);
  EXPECT_EQ(s.samples[0].ips, (std::vector<uint64_t>{0x401234, 0x400100, 0x400020}));  // markers stripped
  EXPECT_EQ(s.samples[0].flags, 0u);

  // Everything is announced before the first sample that needs it.
  EXPECT_LT(s.indexOf("proc:100"), s.indexOf("sample:" + std::to_string(kStart + 100)));
  EXPECT_LT(s.indexOf("module:/bin/prog"), s.indexOf("sample:" + std::to_string(kStart + 100)));
  EXPECT_LT(s.indexOf("thread:100"), s.indexOf("sample:" + std::to_string(kStart + 100)));
}

TEST(LiveDecoder, KernelFramesStayAbsoluteAndTheLeafFlagIsSet) {
  Fixture f;
  RecordBuilder b;
  f.program(b);
  Fixture::sample(b, kProg, kProg, kStart + 100,
                  {kContextKernel, kKernelText + 0x10, kKernelText + 0x20, kContextUser, 0x400100},
                  kMiscKernel);
  f.decoder()->decode(view(b));
  ASSERT_EQ(f.sink->samples.size(), 1u);
  EXPECT_EQ(f.sink->samples[0].ips,
            (std::vector<uint64_t>{kKernelText + 0x10, kKernelText + 0x20, 0x400100}));
  EXPECT_EQ(f.sink->samples[0].flags, trace::kSampleFlagKernelLeaf);
}

TEST(LiveDecoder, OnlyTheTargetIsReportedUnlessTheCaptureIsSystemWide) {
  auto run = [](bool system, bool follow) {
    Fixture f;
    if (!system) f.config.rootPids = {kProg};
    f.config.followChildren = follow;
    RecordBuilder b;
    f.program(b, kProg, kStart + 10);
    f.program(b, 200, kStart + 20);  // an unrelated program
    // A child of the target (fork + exec'd name).
    b.fork(300, kProg, 300, kProg, kStart + 30, id(kProg, kProg, kStart + 30));
    b.comm(300, 300, "child", true, id(300, 300, kStart + 31));
    Fixture::sample(b, kProg, kProg, kStart + 100, {0x400100});
    Fixture::sample(b, 200, 200, kStart + 101, {0x400100});
    Fixture::sample(b, 300, 300, kStart + 102, {0x400100});
    f.decoder()->decode(view(b));
    std::map<uint32_t, int> bySample;
    for (const auto& s : f.sink->samples) ++bySample[s.pid];
    return bySample;
  };
  const auto target = run(false, false);
  EXPECT_EQ(target.count(kProg), 1u);
  EXPECT_EQ(target.count(200), 0u);
  EXPECT_EQ(target.count(300), 0u);
  const auto followed = run(false, true);
  EXPECT_EQ(followed.count(300), 1u);
  EXPECT_EQ(followed.count(200), 0u);
  const auto system = run(true, false);
  EXPECT_EQ(system.size(), 3u);
}

TEST(LiveDecoder, ThreadsRenamesAndExitsAreReported) {
  Fixture f;
  f.config.rootPids = {kProg};
  RecordBuilder b;
  f.program(b);
  b.fork(kProg, kProg, 101, kProg, kStart + 40, id(kProg, kProg, kStart + 40));
  b.comm(kProg, 101, "worker-a", false, id(kProg, 101, kStart + 41));
  Fixture::sample(b, kProg, 101, kStart + 100, {0x400100});
  b.comm(kProg, 101, "worker-b", false, id(kProg, 101, kStart + 200));  // after it was announced: a rename
  b.exit(kProg, kProg, 101, kProg, kStart + 300, id(kProg, 101, kStart + 300));
  f.decoder()->decode(view(b));
  RecordingSink& s = *f.sink;
  std::vector<LiveThread> mine;
  for (const auto& t : s.threads)
    if (t.tid == 101) mine.push_back(t);
  ASSERT_GE(mine.size(), 3u);  // announced, renamed, ended
  EXPECT_EQ(mine.front().name, "worker-a");
  EXPECT_TRUE(mine.front().isTarget);
  EXPECT_EQ(mine[1].name, "worker-b");
  EXPECT_EQ(mine.back().endTs, kStart + 300);
}

TEST(LiveDecoder, ExecAndProcessExitUnloadTheModules) {
  Fixture f;
  f.config.rootPids = {kProg};
  RecordBuilder b;
  f.program(b);
  Fixture::sample(b, kProg, kProg, kStart + 100, {0x400100});
  // exec: the old image goes away, a new one is mapped.
  b.comm(kProg, kProg, "prog2", true, id(kProg, kProg, kStart + 200));
  RecordBuilder::Mmap2 mm;
  mm.pid = kProg;
  mm.tid = kProg;
  mm.addr = 0x500000;
  mm.len = 0x2000;
  mm.name = "/bin/prog2";
  mm.inode = 78;
  mm.id = id(kProg, kProg, kStart + 201);
  b.mmap2(mm);
  b.exit(kProg, 1, kProg, 1, kStart + 400, id(kProg, kProg, kStart + 400));
  f.decoder()->decode(view(b));
  RecordingSink& s = *f.sink;
  EXPECT_NE(s.indexOf("unload:/bin/prog"), SIZE_MAX);
  EXPECT_NE(s.indexOf("module:/bin/prog2"), SIZE_MAX);
  EXPECT_NE(s.indexOf("unload:/bin/prog2"), SIZE_MAX);  // the process exited
  EXPECT_LT(s.indexOf("module:/bin/prog"), s.indexOf("unload:/bin/prog"));
  bool ended = false;
  for (const auto& p : s.processes) ended = ended || p.endTs == kStart + 400;
  EXPECT_TRUE(ended);
}

TEST(LiveDecoder, RundownRecordsDescribeWhatExistedAtTheStart) {
  Fixture f;
  f.config.rootPids = {kProg};
  RecordBuilder rundown;
  f.program(rundown, kProg, kStart);  // stamped with the session start, as buildRundown does
  RecordBuilder b;
  Fixture::sample(b, kProg, kProg, kStart + 100, {0x400100});
  auto dec = f.decoder();
  dec->decode(view(rundown), true);
  dec->decode(view(b), false);
  RecordingSink& s = *f.sink;
  ASSERT_EQ(s.processes.size(), 1u);
  EXPECT_EQ(s.processes[0].startTs, kStart);  // a process that existed before the capture starts with it
  ASSERT_EQ(s.modules.size(), 1u);
  EXPECT_EQ(s.modules[0].loadTs, kStart);
  EXPECT_EQ(s.samples.size(), 1u);
}

TEST(LiveDecoder, RecordsOfSeveralRingsAreMergedByTime) {
  Fixture f;
  f.config.rootPids = {kProg};
  RecordBuilder rundown;
  rundown.fork(kProg, 1, kProg, 1, kStart, id(1, 1, kStart));
  rundown.comm(kProg, kProg, "prog", true, id(kProg, kProg, kStart));
  // One decode call: the sample's chunk comes first, but the mapping it needs is older (another ring).
  RecordBuilder b;
  Fixture::sample(b, kProg, kProg, kStart + 100, {0x400100});
  RecordBuilder::Mmap2 mm;
  mm.pid = kProg;
  mm.tid = kProg;
  mm.addr = 0x400000;
  mm.len = 0x2000;
  mm.name = "/bin/prog";
  mm.inode = 1;
  mm.id = id(kProg, kProg, kStart + 50);
  b.mmap2(mm);
  auto dec = f.decoder();
  dec->decode(view(rundown), true);
  dec->decode(view(b));
  RecordingSink& s = *f.sink;
  ASSERT_EQ(s.samples.size(), 1u);
  EXPECT_LT(s.indexOf("module:/bin/prog"), s.indexOf("sample:" + std::to_string(kStart + 100)));
}

TEST(LiveDecoder, DuplicateForkRecordsFromSeveralRingsAreIgnored) {
  Fixture f;
  RecordBuilder b;
  b.fork(kProg, 1, kProg, 1, kStart + 10, id(1, 1, kStart + 10));
  b.fork(kProg, 1, kProg, 1, kStart + 10, id(1, 1, kStart + 10));  // the same record via another ring
  Fixture::sample(b, kProg, kProg, kStart + 100, {0x400100});
  f.decoder()->decode(view(b));
  EXPECT_EQ(f.sink->processes.size(), 1u);
  EXPECT_EQ(f.sink->threads.size(), 1u);
}

// ---- scheduler events ----

fs::path schedFixture(const std::string& name) {
  return fs::path(TACHYON_FIXTURE_DIR) / "perf" / "sched" / name;
}
std::string readText(const fs::path& p) {
  std::ifstream in(p);
  EXPECT_TRUE(in.good()) << p;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
std::vector<uint8_t> switchRaw(uint32_t prevPid, int64_t prevState, uint32_t nextPid) {
  std::vector<uint8_t> b(64, 0);
  const uint16_t type = 316;
  std::memcpy(b.data(), &type, 2);
  std::memcpy(b.data() + 24, &prevPid, 4);
  const int32_t prio = 120;
  std::memcpy(b.data() + 28, &prio, 4);
  std::memcpy(b.data() + 32, &prevState, 8);
  std::memcpy(b.data() + 56, &nextPid, 4);
  std::memcpy(b.data() + 60, &prio, 4);
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

TEST(LiveDecoder, SchedTracepointsBecomeContextSwitchesAndReadyEvents) {
  Fixture f;
  f.config.rootPids = {kProg};
  f.config.layout = RecordLayout{kSchedSampleType, true};
  f.config.sched.mode = "tracepoint";
  f.config.sched.switches = f.config.sched.wakeups = true;
  f.config.sched.switchFormat = readText(schedFixture("sched_switch_wide_state.format"));
  f.config.sched.wakeFormat = readText(schedFixture("sched_waking.format"));
  f.config.sched.wakeEvent = "sched_waking";
  RecordBuilder b(f.config.layout);
  f.program(b);
  b.fork(kProg, kProg, 101, kProg, kStart + 40, id(kProg, kProg, kStart + 40));
  // Thread 100 blocks (state 1 = interruptible sleep) and thread 101 gets the cpu.
  RecordBuilder::Sample sw;
  sw.misc = kMiscKernel;
  sw.id = id(kProg, kProg, kStart + 500, 2);
  sw.chain = {kContextKernel, kKernelText + 0x100, kContextUser, 0x400100, 0x400020};
  sw.ip = sw.chain[1];
  sw.raw = switchRaw(kProg, 1, 101);
  b.sample(sw);
  // Thread 101 wakes thread 100 later.
  RecordBuilder::Sample wk;
  wk.misc = kMiscKernel;
  wk.id = id(kProg, 101, kStart + 700, 2);
  wk.chain = {kContextKernel, kKernelText + 0x200, kContextUser, 0x400200};
  wk.ip = wk.chain[1];
  wk.raw = wakeRaw(kProg, 2);
  b.sample(wk);
  f.decoder()->decode(view(b));
  RecordingSink& s = *f.sink;
  ASSERT_EQ(s.switches.size(), 1u);
  const auto& cs = s.switches[0];
  EXPECT_EQ(cs.cs.oldTid, kProg);
  EXPECT_EQ(cs.cs.oldPid, kProg);
  EXPECT_EQ(cs.cs.newTid, 101u);
  EXPECT_EQ(cs.cs.newPid, kProg);
  EXPECT_EQ(cs.cs.cpu, 2u);
  EXPECT_EQ(cs.cs.oldState, 5u);       // blocked
  EXPECT_EQ(cs.cs.oldWaitReason, 1u);  // prev_state & 0xFF
  EXPECT_EQ(cs.stack,
            (std::vector<uint64_t>{kKernelText + 0x100, 0x400100, 0x400020}));  // the blocking stack
  ASSERT_EQ(s.readies.size(), 1u);
  EXPECT_EQ(s.readies[0].rd.readiedTid, kProg);
  EXPECT_EQ(s.readies[0].rd.readyingTid, 101u);
  EXPECT_EQ(s.readies[0].rd.readyingPid, kProg);
  EXPECT_EQ(s.readies[0].stack, (std::vector<uint64_t>{kKernelText + 0x200, 0x400200}));
  // No samples came out of the tracepoint records.
  EXPECT_EQ(s.samples.size(), 0u);
}

TEST(LiveDecoder, SwitchRecordFallbackWithoutTracepoints) {
  Fixture f;
  f.config.rootPids = {kProg};
  f.config.sched.mode = "context_switch";
  f.config.sched.switches = true;
  RecordBuilder b;
  f.program(b);
  b.fork(kProg, kProg, 101, kProg, kStart + 40, id(kProg, kProg, kStart + 40));
  b.switchCpuWide(true, kProg, 101, id(kProg, kProg, kStart + 500, 1), /*preempt=*/true);
  f.decoder()->decode(view(b));
  ASSERT_EQ(f.sink->switches.size(), 1u);
  EXPECT_EQ(f.sink->switches[0].cs.oldTid, kProg);
  EXPECT_EQ(f.sink->switches[0].cs.newTid, 101u);
  EXPECT_EQ(f.sink->switches[0].cs.oldState, 1u);  // preempted: ready
}

// ---- LiveSource ----

TEST(LiveSource, DecodesQueuedChunksOnItsThreadAndSendsHeartbeats) {
  Fixture f;
  f.config.rootPids = {kProg};
  LiveSourceConfig lc;
  lc.decoder = f.config;
  lc.period = std::chrono::milliseconds(5);
  uint64_t now = 5'000'000;
  lc.nowNs = [&now] { return now += 1000; };
  LiveSource source(lc);
  source.start();
  RecordBuilder rundown;
  rundown.fork(kProg, 1, kProg, 1, kStart, id(1, 1, kStart));
  rundown.comm(kProg, kProg, "prog", true, id(kProg, kProg, kStart));
  source.submit(rundown.bytes(), {}, true);
  RecordBuilder meta, data;
  RecordBuilder::Mmap2 mm;
  mm.pid = kProg;
  mm.tid = kProg;
  mm.addr = 0x400000;
  mm.len = 0x2000;
  mm.name = "/bin/prog";
  mm.inode = 1;
  mm.id = id(kProg, kProg, kStart + 5);
  meta.mmap2(mm);
  for (int i = 0; i < 10; ++i) Fixture::sample(data, kProg, kProg, kStart + 100 + i, {0x400100});
  source.submit(meta.bytes(), data.bytes());
  source.finish();
  EXPECT_EQ(f.sink->sampleCount(), 10u);
  EXPECT_FALSE(f.sink->timeAdvances.empty());
  EXPECT_EQ(f.sink->dropped, 0u);
  const LiveSourceStats st = source.stats();
  EXPECT_EQ(st.droppedRecords, 0u);
  EXPECT_EQ(st.decoder.samples, 10u);
}

TEST(LiveSource, DropsSamplesNotMetadataWhenTheQueueIsFullAndSaysSo) {
  Fixture f;
  f.config.rootPids = {kProg};
  LiveSourceConfig lc;
  lc.decoder = f.config;
  lc.queueBytes = 1000;  // a tiny queue; the decoder thread is not started yet, so nothing drains it
  lc.period = std::chrono::milliseconds(5);
  LiveSource source(lc);
  RecordBuilder meta;
  f.program(meta);
  source.submit(meta.bytes(), {});  // metadata always fits (its cap is far larger)
  size_t submitted = 0;
  for (int chunk = 0; chunk < 20; ++chunk) {
    RecordBuilder data;
    for (int i = 0; i < 5; ++i)
      Fixture::sample(data, kProg, kProg, kStart + 1000 + chunk * 10 + i, {0x400100});
    submitted += 5;
    source.submit({}, data.bytes());
  }
  source.start();
  source.finish();
  const LiveSourceStats st = source.stats();
  EXPECT_GT(st.droppedRecords, 0u);
  EXPECT_EQ(st.decoder.samples + st.droppedRecords, submitted);
  EXPECT_EQ(f.sink->dropped, st.droppedRecords);  // reported to the sink as well
  EXPECT_EQ(st.metaLost, 0u);
  EXPECT_FALSE(f.sink->processes.empty());  // the metadata made it
}

TEST(LiveSource, SampleChunksThatWaitedTooLongAreDroppedNotDecoded) {
  Fixture f;
  f.config.rootPids = {kProg};
  LiveSourceConfig lc;
  lc.decoder = f.config;
  lc.maxLag = std::chrono::milliseconds(150);
  lc.period = std::chrono::milliseconds(5);
  LiveSource source(lc);
  RecordBuilder meta;
  f.program(meta);
  source.submit(meta.bytes(), {});
  RecordBuilder stale;
  for (int i = 0; i < 8; ++i) Fixture::sample(stale, kProg, kProg, kStart + 1000 + i, {0x400100});
  source.submit({}, stale.bytes());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the decoder has not started: they go stale
  RecordBuilder fresh;
  for (int i = 0; i < 3; ++i) Fixture::sample(fresh, kProg, kProg, kStart + 2000 + i, {0x400100});
  source.start();
  source.submit({}, fresh.bytes());
  source.finish();
  const LiveSourceStats st = source.stats();
  EXPECT_EQ(st.droppedRecords, 8u);
  EXPECT_EQ(f.sink->dropped, 8u);
  EXPECT_EQ(f.sink->sampleCount(), 3u);  // the fresh ones were decoded
}

}  // namespace
}  // namespace tachyon::capture::perf
