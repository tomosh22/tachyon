#include "etlconvert/stack_matcher.h"
#include "test_util.h"

#include <nlohmann/json.hpp>

#include "core/platform.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace tachyon::etlconvert {
namespace {

using namespace testing;

constexpr uint64_t kKernelBase = 0xFFFFF80000000000ull;
constexpr uint64_t kGameBase = 0x7ff600000000ull;
constexpr uint64_t kNtdllBase = 0x7ffb00000000ull;
constexpr uint32_t kNtdllStamp = 0x5E000000;

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

// Process 100 "game.exe" is the target (threads 101 and 102); process 200 "other.exe" has threads 201, 202.
// The rundown (DCEnd) of the ntdll images comes at the end, like the kernel logger writes it.
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

  // S1: target sample with a one-event user stack through game.exe into ntdll.
  s.sample(1000, 101, 100, 0, kGameBase + 0x1000);
  s.stack(1005, 1000, 100, 101, {kGameBase + 0x1000, kNtdllBase + 0x500});
  // S2: sample of a thread in another process.
  s.sample(2000, 201, 200, 1, 0x500000);
  s.stack(2004, 2000, 200, 201, {0x500000});
  // S3: kernel and user halves arrive as two StackWalk events; one user frame is unmapped.
  s.sample(3000, 102, 100, 2, kKernelBase + 0x100);
  s.stack(3003, 3000, 100, 102, {kKernelBase + 0x100, kKernelBase + 0x200});
  s.stack(3004, 3000, 100, 102, {kGameBase + 0x2000, 0x12345});
  // A stack of a non-target thread whose parent was filtered out.
  s.stack(3100, 3050, 200, 202, {0x1});
  // S4: idle thread sample. S5: target sample that never gets a stack.
  s.sample(4000, 0, 0, 3, kKernelBase + 0x900);
  s.sample(5000, 101, 100, 0, kGameBase + 0x1500);
  // C1: 201 -> 101, stack of the incoming thread. C2: unrelated threads. C3: 101 -> idle.
  s.cswitch(6000, 101, 0, /*new=*/101, /*old=*/201, /*waitReason=*/16);
  s.stack(6002, 6000, 100, 101, {kGameBase + 0x3000});
  s.cswitch(7000, 202, 1, 201, 202);
  s.cswitch(8000, 101, 0, 0, 101, /*waitReason=*/7);
  s.stack(8001, 8000, 100, 101, {kKernelBase + 0x300, kGameBase + 0x3100, kNtdllBase + 0x600});
  // R1: other process readies a target thread; its stack must be kept (resolved in process 200).
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

ConvertOptions baseOptions() {
  static const DevicePathMapper mapper = makeMapper();
  ConvertOptions o;
  o.targetPids = {100};
  o.samplingHz = 8000;
  o.agentVersion = "test-agent 1.2.3";
  o.captureOptionsJson = R"({"hz":8000,"stacks":true})";
  o.deviceMapper = &mapper;
  o.threaded = false;
  o.stackWindowMs = 1;  // 10'000 ticks at the 10 MHz test clock
  o.peLookup = [](const std::string& path) -> Result<PeInfo> {
    if (path == "C:\\game\\game.exe") return parsePe(buildPe(gameSpec()));
    if (path == "C:\\Windows\\System32\\ntdll.dll") return parsePe(buildPe(ntdllSpec()));
    return makeError(ErrorCode::NotFound, path);
  };
  return o;
}

struct Converted {
  ConvertStats stats;
  std::unique_ptr<ReadBack> rb;
};

Converted convert(const EventScript& script, const ConvertOptions& options, const char* name) {
  const auto path = tempPath(name);
  auto r = script.run(path, options);
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  Converted c;
  if (!r.ok()) return c;
  c.stats = std::move(r).value();
  c.rb = readBack(path);
  return c;
}

TEST(Pipeline, ProcessModeKeepsTargetSamplesAndRelatedSwitches) {
  auto c = convert(multiProcessScenario(), baseOptions(), "multi.tachyon");
  ASSERT_NE(c.rb, nullptr);
  const ConvertStats& st = c.stats;
  const ReadBack& rb = *c.rb;

  EXPECT_EQ(st.sampledProfileEvents, 5u);
  EXPECT_EQ(st.samplesWritten, 3u);
  EXPECT_EQ(st.samplesDropped, 2u);  // other process, idle
  EXPECT_EQ(st.cswitchEvents, 3u);
  EXPECT_EQ(st.cswitchesWritten, 2u);
  EXPECT_EQ(st.cswitchesDropped, 1u);
  EXPECT_EQ(st.readyEvents, 2u);
  EXPECT_EQ(st.readiesWritten, 1u);
  EXPECT_EQ(st.readiesDropped, 1u);

  ASSERT_EQ(rb.samples.size(), 3u);
  EXPECT_EQ(rb.samples.ts, (std::vector<uint64_t>{1000, 3000, 5000}));
  EXPECT_EQ(rb.samples.tid, (std::vector<uint32_t>{101, 102, 101}));
  EXPECT_EQ(rb.samples.cpu, (std::vector<uint16_t>{0, 2, 0}));
  // Module order follows load order: 0 game.exe, 1 ntoskrnl, 2 ntdll(100), 3 ntdll(200).
  EXPECT_EQ(rb.stackOf(rb.samples.callstack[0]),
            (std::vector<std::pair<uint32_t, uint64_t>>{{0, 0x1000}, {2, 0x500}}));
  EXPECT_EQ(rb.stackOf(rb.samples.callstack[1]),
            (std::vector<std::pair<uint32_t, uint64_t>>{
                {1, 0x100}, {1, 0x200}, {0, 0x2000}, {kInvalidIdx, 0x12345}}));
  EXPECT_EQ(rb.samples.callstack[2].get(), 0u);  // never matched: written without a stack
  EXPECT_EQ(rb.samples.flags[0], 0);
  EXPECT_EQ(rb.samples.flags[1], trace::kSampleFlagKernelLeaf);

  ASSERT_EQ(rb.switches.size(), 2u);
  const trace::ContextSwitchRec& c1 = rb.switches[0];
  EXPECT_EQ(c1.ts, 6000u);
  EXPECT_EQ(c1.new_tid, 101u);
  EXPECT_EQ(c1.old_tid, 201u);
  EXPECT_EQ(c1.old_wait_reason, 16);
  EXPECT_EQ(c1.old_state, 5);
  EXPECT_EQ(c1.old_wait_mode, 1);
  EXPECT_EQ(c1.new_priority, 8);
  EXPECT_EQ(c1.cpu, 0u);
  EXPECT_EQ(c1.reserved, trace::kSwitchFlagStackFromOtherThread);  // the stack belongs to the incoming thread
                                                                   // (101), not old_tid (201)
  EXPECT_EQ(rb.stackOf(c1.old_thread_stack), (std::vector<std::pair<uint32_t, uint64_t>>{{0, 0x3000}}));
  const trace::ContextSwitchRec& c3 = rb.switches[1];
  EXPECT_EQ(c3.new_tid, 0u);
  EXPECT_EQ(c3.old_tid, 101u);
  EXPECT_EQ(c3.reserved, 0u);
  EXPECT_EQ(rb.stackOf(c3.old_thread_stack),
            (std::vector<std::pair<uint32_t, uint64_t>>{{1, 0x300}, {0, 0x3100}, {2, 0x600}}));

  ASSERT_EQ(rb.readies.size(), 1u);
  const trace::ReadyRec& r = rb.readies[0];
  EXPECT_EQ(r.readied_tid, 101u);
  EXPECT_EQ(r.readying_tid, 201u);
  EXPECT_EQ(r.adjust_reason, 1);
  EXPECT_EQ(r.flags, 3);
  EXPECT_EQ(r.reserved, 0u);
  // The readying stack lives in process 200 and resolves against that process' ntdll (module 3).
  EXPECT_EQ(rb.stackOf(r.readying_stack), (std::vector<std::pair<uint32_t, uint64_t>>{{3, 0x700}}));

  // Stack matching accounting.
  EXPECT_EQ(st.stackWalkEvents, 8u);
  EXPECT_EQ(st.stacksMatched, 6u);    // S1, S3 (both halves), C1, C3, R1
  EXPECT_EQ(st.stacksUnmatched, 2u);  // S2's and the orphan stack, both of non-target threads
  EXPECT_EQ(st.stacksUnmatchedNonTarget, 2u);
  EXPECT_EQ(st.stacksTwoPart, 1u);
  EXPECT_EQ(st.parentsWithoutStack[static_cast<size_t>(ParentKind::Sample)], 1u);
  EXPECT_EQ(st.parentsWithStack[static_cast<size_t>(ParentKind::Sample)], 2u);
  EXPECT_EQ(st.parentsWithStack[static_cast<size_t>(ParentKind::CSwitch)], 2u);
  EXPECT_EQ(st.parentsWithStack[static_cast<size_t>(ParentKind::Ready)], 1u);
  uint64_t roleTotal = 0;
  for (auto& byKind : st.matchRole)
    for (uint64_t v : byKind) roleTotal += v;
  EXPECT_EQ(roleTotal, 5u);  // one per parent that got a stack (the second half is not a new role)
  EXPECT_EQ(st.framesUnmapped, 1u);
  EXPECT_EQ(st.framesTotal, 2u + 4u + 1u + 3u + 1u);

  // Time range = kept records.
  EXPECT_EQ(rb.reader.header().start_ts, 1000u);
  EXPECT_EQ(rb.reader.header().end_ts, 9000u);
  EXPECT_EQ(rb.reader.header().clock_frequency_hz, 10'000'000u);
  EXPECT_EQ(rb.reader.header().platform, trace::kPlatformWindows);
  EXPECT_EQ(rb.reader.header().flags & trace::kFlagPartial, 0u);
  EXPECT_EQ(st.startTs, 1000u);
  EXPECT_EQ(st.endTs, 9000u);
}

TEST(Pipeline, ThreadsProcessesAndNames) {
  auto c = convert(multiProcessScenario(), baseOptions(), "multi_threads.tachyon");
  ASSERT_NE(c.rb, nullptr);
  const ReadBack& rb = *c.rb;
  std::map<uint32_t, trace::ThreadRec> threads;
  for (const auto& t : rb.threads) threads[t.tid] = t;
  // Target threads, the readying/previous threads that appear in kept records, and the idle thread.
  ASSERT_EQ(threads.size(), 4u);
  EXPECT_EQ(threads.count(202), 0u);
  EXPECT_EQ(threads[101].pid, 100u);
  EXPECT_EQ(threads[101].flags & trace::kThreadFlagTargetProcess, 1u);
  EXPECT_EQ(threads[102].flags & trace::kThreadFlagTargetProcess, 1u);
  EXPECT_EQ(threads[201].pid, 200u);
  EXPECT_EQ(threads[201].flags, 0u);
  EXPECT_EQ(threads[0].pid, 0u);
  EXPECT_EQ(rb.str(threads[0].name), "Idle");
  EXPECT_EQ(rb.str(threads[101].name), "Main");
  EXPECT_EQ(uint64_t{threads[102].end_ts}, 9900u);
  EXPECT_EQ(uint64_t{threads[101].end_ts}, 9000u);  // still alive: trace end

  std::map<uint32_t, trace::ProcessRec> procs;
  for (const auto& p : rb.processes) procs[p.pid] = p;
  ASSERT_EQ(procs.size(), 3u);
  EXPECT_EQ(rb.str(procs[100].name), "game.exe");
  EXPECT_EQ(rb.str(procs[100].command_line), "\"game.exe\"");
  EXPECT_EQ(procs[100].parent_pid, 4u);
  EXPECT_EQ(rb.str(procs[200].name), "other.exe");

  ASSERT_EQ(rb.names.size(), 1u);
  EXPECT_EQ(rb.names[0].tid, 101u);
  EXPECT_EQ(rb.str(rb.names[0].name), "Main");
  EXPECT_EQ(rb.names[0].ts, 9600u);
}

TEST(Pipeline, ModulesAreIdentifiedAndMappedPerProcess) {
  auto c = convert(multiProcessScenario(), baseOptions(), "multi_modules.tachyon");
  ASSERT_NE(c.rb, nullptr);
  const ReadBack& rb = *c.rb;
  ASSERT_EQ(rb.modules.size(), 4u);
  const trace::ModuleInfo& game = rb.modules[0];
  EXPECT_EQ(rb.str(game.path), "C:\\game\\game.exe");
  EXPECT_EQ(game.pid, 100u);
  EXPECT_EQ(game.base, kGameBase);
  EXPECT_EQ(game.size, 0x10000u);
  EXPECT_EQ(game.id_kind, trace::kModuleIdPdb);
  EXPECT_EQ(game.id_len, 16);
  EXPECT_EQ(game.age, 3u);
  EXPECT_EQ(game.load_ts, 3u);
  EXPECT_EQ(game.unload_ts, 9000u);  // still loaded: trace end

  const trace::ModuleInfo& kernel = rb.modules[1];
  EXPECT_EQ(rb.str(kernel.path), "C:\\Windows\\system32\\ntoskrnl.exe");
  EXPECT_EQ(kernel.pid, 0u);
  EXPECT_EQ(kernel.id_kind,
            trace::kModuleIdPeTimestampSize);  // file not found: TimeDateStamp + SizeOfImage fallback
  EXPECT_EQ(kernel.id_len, 8);

  EXPECT_EQ(rb.modules[2].pid, 100u);
  EXPECT_EQ(rb.modules[3].pid, 200u);
  EXPECT_EQ(rb.modules[2].id_kind, trace::kModuleIdPdb);
  EXPECT_EQ(rb.modules[2].age, 7u);
  EXPECT_EQ(rb.modules[2].load_ts, 1000u);  // rundown: loaded before the trace -> trace start

  EXPECT_EQ(c.stats.modulesRsds, 3u);
  EXPECT_EQ(c.stats.modulesFallback, 1u);
  EXPECT_EQ(c.stats.modulesFileMissing, 1u);
}

TEST(Pipeline, SystemModeKeepsEverything) {
  ConvertOptions o = baseOptions();
  o.mode = CaptureMode::System;
  auto c = convert(multiProcessScenario(), o, "multi_system.tachyon");
  ASSERT_NE(c.rb, nullptr);
  EXPECT_EQ(c.stats.samplesWritten, 5u);
  EXPECT_EQ(c.stats.cswitchesWritten, 3u);
  EXPECT_EQ(c.stats.readiesWritten, 2u);
  EXPECT_EQ(c.stats.stacksUnmatchedNonTarget, 0u);
  // Target threads are still flagged.
  uint32_t flagged = 0;
  for (const auto& t : c.rb->threads) flagged += t.flags & trace::kThreadFlagTargetProcess;
  EXPECT_EQ(flagged, 2u);
  EXPECT_EQ(c.rb->threads.size(), 5u);  // idle plus the four real threads
}

TEST(Pipeline, EmptyTargetListMeansSystemMode) {
  ConvertOptions o = baseOptions();
  o.targetPids.clear();
  auto c = convert(multiProcessScenario(), o, "multi_notargets.tachyon");
  ASSERT_NE(c.rb, nullptr);
  EXPECT_EQ(c.stats.samplesWritten, 5u);
  for (const auto& t : c.rb->threads) EXPECT_EQ(t.flags, 0u);
}

TEST(Pipeline, ThreadedAndInlineOutputsAreIdentical) {
  ConvertOptions inlineOpt = baseOptions();
  ConvertOptions threadedOpt = baseOptions();
  threadedOpt.threaded = true;
  threadedOpt.batchEvents = 2;  // many small batches through a tiny queue
  threadedOpt.queueDepth = 2;
  auto a = convert(multiProcessScenario(), inlineOpt, "multi_inline.tachyon");
  auto b = convert(multiProcessScenario(), threadedOpt, "multi_threaded.tachyon");
  ASSERT_NE(a.rb, nullptr);
  ASSERT_NE(b.rb, nullptr);
  EXPECT_EQ(a.rb->samples, b.rb->samples);
  EXPECT_EQ(a.rb->nodes, b.rb->nodes);
  EXPECT_EQ(a.rb->frames, b.rb->frames);
  EXPECT_EQ(a.rb->modules, b.rb->modules);
  EXPECT_EQ(a.rb->switches, b.rb->switches);
  EXPECT_EQ(a.rb->readies, b.rb->readies);
  EXPECT_EQ(a.stats.samplesWritten, b.stats.samplesWritten);
}

TEST(Pipeline, SinglePassCannotMapModulesWhoseRundownComesLater) {
  ConvertOptions o = baseOptions();
  o.twoPass = false;
  auto c = convert(multiProcessScenario(), o, "multi_single.tachyon");
  ASSERT_NE(c.rb, nullptr);
  const ReadBack& rb = *c.rb;
  // ntdll frames are still unknown when the samples are processed: they stay absolute addresses.
  EXPECT_EQ(rb.stackOf(rb.samples.callstack[0]),
            (std::vector<std::pair<uint32_t, uint64_t>>{{0, 0x1000}, {kInvalidIdx, kNtdllBase + 0x500}}));
  EXPECT_EQ(rb.samples.size(), 3u);
  EXPECT_EQ(rb.modules.size(), 4u);  // metadata still ends up in the file
  EXPECT_GT(c.stats.framesUnmapped, 1u);
}

TEST(Pipeline, UnmappedFramesCanBeDropped) {
  ConvertOptions o = baseOptions();
  o.keepUnmappedFrames = false;
  auto c = convert(multiProcessScenario(), o, "multi_dropunmapped.tachyon");
  ASSERT_NE(c.rb, nullptr);
  EXPECT_EQ(c.rb->stackOf(c.rb->samples.callstack[1]),
            (std::vector<std::pair<uint32_t, uint64_t>>{{1, 0x100}, {1, 0x200}, {0, 0x2000}}));
  EXPECT_EQ(c.stats.framesUnmapped, 1u);
}

TEST(Pipeline, LateStackIsUnmatchedAndParentGetsNoStack) {
  EventScript s;
  s.thread(opcodes::kThreadStart, 1, 100, 101);
  s.sample(100, 101, 100, 0, 0x1000);
  s.sample(50'000, 101, 100, 0, 0x1000);  // newer event: the window (10'000 ticks) has passed
  s.stack(50'010, 100, 100, 101, {0x1000});
  ConvertOptions o = baseOptions();
  auto c = convert(s, o, "late.tachyon");
  ASSERT_NE(c.rb, nullptr);
  EXPECT_EQ(c.stats.stacksUnmatched, 1u);
  EXPECT_EQ(c.stats.parentsWithoutStack[0], 2u);
  EXPECT_EQ(c.rb->samples.size(), 2u);
  EXPECT_EQ(c.rb->samples.callstack[0].get(), 0u);
}

TEST(Pipeline, CountsMalformedAndStackKeyEventsAndWarns) {
  EventScript s;
  s.add(guids::kPerfInfo, opcodes::kPerfInfoSampledProfile, 2, 10, 1, 1, 0, ByteVec(4));
  s.add(guids::kStackWalk, opcodes::kStackWalkKeyKernel, 2, 11, 1, 1, 0, ByteVec(24));
  s.add(guids::kStackWalk, opcodes::kStackWalkKeyUser, 2, 12, 1, 1, 0, ByteVec(24));
  s.add(guids::kStackWalk, opcodes::kStackWalkStack, 2, 13, 1, 1, 0, ByteVec(5));
  Guid unknown = guids::kProcess;
  unknown.data1 ^= 0xFF;
  s.add(unknown, 1, 1, 14, 1, 1, 0, ByteVec(8));
  s.sample(15, 1, 1, 0, 0x1000);
  ConvertOptions o = baseOptions();
  o.targetPids.clear();
  o.eventsLost = 12;
  auto c = convert(s, o, "malformed.tachyon");
  ASSERT_NE(c.rb, nullptr);
  EXPECT_EQ(c.stats.malformedEvents, 2u);
  EXPECT_EQ(c.stats.stackKeyEvents, 2u);
  EXPECT_EQ(c.stats.otherEvents, 1u);
  EXPECT_EQ(c.stats.eventsTotal, 6u);
  EXPECT_EQ(c.stats.eventsLost, 12u);
  bool keyWarning = false;
  bool lostWarning = false;
  for (const auto& w : c.stats.warnings) {
    keyWarning |= w.find("stack-key") != std::string::npos;
    lostWarning |= w.find("lost") != std::string::npos;
  }
  EXPECT_TRUE(keyWarning);
  EXPECT_TRUE(lostWarning);
}

TEST(Pipeline, CaptureInfoJson) {
  auto c = convert(multiProcessScenario(), baseOptions(), "multi_info.tachyon");
  ASSERT_NE(c.rb, nullptr);
  const auto doc = nlohmann::json::parse(c.rb->captureInfo);
  EXPECT_EQ(doc["source"], "etl");
  EXPECT_EQ(doc["sampling_hz"], 8000);
  EXPECT_EQ(doc["agent_version"], "test-agent 1.2.3");
  EXPECT_EQ(doc["events_lost"], 0);
  EXPECT_EQ(doc["clock_frequency_hz"], 10'000'000);
  EXPECT_EQ(doc["mode"], "process");
  EXPECT_EQ(doc["target_pids"], nlohmann::json::array({100}));
  EXPECT_EQ(doc["capture_options"]["hz"], 8000);
  EXPECT_EQ(doc["host"]["platform"], "windows");
  EXPECT_EQ(doc["host"]["logical_cores"], 8);
  EXPECT_EQ(doc["convert_stats"]["samples_written"], 3);
  EXPECT_EQ(doc["convert_stats"]["modules_rsds"], 3);
  EXPECT_TRUE(doc["convert_stats"]["stack_match_roles"].contains("cswitch"));
}

TEST(Pipeline, NonJsonCaptureOptionsAreKeptAsString) {
  ConvertOptions o = baseOptions();
  o.captureOptionsJson = "not json {";
  auto c = convert(multiProcessScenario(), o, "multi_badjson.tachyon");
  ASSERT_NE(c.rb, nullptr);
  const auto doc = nlohmann::json::parse(c.rb->captureInfo);
  EXPECT_EQ(doc["capture_options"], "not json {");
}

TEST(Pipeline, EventsLostIsTakenFromTheLargerOfSessionAndHeader) {
  EventScript s = multiProcessScenario();
  ConvertOptions o = baseOptions();
  EtlFileInfo info;
  info.clockFrequencyHz = 10'000'000;
  auto pipeline = ConversionPipeline::create(tempPath("lost.tachyon"), o, info);
  ASSERT_TRUE(pipeline.ok());
  (*pipeline)->beginPass(PipelinePass::Main);
  for (const HexEvent& e : s.events()) (*pipeline)->onEvent(e.view());
  info.eventsLost = 99;
  (*pipeline)->setFileInfo(info);
  auto stats = (*pipeline)->finish();
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->eventsLost, 99u);
  EXPECT_FALSE((*pipeline)->finish().ok());
}

TEST(Pipeline, ProgressIsMonotonicAndEndsAtOne) {
  std::vector<float> seen;
  ConvertOptions o = baseOptions();
  o.progress = [&](float p) { seen.push_back(p); };
  auto c = convert(multiProcessScenario(), o, "progress.tachyon");
  ASSERT_NE(c.rb, nullptr);
  ASSERT_FALSE(seen.empty());
  EXPECT_FLOAT_EQ(seen.back(), 1.0f);
  for (size_t i = 1; i < seen.size(); ++i) EXPECT_LE(seen[i - 1], seen[i]);
}

TEST(Pipeline, RejectsZeroClockFrequency) {
  EtlFileInfo info;
  info.clockFrequencyHz = 0;
  EXPECT_FALSE(ConversionPipeline::create(tempPath("nofreq.tachyon"), baseOptions(), info).ok());
}

TEST(Pipeline, OutputOrderIsSortedByTimestamp) {
  EventScript s;
  s.thread(opcodes::kThreadStart, 1, 100, 101);
  // Timestamps arrive slightly out of order (different CPUs).
  s.sample(300, 101, 100, 0, 0x1000);
  s.sample(100, 101, 100, 1, 0x1000);
  s.sample(200, 101, 100, 2, 0x1000);
  auto c = convert(s, baseOptions(), "unsorted.tachyon");
  ASSERT_NE(c.rb, nullptr);
  EXPECT_EQ(c.rb->samples.ts, (std::vector<uint64_t>{100, 200, 300}));
  EXPECT_EQ(c.rb->samples.cpu, (std::vector<uint16_t>{1, 2, 0}));
}

TEST(Pipeline, ConvertOnNonWindowsIsUnsupported) {
#if !TACHYON_PLATFORM_WINDOWS
  auto r = EtlConverter::convert("x.etl", tempPath("x.tachyon"), baseOptions());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
#else
  GTEST_SKIP() << "covered by the Windows tests";
#endif
}

// ---- hex fixtures
// --------------------------------------------------------------------------------------------

TEST(HexFixtures, WP01SyntheticBasicConverts) {
  ConvertOptions o;
  o.targetPids = {100};
  o.threaded = false;
  static const DevicePathMapper mapper;  // Linux: paths are used as logged
  o.deviceMapper = &mapper;
  EtlFileInfo info;
  info.clockFrequencyHz = 10'000'000;
  const auto out = tempPath("synthetic_basic.tachyon");
  auto st = EtlConverter::convertHexEvents(
      std::string(TACHYON_FIXTURE_DIR) + "/etw/synthetic_basic.hexevents", out, o, info);
  ASSERT_TRUE(st.ok()) << st.error().toString();
  auto rb = readBack(out);
  ASSERT_NE(rb, nullptr);
  EXPECT_EQ(st->sampledProfileEvents, 4u);
  EXPECT_EQ(st->samplesWritten, 3u);  // the idle sample is dropped
  EXPECT_EQ(rb->samples.tid, (std::vector<uint32_t>{101, 101, 102}));
  EXPECT_EQ(st->cswitchesWritten, 1u);
  EXPECT_EQ(st->readiesWritten, 1u);
  EXPECT_EQ(st->setNameEvents, 1u);
  ASSERT_EQ(rb->processes.size(), 1u);
  EXPECT_EQ(rb->str(rb->processes[0].name), "game.exe");
  ASSERT_EQ(rb->modules.size(), 1u);
  EXPECT_EQ(rb->str(rb->modules[0].path), "\\Device\\HarddiskVolume3\\game\\game.exe");
  EXPECT_EQ(rb->modules[0].id_kind, trace::kModuleIdPeTimestampSize);  // no such file here: fallback
  EXPECT_EQ(rb->modules[0].base, 0x7ff600000000ull);
  EXPECT_EQ(rb->modules[0].size, 0x10000u);
  bool renamed = false;
  for (const auto& n : rb->names) renamed |= rb->str(n.name) == "RenderThread";
  EXPECT_TRUE(renamed);
  // All three user stacks of 101/102 resolved into the one module.
  for (CallstackIdx cs : rb->samples.callstack) EXPECT_GT(cs.get(), 0u);
  EXPECT_FALSE(EtlConverter::convertHexEvents("/nonexistent.hexevents", out, o, info).ok());
}

std::string scenarioAsHex(const EventScript& s) {
  std::ostringstream os;
  os << "# Hand-built synthetic fixture (NOT a real capture) for the etlconvert unit tests.\n"
        "# Process 100 'game.exe' (threads 101, 102) is the conversion target; process 200 'other.exe' has "
        "threads\n"
        "# 201, 202. Covers: kernel+user StackWalk pairs, an unmapped frame, filtered "
        "samples/switches/readies, a\n"
        "# cross-process readying stack, kernel image, and the DCEnd image rundown at the end of the file.\n"
        "# Regenerate with TACHYON_WRITE_FIXTURES=<dir> ctest -R FixtureMatchesBuilder.\n";
  for (const HexEvent& e : s.events()) os << formatHexEventLine(e.view()) << '\n';
  return os.str();
}

TEST(HexFixtures, FixtureMatchesBuilder) {
  const std::string text = scenarioAsHex(multiProcessScenario());
  std::string dir;
#if TACHYON_PLATFORM_WINDOWS
  char* raw = nullptr;
  size_t rawLen = 0;
  if (_dupenv_s(&raw, &rawLen, "TACHYON_WRITE_FIXTURES") == 0 && raw != nullptr) {
    dir = raw;
    free(raw);
  }
#else
  if (const char* env = std::getenv("TACHYON_WRITE_FIXTURES")) dir = env;
#endif
  if (!dir.empty()) {
    std::ofstream(dir + "/convert_multiproc.hexevents", std::ios::binary) << text;
    GTEST_SKIP() << "fixture written";
  }
  std::ifstream f(std::string(TACHYON_FIXTURE_DIR) + "/etw/convert_multiproc.hexevents", std::ios::binary);
  ASSERT_TRUE(f.good());
  std::ostringstream committed;
  committed << f.rdbuf();
  EXPECT_EQ(committed.str(), text);
}

TEST(HexFixtures, MultiProcFixtureGivesTheSameResultAsTheBuilder) {
  ConvertOptions o = baseOptions();
  EtlFileInfo info;
  info.clockFrequencyHz = 10'000'000;
  const auto out = tempPath("multiproc_hex.tachyon");
  auto st = EtlConverter::convertHexEvents(
      std::string(TACHYON_FIXTURE_DIR) + "/etw/convert_multiproc.hexevents", out, o, info);
  ASSERT_TRUE(st.ok()) << st.error().toString();
  auto direct = convert(multiProcessScenario(), o, "multiproc_direct.tachyon");
  auto rb = readBack(out);
  ASSERT_NE(rb, nullptr);
  ASSERT_NE(direct.rb, nullptr);
  EXPECT_EQ(rb->samples, direct.rb->samples);
  EXPECT_EQ(rb->nodes, direct.rb->nodes);
  EXPECT_EQ(rb->frames, direct.rb->frames);
  EXPECT_EQ(rb->switches, direct.rb->switches);
  EXPECT_EQ(rb->readies, direct.rb->readies);
}

}  // namespace
}  // namespace tachyon::etlconvert
