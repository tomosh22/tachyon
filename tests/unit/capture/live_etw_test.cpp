// WP-65w: the platform-independent parts of the ETW live source. LiveEtwDecoder turns scripted kernel events
// (the same EventScript the etlconvert tests use) into ILiveSink calls and must keep exactly what the file
// converter keeps; LiveEtwSource adds the queue, the timestamp ordering, the hold-back and the drop rules.
// The real-time consumer and the end-to-end behaviour are Windows-only (tests/windows/capture).
#include "../etlconvert/test_util.h"
#include "capture/windows_etw/live_etw_decoder.h"
#include "capture/windows_etw/live_etw_source.h"
#include "capture/windows_etw/rt_consumer.h"
#include "capture/windows_etw/session_config.h"
#include "capture/windows_etw/windows_etw_backend.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

namespace tachyon::capture::etw {
namespace {

using namespace etlconvert;
using namespace etlconvert::testing;

constexpr uint64_t kKernelBase = 0xFFFFF80000000000ull;
constexpr uint64_t kGameBase = 0x7ff600000000ull;
constexpr uint64_t kNtdllBase = 0x7ffb00000000ull;
constexpr uint32_t kNtdllStamp = 0x5E000000;
constexpr uint64_t kStartTs = 500;

const std::u16string kGamePath = u"\\Device\\HarddiskVolume3\\game\\game.exe";
const std::u16string kNtdllPath = u"\\Device\\HarddiskVolume3\\Windows\\System32\\ntdll.dll";
const std::u16string kKernelPath = u"\\SystemRoot\\system32\\ntoskrnl.exe";

PeSpec gameSpec() {
  PeSpec s;
  s.timeDateStamp = 0x5F001234;
  s.sizeOfImage = 0x10000;
  return s;
}
PeSpec ntdllSpec() {
  PeSpec s;
  s.timeDateStamp = kNtdllStamp;
  s.sizeOfImage = 0x20000;
  s.age = 7;
  s.guid[0] = 0xAB;
  s.pdb = "ntdll.pdb";
  return s;
}

DevicePathMapper makeMapper() {
  DevicePathMapper m;
  m.addDevice("\\Device\\HarddiskVolume3", "C:");
  m.setSystemRoot("C:\\Windows");
  return m;
}

PeLookupFn peLookup() {
  return [](const std::string& path) -> Result<PeInfo> {
    if (path == "C:\\game\\game.exe") return parsePe(buildPe(gameSpec()));
    if (path == "C:\\Windows\\System32\\ntdll.dll") return parsePe(buildPe(ntdllSpec()));
    return makeError(ErrorCode::NotFound, path);
  };
}

// The scenario of pipeline_test.cpp: process 100 "game.exe" (threads 101, 102) is the target, process 200
// "other.exe" (201, 202) is not. The end-of-session rundown (DCEnd) of the ntdll images comes last.
EventScript multiProcessScenario() {
  using namespace opcodes;
  EventScript s;
  s.process(kProcessDcStart, 1, 100, 4, "game.exe");
  s.process(kProcessDcStart, 1, 200, 4, "other.exe");
  s.thread(kThreadStart, 2, 100, 101);
  s.thread(kThreadStart, 2, 100, 102);
  s.thread(kThreadStart, 2, 200, 201);
  s.thread(kThreadStart, 2, 200, 202);
  s.image(kImageLoad, 3, 100, kGameBase, 0x10000, 0x5F001234, kGamePath);
  s.image(kImageLoad, 3, 0xFFFFFFFFu, kKernelBase, 0x100000, 1, kKernelPath);
  s.sample(1000, 101, 100, 0, kGameBase + 0x1000);
  s.stack(1005, 1000, 100, 101, {kGameBase + 0x1000, kNtdllBase + 0x500});
  s.sample(2000, 201, 200, 1, 0x500000);
  s.stack(2004, 2000, 200, 201, {0x500000});
  s.sample(3000, 102, 100, 2, kKernelBase + 0x100);
  s.stack(3003, 3000, 100, 102, {kKernelBase + 0x100, kKernelBase + 0x200});
  s.stack(3004, 3000, 100, 102, {kGameBase + 0x2000, 0x12345});
  s.stack(3100, 3050, 200, 202, {0x1});
  s.sample(4000, 0, 0, 3, kKernelBase + 0x900);
  s.sample(5000, 101, 100, 0, kGameBase + 0x1500);
  s.cswitch(6000, 101, 0, /*new=*/101, /*old=*/201, /*waitReason=*/16);
  s.stack(6002, 6000, 100, 101, {kGameBase + 0x3000});
  s.cswitch(7000, 202, 1, 201, 202);
  s.cswitch(8000, 101, 0, 0, 101, /*waitReason=*/7);
  s.stack(8001, 8000, 100, 101, {kKernelBase + 0x300, kGameBase + 0x3100, kNtdllBase + 0x600});
  s.ready(9000, 201, 200, 1, 101);
  s.stack(9001, 9000, 200, 201, {kNtdllBase + 0x700});
  s.ready(9500, 201, 200, 1, 202);
  s.setName(9600, 100, 101, u"Main");
  s.thread(kThreadEnd, 9900, 100, 102);
  s.process(kProcessDcEnd, 10000, 100, 4, "game.exe");
  s.image(kImageDcEnd, 10000, 100, kNtdllBase, 0x20000, kNtdllStamp, kNtdllPath);
  s.image(kImageDcEnd, 10000, 200, kNtdllBase, 0x20000, kNtdllStamp, kNtdllPath);
  return s;
}

// Records what the decoder tells the sink, in order. Thread-safe (LiveEtwSource calls from its thread).
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

  void onSessionInfo(const LiveSessionInfo& i) override {
    std::lock_guard lock(m);
    info = i;
  }
  void onProcess(const LiveProcess& p) override {
    std::lock_guard lock(m);
    processes.push_back(p);
  }
  void onThread(const LiveThread& t) override {
    std::lock_guard lock(m);
    threads.push_back(t);
  }
  void onModule(const LiveModule& mod) override {
    std::lock_guard lock(m);
    modules.push_back(mod);
  }
  void onSample(uint64_t ts, uint32_t pid, uint32_t tid, uint32_t cpu, uint16_t flags,
                std::span<const uint64_t> ips) override {
    std::lock_guard lock(m);
    samples.push_back({ts, pid, tid, cpu, flags, std::vector<uint64_t>(ips.begin(), ips.end())});
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
    advances.push_back(ts);
  }
  void onDropped(uint64_t n) override {
    std::lock_guard lock(m);
    dropped += n;
  }

  const LiveThread* thread(uint32_t tid) const {
    const LiveThread* last = nullptr;
    for (const LiveThread& t : threads)
      if (t.tid == tid) last = &t;
    return last;
  }
  const LiveProcess* process(uint32_t pid) const {
    const LiveProcess* last = nullptr;
    for (const LiveProcess& p : processes)
      if (p.pid == pid) last = &p;
    return last;
  }
  const LiveModule* module(uint32_t pid, uint64_t base, bool unloaded = false) const {
    const LiveModule* found = nullptr;
    for (const LiveModule& mod : modules)
      if (mod.pid == pid && mod.base == base && (mod.unloadTs != 0) == unloaded) found = &mod;
    return found;
  }

  mutable std::mutex m;
  LiveSessionInfo info;
  std::vector<LiveProcess> processes;
  std::vector<LiveThread> threads;
  std::vector<LiveModule> modules;
  std::vector<Sample> samples;
  std::vector<Switch> switches;
  std::vector<Ready> readies;
  std::vector<uint64_t> advances;
  uint64_t dropped = 0;
};

LiveEtwDecoderConfig decoderConfig(const std::shared_ptr<RecordingSink>& sink,
                                   std::vector<uint32_t> roots = {100}) {
  LiveEtwDecoderConfig c;
  c.sink = sink;
  c.clockFrequencyHz = 10'000'000;
  c.rootPids = std::move(roots);
  c.startTs = kStartTs;
  c.stackWindowMs = 1;  // 10'000 ticks at the 10 MHz test clock
  c.peLookup = peLookup();
  c.deviceMapper = makeMapper();
  return c;
}

void feed(LiveEtwDecoder& decoder, const EventScript& script) {
  for (const HexEvent& e : script.events()) decoder.onEvent(e.view());
  decoder.flush();
}

ConvertOptions converterOptions(std::vector<uint32_t> targets) {
  static const DevicePathMapper mapper = makeMapper();
  ConvertOptions o;
  o.targetPids = std::move(targets);
  o.deviceMapper = &mapper;
  o.threaded = false;
  o.stackWindowMs = 1;
  o.peLookup = peLookup();
  return o;
}

// ---------------------------------------------------------------------------------------------------------
// LiveEtwDecoder

TEST(LiveEtwDecoder, KeepsWhatTheFileConverterKeeps) {
  const EventScript script = multiProcessScenario();
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  feed(decoder, script);

  const auto path = tempPath("live_vs_file.tachyon");
  auto stats = script.run(path, converterOptions({100}));
  ASSERT_TRUE(stats.ok()) << stats.error().toString();
  EXPECT_EQ(sink->samples.size(), stats->samplesWritten);
  EXPECT_EQ(sink->switches.size(), stats->cswitchesWritten);
  EXPECT_EQ(sink->readies.size(), stats->readiesWritten);
  EXPECT_EQ(decoder.stats().samples, stats->samplesWritten);
  EXPECT_EQ(decoder.stats().stacksMatched, stats->stacksMatched);
  EXPECT_EQ(stats->samplesWritten, 3u);  // S1, S3, S5 (the other process and the idle thread are filtered)
  EXPECT_EQ(stats->cswitchesWritten, 2u);
  EXPECT_EQ(stats->readiesWritten, 1u);
}

TEST(LiveEtwDecoder, SamplesCarryRawStacksLeafFirst) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  feed(decoder, multiProcessScenario());
  ASSERT_EQ(sink->samples.size(), 3u);
  const auto& s1 = sink->samples[0];
  EXPECT_EQ(s1.ts, 1000u);
  EXPECT_EQ(s1.pid, 100u);
  EXPECT_EQ(s1.tid, 101u);
  EXPECT_EQ(s1.cpu, 0u);
  EXPECT_EQ(s1.flags, 0u);
  EXPECT_EQ(s1.ips, (std::vector<uint64_t>{kGameBase + 0x1000, kNtdllBase + 0x500}));
  // S3: the kernel and the user half arrive as two StackWalk events: kernel first, kernel leaf flag.
  const auto& s3 = sink->samples[1];
  EXPECT_EQ(s3.tid, 102u);
  EXPECT_EQ(s3.flags, trace::kSampleFlagKernelLeaf);
  EXPECT_EQ(s3.ips,
            (std::vector<uint64_t>{kKernelBase + 0x100, kKernelBase + 0x200, kGameBase + 0x2000, 0x12345}));
  // S5 never got a stack: the sampled instruction pointer is the stack.
  const auto& s5 = sink->samples[2];
  EXPECT_EQ(s5.ips, (std::vector<uint64_t>{kGameBase + 0x1500}));
  EXPECT_EQ(decoder.stats().parentsWithoutStack, 1u);
}

TEST(LiveEtwDecoder, SwitchesAndReadiesKeepTheirStacksAndFlags) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  feed(decoder, multiProcessScenario());
  ASSERT_EQ(sink->switches.size(), 2u);
  const auto& c1 = sink->switches[0].cs;
  EXPECT_EQ(c1.ts, 6000u);
  EXPECT_EQ(c1.newTid, 101u);
  EXPECT_EQ(c1.newPid, 100u);
  EXPECT_EQ(c1.oldTid, 201u);
  EXPECT_EQ(c1.oldPid, 200u);
  EXPECT_EQ(c1.oldWaitReason, 16);
  // The stack of C1 belongs to the incoming thread, not to the one that switched out.
  EXPECT_EQ(c1.flags, trace::kSwitchFlagStackFromOtherThread);
  EXPECT_EQ(sink->switches[0].stack, (std::vector<uint64_t>{kGameBase + 0x3000}));
  const auto& c3 = sink->switches[1].cs;
  EXPECT_EQ(c3.newTid, 0u);
  EXPECT_EQ(c3.oldTid, 101u);
  EXPECT_EQ(c3.oldWaitReason, 7);
  EXPECT_EQ(c3.flags, 0u);
  EXPECT_EQ(sink->switches[1].stack,
            (std::vector<uint64_t>{kKernelBase + 0x300, kGameBase + 0x3100, kNtdllBase + 0x600}));
  ASSERT_EQ(sink->readies.size(), 1u);
  const auto& r = sink->readies[0].rd;
  EXPECT_EQ(r.readiedTid, 101u);
  EXPECT_EQ(r.readyingTid, 201u);
  EXPECT_EQ(r.readyingPid, 200u);
  EXPECT_EQ(sink->readies[0].stack, (std::vector<uint64_t>{kNtdllBase + 0x700}));
}

TEST(LiveEtwDecoder, ReportsProcessesThreadsNamesAndModules) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  feed(decoder, multiProcessScenario());

  const LiveProcess* game = sink->process(100);
  ASSERT_NE(game, nullptr);
  EXPECT_EQ(game->name, "game.exe");
  EXPECT_EQ(game->ppid, 4u);
  EXPECT_EQ(game->startTs, kStartTs);  // a rundown process: stamped with the capture start
  EXPECT_EQ(sink->process(200), nullptr)
      << "the other process is only referenced, never reported in process mode";

  const LiveThread* main = sink->thread(101);
  ASSERT_NE(main, nullptr);
  EXPECT_EQ(main->pid, 100u);
  EXPECT_TRUE(main->isTarget);
  EXPECT_EQ(main->name, "Main");
  const LiveThread* ended = sink->thread(102);
  ASSERT_NE(ended, nullptr);
  EXPECT_EQ(ended->endTs, 9900u);

  // game.exe: loaded live, identified by the PDB guid of its PE; the path is the DOS path.
  const LiveModule* exe = sink->module(100, kGameBase);
  ASSERT_NE(exe, nullptr);
  EXPECT_EQ(exe->path, "C:\\game\\game.exe");
  EXPECT_EQ(exe->size, 0x10000u);
  EXPECT_EQ(exe->idKind, trace::kModuleIdPdb);
  EXPECT_EQ(exe->idLen, 16);
  EXPECT_EQ(exe->age, gameSpec().age);
  EXPECT_EQ(exe->loadTs, 3u);
  EXPECT_EQ(exe->unloadTs, 0u);
  // ntdll arrived through the end rundown of process 100 only (the DCEnd of 200 is not a target's).
  const LiveModule* ntdll = sink->module(100, kNtdllBase);
  ASSERT_NE(ntdll, nullptr);
  EXPECT_EQ(ntdll->idKind, trace::kModuleIdPdb);
  EXPECT_EQ(ntdll->age, 7u);
  EXPECT_EQ(ntdll->loadTs, kStartTs);
  EXPECT_EQ(sink->module(200, kNtdllBase), nullptr);
  // No kernel image is reported: kernel frames stay absolute addresses.
  for (const LiveModule& m : sink->modules) EXPECT_LT(m.base, kKernelAddressBase);
}

TEST(LiveEtwDecoder, SystemModeKeepsEveryKnownThread) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink, {}));
  feed(decoder, multiProcessScenario());
  // S1 S2 S3 S5 (the idle sample is dropped: tid 0), C1 C2 C3, R1 R2.
  EXPECT_EQ(sink->samples.size(), 4u);
  EXPECT_EQ(sink->switches.size(), 3u);
  EXPECT_EQ(sink->readies.size(), 2u);
  EXPECT_NE(sink->process(200), nullptr);
  const LiveThread* other = sink->thread(201);
  ASSERT_NE(other, nullptr);
  EXPECT_FALSE(other->isTarget) << "system mode has no target process";
}

TEST(LiveEtwDecoder, FollowsChildrenAndStopsAtProcessEnd) {
  using namespace opcodes;
  EventScript s;
  s.process(kProcessStart, 10, 100, 4, "game.exe");
  s.thread(kThreadStart, 11, 100, 101);
  s.process(kProcessStart, 20, 300, 100, "helper.exe");  // a child of the target
  s.thread(kThreadStart, 21, 300, 301);
  s.process(kProcessStart, 22, 400, 4, "unrelated.exe");
  s.thread(kThreadStart, 23, 400, 401);
  s.sample(100, 101, 100, 0, 0x1000);
  s.sample(110, 301, 300, 1, 0x2000);
  s.sample(120, 401, 400, 2, 0x3000);
  s.process(kProcessEnd, 200, 300, 100, "helper.exe");
  s.sample(210, 301, 300, 1, 0x2000);  // the pid is no longer a target
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoderConfig cfg = decoderConfig(sink);
  cfg.followChildren = true;
  LiveEtwDecoder decoder(cfg);
  feed(decoder, s);
  ASSERT_EQ(sink->samples.size(), 2u);
  EXPECT_EQ(sink->samples[0].tid, 101u);
  EXPECT_EQ(sink->samples[1].tid, 301u);
  EXPECT_EQ(sink->samples[1].pid, 300u);
  const LiveProcess* helper = sink->process(300);
  ASSERT_NE(helper, nullptr);
  EXPECT_EQ(helper->endTs, 200u);
  EXPECT_EQ(sink->process(400), nullptr);
  EXPECT_EQ(decoder.stats().droppedNotTarget, 2u);

  // Without followChildren the child is not a target.
  auto sink2 = std::make_shared<RecordingSink>();
  LiveEtwDecoder plain(decoderConfig(sink2));
  feed(plain, s);
  ASSERT_EQ(sink2->samples.size(), 1u);
  EXPECT_EQ(sink2->samples[0].tid, 101u);
}

TEST(LiveEtwDecoder, UnloadsModulesAtImageUnloadAndProcessEnd) {
  using namespace opcodes;
  EventScript s;
  s.process(kProcessStart, 10, 100, 4, "game.exe");
  s.image(kImageLoad, 11, 100, kGameBase, 0x10000, 0x5F001234, kGamePath);
  s.image(kImageLoad, 12, 100, kNtdllBase, 0x20000, kNtdllStamp, kNtdllPath);
  s.image(kImageLoad, 13, 100, kNtdllBase, 0x20000, kNtdllStamp, kNtdllPath);  // repeated: reported once
  s.image(kImageUnload, 50, 100, kNtdllBase, 0x20000, kNtdllStamp, kNtdllPath);
  s.process(kProcessEnd, 60, 100, 4, "game.exe");
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  feed(decoder, s);
  EXPECT_EQ(decoder.stats().modulesLoaded, 2u);
  const LiveModule* unloaded = sink->module(100, kNtdllBase, true);
  ASSERT_NE(unloaded, nullptr);
  EXPECT_EQ(unloaded->unloadTs, 50u);
  EXPECT_EQ(unloaded->loadTs, 12u);
  const LiveModule* exeUnloaded = sink->module(100, kGameBase, true);
  ASSERT_NE(exeUnloaded, nullptr) << "process end unloads what is left";
  EXPECT_EQ(exeUnloaded->unloadTs, 60u);
  EXPECT_EQ(sink->process(100)->endTs, 60u);
}

TEST(LiveEtwDecoder, WhatPrecedesAProcessEndIsKeptEvenWhenItArrivesAfterIt) {
  // The buffers of the CPUs are delivered in any order, so the last samples of a process can reach the
  // decoder after its Process End event. They belong to it; samples after the End do not.
  using namespace opcodes;
  EventScript s;
  s.process(kProcessStart, 10, 100, 4, "game.exe");
  s.thread(kThreadStart, 11, 100, 101);
  s.image(kImageLoad, 12, 100, kGameBase, 0x10000, 0x5F001234, kGamePath);
  s.process(kProcessEnd, 1000, 100, 4, "game.exe");
  s.sample(900, 101, 100, 0, kGameBase + 0x10);   // older than the End, delivered after it
  s.sample(1100, 101, 100, 0, kGameBase + 0x10);  // after the End: not the target's
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  for (const HexEvent& e : s.events()) decoder.onEvent(e.view());
  decoder.flush();
  ASSERT_EQ(sink->samples.size(), 1u);
  EXPECT_EQ(sink->samples[0].ts, 900u);
  EXPECT_EQ(sink->samples[0].pid, 100u);
  EXPECT_EQ(decoder.stats().droppedNotTarget, 1u);
  // The process was reported when it started and when it ended, not again for the late sample.
  EXPECT_EQ(sink->processes.size(), 2u);
  EXPECT_EQ(sink->process(100)->endTs, 1000u);
  // Its modules are unloaded at the End's timestamp, but only once the End is well in the past.
  const LiveModule* exe = sink->module(100, kGameBase, true);
  ASSERT_NE(exe, nullptr);
  EXPECT_EQ(exe->unloadTs, 1000u);
}

TEST(LiveEtwDecoder, AnEndedProcessIsForgottenAfterTheGracePeriod) {
  using namespace opcodes;
  EventScript s;
  s.process(kProcessStart, 10, 100, 4, "game.exe");
  s.image(kImageLoad, 12, 100, kGameBase, 0x10000, 0x5F001234, kGamePath);
  s.process(kProcessEnd, 1000, 100, 4, "game.exe");
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  for (const HexEvent& e : s.events()) decoder.onEvent(e.view());
  EXPECT_EQ(sink->module(100, kGameBase, true), nullptr) << "still within the grace period";
  EventScript later;
  later.sample(1000 + kEndedGraceSeconds * 10'000'000 + 1, 999, 999, 0, 0x10);  // any later event moves time
  for (const HexEvent& e : later.events()) decoder.onEvent(e.view());
  ASSERT_NE(sink->module(100, kGameBase, true), nullptr);
  EXPECT_EQ(sink->module(100, kGameBase, true)->unloadTs, 1000u);
  // The pid can be reused by an unrelated process: it is not a target.
  EventScript reuse;
  reuse.process(kProcessStart, 1000 + kEndedGraceSeconds * 10'000'000 + 10, 100, 4, "other.exe");
  reuse.thread(kThreadStart, 1000 + kEndedGraceSeconds * 10'000'000 + 11, 100, 555);
  reuse.sample(1000 + kEndedGraceSeconds * 10'000'000 + 20, 555, 100, 0, 0x10);
  for (const HexEvent& e : reuse.events()) decoder.onEvent(e.view());
  decoder.flush();
  EXPECT_TRUE(sink->samples.empty());
}

TEST(LiveEtwDecoder, RundownGivesThreadsNamesAndModulesOfTheTarget) {
  RundownSnapshot snap;
  snap.processes = {{100, 4, "game.exe"}, {200, 4, "other.exe"}};
  snap.threads = {{101, 100, "Render"}, {102, 100, ""}, {201, 200, ""}};
  snap.modules = {{100, kGameBase, 0x10000, "C:\\game\\game.exe"},
                  {100, kNtdllBase, 0x20000, "C:\\Windows\\System32\\ntdll.dll"},
                  {200, kNtdllBase, 0x20000, "C:\\Windows\\System32\\ntdll.dll"}};
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  decoder.applyRundown(snap);
  ASSERT_NE(sink->process(100), nullptr);
  EXPECT_EQ(sink->process(100)->startTs, kStartTs);
  EXPECT_EQ(sink->process(200), nullptr);
  ASSERT_NE(sink->thread(101), nullptr);
  EXPECT_EQ(sink->thread(101)->name, "Render");
  EXPECT_EQ(sink->thread(201), nullptr);
  ASSERT_NE(sink->module(100, kGameBase), nullptr);
  EXPECT_EQ(sink->module(100, kGameBase)->idKind, trace::kModuleIdPdb);
  EXPECT_EQ(sink->module(100, kGameBase)->loadTs, kStartTs);
  EXPECT_EQ(sink->module(200, kNtdllBase), nullptr);

  // A sample of a rundown thread now has its pid even though no Thread event was seen.
  EventScript s;
  s.sample(1000, 101, 0xFFFFFFFFu, 0, kGameBase + 0x40);
  feed(decoder, s);
  ASSERT_EQ(sink->samples.size(), 1u);
  EXPECT_EQ(sink->samples[0].pid, 100u);
}

TEST(LiveEtwDecoder, AStackWalkWithoutParentIsCountedNotFatal) {
  EventScript s;
  s.process(opcodes::kProcessDcStart, 1, 100, 4, "game.exe");
  s.thread(opcodes::kThreadStart, 2, 100, 101);
  s.stack(10, 5, 100, 101, {1, 2, 3});
  s.sample(100, 101, 100, 0, 0x10);
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwDecoder decoder(decoderConfig(sink));
  feed(decoder, s);
  EXPECT_EQ(decoder.stats().stacksUnmatched, 1u);
  ASSERT_EQ(sink->samples.size(), 1u);
  EXPECT_EQ(sink->samples[0].ips, (std::vector<uint64_t>{0x10}));
}

// ---------------------------------------------------------------------------------------------------------
// LiveEtwSource

LiveEtwSourceConfig sourceConfig(const std::shared_ptr<RecordingSink>& sink) {
  LiveEtwSourceConfig c;
  c.decoder = decoderConfig(sink);
  c.period = std::chrono::milliseconds(5);
  c.holdBack = std::chrono::milliseconds(0);
  c.maxLag = std::chrono::seconds(30);
  return c;
}

void submitAll(LiveEtwSource& source, const EventScript& script, bool reversed = false) {
  const auto& ev = script.events();
  if (reversed) {
    for (auto it = ev.rbegin(); it != ev.rend(); ++it) source.submit(it->view());
  } else {
    for (const HexEvent& e : ev) source.submit(e.view());
  }
  source.endBuffer();
}

TEST(LiveEtwSource, DecodesWhatTheDecoderWouldAndFinishFlushes) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSource source(sourceConfig(sink));
  source.start();
  source.arm({100});
  submitAll(source, multiProcessScenario());
  source.finish();
  EXPECT_EQ(sink->samples.size(), 3u);
  EXPECT_EQ(sink->switches.size(), 2u);
  EXPECT_EQ(sink->readies.size(), 1u);
  const LiveEtwSourceStats st = source.stats();
  EXPECT_EQ(st.decoder.samples, 3u);
  EXPECT_EQ(st.droppedEvents, 0u);
  EXPECT_EQ(st.metaLost, 0u);
  EXPECT_GT(st.eventsSubmitted, 20u);
}

TEST(LiveEtwSource, OrdersEventsByTimestampWithinABatch) {
  // The buffers of different CPUs arrive in any order: a whole scenario fed backwards decodes the same.
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSource source(sourceConfig(sink));
  source.start();
  source.arm({100});
  submitAll(source, multiProcessScenario(), /*reversed=*/true);
  source.finish();
  ASSERT_EQ(sink->samples.size(), 3u);
  EXPECT_EQ(sink->samples[0].ts, 1000u);
  EXPECT_EQ(sink->samples[1].ts, 3000u);
  EXPECT_EQ(sink->samples[2].ts, 5000u);
  EXPECT_EQ(sink->samples[0].ips.size(), 2u) << "the stack found its parent";
  EXPECT_EQ(sink->samples[1].ips.size(), 4u);
  EXPECT_NE(sink->module(100, kGameBase), nullptr) << "the image load came before the samples it explains";
}

TEST(LiveEtwSource, NothingIsDecodedBeforeArm) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSource source(sourceConfig(sink));
  source.start();
  submitAll(source, multiProcessScenario());
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_TRUE(sink->samples.empty());
  source.arm({100});  // the launched pid is known now
  source.finish();
  EXPECT_EQ(sink->samples.size(), 3u);
}

TEST(LiveEtwSource, FinishWithoutArmIsHarmless) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSource source(sourceConfig(sink));
  source.start();
  source.finish();
  source.finish();
  EXPECT_TRUE(sink->samples.empty());
}

TEST(LiveEtwSource, HoldsDataBackAndDropsWhatIsTooOld) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSourceConfig cfg = sourceConfig(sink);
  cfg.holdBack = std::chrono::milliseconds(150);
  cfg.maxLag = std::chrono::milliseconds(1000);
  LiveEtwSource source(cfg);
  source.start();
  source.arm({100});
  EventScript meta;
  meta.process(opcodes::kProcessStart, 1, 100, 4, "game.exe");
  meta.thread(opcodes::kThreadStart, 2, 100, 101);
  EventScript data;
  data.sample(10, 101, 100, 0, 0x1000);
  data.sample(50'000, 101, 100, 0,
              0x1001);  // 5 ms of trace clock later: releases the first (stack window 1 ms)
  submitAll(source, meta);
  submitAll(source, data);
  // Metadata is decoded at once; the sample waits for the hold-back.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_NE(sink->process(100), nullptr);
  EXPECT_TRUE(sink->samples.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_EQ(sink->samples.size(), 1u) << "the newest sample still waits for its stack";
  source.finish();
  EXPECT_EQ(sink->samples.size(), 2u);
  EXPECT_EQ(sink->dropped, 0u);
}

TEST(LiveEtwSource, StaleDataIsDroppedAndReportedMetadataIsNot) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSourceConfig cfg = sourceConfig(sink);
  cfg.holdBack = std::chrono::milliseconds(0);
  cfg.maxLag = std::chrono::milliseconds(50);
  cfg.period = std::chrono::milliseconds(300);  // the thread sleeps past maxLag before it looks at the queue
  LiveEtwSource source(cfg);
  source.start();
  // Not armed yet: the thread does not drain. Submit, wait past maxLag, then arm: the data is stale.
  EventScript s;
  s.process(opcodes::kProcessStart, 1, 100, 4, "game.exe");
  s.thread(opcodes::kThreadStart, 2, 100, 101);
  s.sample(10, 101, 100, 0, 0x1000);
  s.sample(11, 101, 100, 0, 0x1001);
  submitAll(source, s);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  source.arm({100});
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  source.finish();
  EXPECT_TRUE(sink->samples.empty());
  EXPECT_EQ(sink->dropped, 2u);
  EXPECT_NE(sink->process(100), nullptr) << "metadata is never dropped";
  EXPECT_NE(sink->thread(101), nullptr);
  EXPECT_EQ(source.stats().droppedEvents, 2u);
}

TEST(LiveEtwSource, DataOverTheQueueCapIsDroppedAndCounted) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSourceConfig cfg = sourceConfig(sink);
  cfg.queueBytes = 200;  // room for about three sampled-profile records
  LiveEtwSource source(cfg);
  source.start();  // not armed: nothing is drained, the queue fills
  EventScript s;
  s.process(opcodes::kProcessStart, 1, 100, 4, "game.exe");
  s.thread(opcodes::kThreadStart, 2, 100, 101);
  submitAll(source, s);
  for (uint64_t i = 0; i < 20; ++i) {
    EventScript one;
    one.sample(10 + i, 101, 100, 0, 0x1000 + i);
    submitAll(source, one);
  }
  source.arm({100});
  source.finish();
  const LiveEtwSourceStats st = source.stats();
  EXPECT_GT(st.droppedEvents, 10u);
  EXPECT_EQ(sink->samples.size() + st.droppedEvents, 20u);
  EXPECT_EQ(sink->dropped, st.droppedEvents) << "reported to the sink";
  EXPECT_NE(sink->thread(101), nullptr);
}

TEST(LiveEtwSource, SendsHeartbeatsOnTheTraceClock) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSourceConfig cfg = sourceConfig(sink);
  std::atomic<uint64_t> clock{1000};
  cfg.nowTicks = [&clock] { return clock.fetch_add(10); };
  LiveEtwSource source(cfg);
  source.start();
  source.arm({100});
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  source.finish();
  std::lock_guard lock(sink->m);
  ASSERT_GE(sink->advances.size(), 2u) << "a quiet capture still moves time";
  EXPECT_TRUE(std::is_sorted(sink->advances.begin(), sink->advances.end()));
}

TEST(LiveEtwSource, IgnoresEventsTheDecoderDoesNotUse) {
  auto sink = std::make_shared<RecordingSink>();
  LiveEtwSource source(sourceConfig(sink));
  source.start();
  source.arm({100});
  Guid other = guids::kProcess;
  other.data1 ^= 0x1234;
  ByteVec payload(16, std::byte{0});
  source.submit(makeEvent(other, 1, 0, 5, 1, 1, 0, payload));
  source.submit(makeEvent(guids::kPerfInfo, 99, 0, 5, 1, 1, 0, payload));  // not SampledProfile
  source.endBuffer();
  source.finish();
  EXPECT_EQ(source.stats().eventsIgnored, 2u);
  EXPECT_EQ(source.stats().eventsSubmitted, 0u);
}

// ---------------------------------------------------------------------------------------------------------
// Session / backend plumbing that runs everywhere

TEST(LiveEtwConfig, LiveRequestsRealTimeModeOnTheSameSession) {
  CaptureOptions o;
  o.outPath = "live.tachyon";
  auto plain = makeSessionConfig(o, "Tachyon 1 0", makeSessionGuid(1, 0, 0), "live.tachyon.etl", 8);
  ASSERT_TRUE(plain.ok());
  EXPECT_FALSE(plain->realTime);
  o.live = true;
  auto live = makeSessionConfig(o, "Tachyon 1 0", makeSessionGuid(1, 0, 0), "live.tachyon.etl", 8);
  ASSERT_TRUE(live.ok());
  EXPECT_TRUE(live->realTime);
  // Everything else is unchanged: the ETL is written exactly as without live.
  EXPECT_EQ(live->enableFlags, plain->enableFlags);
  EXPECT_EQ(live->buffers.bufferSizeKb, plain->buffers.bufferSizeKb);
  EXPECT_EQ(live->buffers.maximumBuffers, plain->buffers.maximumBuffers);
  EXPECT_EQ(live->stackEvents.size(), plain->stackEvents.size());
  o.circularMb = 64;
  auto circ = makeSessionConfig(o, "Tachyon 1 0", makeSessionGuid(1, 0, 0), "live.tachyon.etl", 8);
  ASSERT_TRUE(circ.ok());
  EXPECT_TRUE(circ->realTime);
  EXPECT_TRUE(circ->circular);
  const auto json = nlohmann::json::parse(captureOptionsJson(o, *circ));
  EXPECT_TRUE(json["live"].get<bool>());
}

TEST(LiveEtwConfig, TheBackendSupportsLive) {
  WindowsEtwBackend backend;
  EXPECT_TRUE(backend.supportsLive());
}

TEST(LiveEtwConfig, TraceClockMovesForward) {
  const uint64_t a = traceClockNow();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const uint64_t b = traceClockNow();
  EXPECT_GT(b, a);
  EXPECT_GT(traceClockFrequency(), 0u);
}

}  // namespace
}  // namespace tachyon::capture::etw
