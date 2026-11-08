// LiveEncoder -> LiveModel round trips (WP-65a): every record type, keyframes, window trimming, retention gaps,
// table caps, out-of-order input, and snapshots that the analysis code accepts.
#include "analysis/lod.h"
#include "analysis/query.h"
#include "analysis/scope_pairing.h"
#include "analysis/thread_states.h"
#include "control/live_encoder.h"
#include "model/live_model.h"
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

namespace tachyon::control {
namespace {

using capture::LiveModule;
using model::LiveModel;
using model::LiveModelOptions;
using model::TraceModel;
using namespace std::chrono_literals;

constexpr uint64_t kSec = 1'000'000'000ull;

std::span<const std::byte> bytesOf(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

LiveEncoderConfig testConfig() {
  LiveEncoderConfig c;
  c.autoTick = false;
  return c;
}

// An encoder and a model fed incrementally from it (what a subscriber that stayed connected sees).
struct Pipe {
  LiveEncoder enc;
  LiveModel model;
  uint64_t next = 1;

  explicit Pipe(LiveEncoderConfig c = testConfig(), LiveModelOptions o = {}) : enc(std::move(c)), model(o) {}

  void pump() {
    enc.flush();
    LiveBatch b;
    while (enc.waitBatch(next, b, 0ms) == LiveWait::Ready) {
      EXPECT_TRUE(model.apply(bytesOf(*b.data), false).ok());
      ++next;
    }
  }
};

void sessionAndTarget(capture::ILiveSink& s, uint32_t pid = 100) {
  capture::LiveSessionInfo info;
  info.clockFrequencyHz = kSec;
  info.platform = 2;
  info.samplingHz = 1000;
  info.startTs = 1000;
  s.onSessionInfo(info);
  capture::LiveProcess p;
  p.pid = pid;
  p.ppid = 1;
  p.startTs = 1000;
  p.name = "game";
  p.commandLine = "/bin/game --fast";
  s.onProcess(p);
  capture::LiveModule m;
  m.pid = pid;
  m.base = 0x400000;
  m.size = 0x10000;
  m.path = "/bin/game";
  m.idKind = 2;
  m.idLen = 4;
  m.id[0] = 0xAB;
  m.loadTs = 1000;
  s.onModule(m);
  capture::LiveModule lib = m;
  lib.base = 0x7f0000000000ull;
  lib.size = 0x20000;
  lib.path = "/lib/libc.so";
  s.onModule(lib);
}

capture::LiveThread thread(uint32_t pid, uint32_t tid, const char* name) {
  capture::LiveThread t;
  t.pid = pid;
  t.tid = tid;
  t.startTs = 1000;
  t.name = name;
  t.isTarget = true;
  return t;
}

TEST(LiveRoundTrip, EveryRecordTypeSurvives) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "main"));
  const std::vector<uint64_t> stackA = {0x400010, 0x400020, 0x400030};  // leaf first
  p.enc.onSample(2000, 100, 101, 3, 0, stackA);
  p.enc.onSample(3000, 100, 101, 3, 1, std::vector<uint64_t>{0xFFFFFFFF81000000ull, 0x400010, 0x400020});
  p.enc.onSample(4000, 100, 101, 2, 0, std::vector<uint64_t>{0x7f0000000100ull, 0x500000});  // libc, unmapped

  capture::LiveContextSwitch cs;
  cs.ts = 3500;
  cs.cpu = 3;
  cs.oldPid = 100;
  cs.oldTid = 101;
  cs.newPid = 100;
  cs.newTid = 102;
  cs.oldState = 5;
  cs.oldWaitReason = 2;
  cs.oldStack = stackA;
  p.enc.onContextSwitch(cs);
  capture::LiveReady rd;
  rd.ts = 3600;
  rd.cpu = 1;
  rd.readiedPid = 100;
  rd.readiedTid = 101;
  rd.readyingPid = 100;
  rd.readyingTid = 102;
  rd.readyingStack = stackA;
  p.enc.onReady(rd);

  p.enc.onScopeEvent({2500, 100, 101, trace::kScopeBegin, "Update", "dt=1", 0xFF00FFFF, 0x400015});
  p.enc.onScopeEvent({2900, 100, 101, trace::kScopeEnd, "", "", 0, 0});
  p.enc.onFrameMark({2000, 100, 101, "Frame", 7});
  p.enc.onMarker({2100, 100, 0, "Hitch", "long", 0xFF0000FF});
  p.enc.onPlot({2200, "fps", 59.5});
  p.enc.onDropped(5);
  p.enc.onTimeAdvance(5000);
  p.pump();

  auto snap = p.model.snapshot();
  const TraceModel& m = *snap;
  EXPECT_TRUE(m.isPartial);
  EXPECT_FALSE(m.loadStatus.complete);
  EXPECT_NE(m.captureInfo.raw_json.find("\"live\":true"), std::string::npos);
  EXPECT_EQ(m.captureInfo.events_lost, 5u);
  EXPECT_EQ(m.captureInfo.sampling_hz, 1000u);
  EXPECT_EQ(m.header.clock_frequency_hz, kSec);
  EXPECT_EQ(m.header.end_ts, 5000u);
  EXPECT_EQ(m.sampleIntervalTicks, 1'000'000u);

  ASSERT_EQ(m.processes.size(), 1u);
  EXPECT_EQ(m.processes[0].pid, 100u);
  EXPECT_EQ(m.processes[0].name, "game");
  EXPECT_EQ(m.processes[0].command_line, "/bin/game --fast");

  ASSERT_EQ(m.modules.size(), 2u);
  EXPECT_EQ(m.modules[0].path, "/bin/game");
  EXPECT_EQ(m.modules[0].id.kind, 2);
  EXPECT_EQ(m.modules[0].id.bytes[0], 0xAB);

  // Threads: 101 (main), 102 (created by the context switch).
  ASSERT_EQ(m.threads.size(), 2u);
  EXPECT_EQ(m.threads[0].name, "main");
  EXPECT_TRUE(m.threads[0].is_target);
  EXPECT_EQ(m.threads[0].role, model::ThreadRole::Main);
  EXPECT_EQ(m.tidToThread.at(101).get(), 0u);
  EXPECT_EQ(m.tidToThread.at(102).get(), 1u);

  // Samples and their callstacks.
  const auto& cols = m.samplesByThread[0];
  ASSERT_EQ(cols.ts.size(), 3u);
  EXPECT_EQ(cols.ts[0], 2000u);
  EXPECT_EQ(cols.cpu[2], 2u);
  EXPECT_EQ(cols.flags[1], 1u);
  auto framesOf = [&](CallstackIdx leaf) {  // leaf first: (module, addr_or_rva)
    std::vector<std::pair<uint32_t, uint64_t>> out;
    for (CallstackIdx n = leaf; n.valid() && n.get() != 0; n = m.nodes[n.get()].parent) {
      const model::Frame& f = m.frames[m.nodes[n.get()].frame.get()];
      out.emplace_back(f.module.get(), f.addr_or_rva);
    }
    return out;
  };
  using FrameList = std::vector<std::pair<uint32_t, uint64_t>>;
  EXPECT_EQ(framesOf(cols.callstack[0]), (FrameList{{0, 0x10}, {0, 0x20}, {0, 0x30}}));
  EXPECT_EQ(framesOf(cols.callstack[1]), (FrameList{{kInvalidIdx, 0xFFFFFFFF81000000ull}, {0, 0x10}, {0, 0x20}}));
  EXPECT_EQ(framesOf(cols.callstack[2]), (FrameList{{1, 0x100}, {kInvalidIdx, 0x500000}}));

  ASSERT_EQ(m.contextSwitches.size(), 1u);
  EXPECT_EQ(m.contextSwitches[0].new_tid, 102u);
  EXPECT_EQ(m.contextSwitches[0].old_wait_reason, 2u);
  EXPECT_EQ(framesOf(m.contextSwitches[0].old_thread_stack), framesOf(cols.callstack[0]));
  ASSERT_EQ(m.readyEvents.size(), 1u);
  EXPECT_EQ(m.readyEvents[0].readying_tid, 102u);
  EXPECT_EQ(framesOf(m.readyEvents[0].readying_stack), framesOf(cols.callstack[0]));

  ASSERT_EQ(m.scopeEvents.size(), 2u);
  EXPECT_EQ(m.strings.get(m.scopeEvents[0].id), "Update");
  EXPECT_EQ(m.strings.get(m.scopeEvents[0].data), "dt=1");
  EXPECT_EQ(m.scopeEvents[0].caller_addr, 0x400015u);
  ASSERT_EQ(m.frameMarks.size(), 1u);
  EXPECT_EQ(m.strings.get(m.frameMarks[0].name), "Frame");
  EXPECT_EQ(m.frameMarks[0].frame_number, 7u);
  ASSERT_EQ(m.markers.size(), 1u);
  EXPECT_FALSE(m.markers[0].thread.valid());  // a global marker
  EXPECT_EQ(m.strings.get(m.markers[0].name), "Hitch");
  ASSERT_EQ(m.plots.size(), 1u);
  EXPECT_EQ(m.plots[0].value[0], 59.5);
  EXPECT_TRUE(m.header.flags & trace::kFlagHasInstrumentation);
  EXPECT_GT(p.model.generation(), 0u);
}

TEST(LiveRoundTrip, RenameAndModuleUnloadAreRedefinitions) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "thread-a"));
  p.pump();
  p.enc.onThread(thread(100, 101, "renamed"));  // after the definition went out: a ThreadName record
  capture::LiveModule gone;
  gone.pid = 100;
  gone.base = 0x7f0000000000ull;
  gone.size = 0x20000;
  gone.path = "/lib/libc.so";
  gone.unloadTs = 9000;
  p.enc.onModule(gone);
  p.enc.onTimeAdvance(9500);
  p.pump();
  auto m = p.model.snapshot();
  EXPECT_EQ(m->threads[0].name, "renamed");
  EXPECT_EQ(m->modules[1].unload_ts, 9000u);
  // An unloaded module no longer resolves frames: the address becomes absolute.
  p.enc.onSample(9600, 100, 101, 0, 0, std::vector<uint64_t>{0x7f0000000100ull});
  p.pump();
  m = p.model.snapshot();
  const auto node = m->samplesByThread[0].callstack[0];
  EXPECT_FALSE(m->frames[m->nodes[node.get()].frame.get()].module.valid());
}

TEST(LiveRoundTrip, StackCacheIsInvalidatedByModuleChanges) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "main"));
  const std::vector<uint64_t> ips = {0x800100};
  p.enc.onSample(2000, 100, 101, 0, 0, ips);  // unmapped
  capture::LiveModule later;
  later.pid = 100;
  later.base = 0x800000;
  later.size = 0x1000;
  later.path = "/lib/late.so";
  later.loadTs = 2500;
  p.enc.onModule(later);
  p.enc.onSample(3000, 100, 101, 0, 0, ips);  // the same ips now resolve to a module
  p.pump();
  auto m = p.model.snapshot();
  const auto& c = m->samplesByThread[0];
  ASSERT_EQ(c.ts.size(), 2u);
  EXPECT_NE(c.callstack[0].get(), c.callstack[1].get());
  EXPECT_FALSE(m->frames[m->nodes[c.callstack[0].get()].frame.get()].module.valid());
  EXPECT_TRUE(m->frames[m->nodes[c.callstack[1].get()].frame.get()].module.valid());
}

TEST(LiveRoundTrip, OutOfOrderEventsAreSortedInTheSnapshot) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "main"));
  for (uint64_t ts : {2000, 4000, 3000, 6000, 5000})
    p.enc.onSample(ts, 100, 101, 0, 0, std::vector<uint64_t>{0x400010});
  p.pump();
  for (uint64_t ts : {1500, 4500}) p.enc.onSample(ts, 100, 101, 0, 0, std::vector<uint64_t>{0x400010});
  p.pump();
  auto m = p.model.snapshot();
  const auto& ts = m->samplesByThread[0].ts;
  EXPECT_TRUE(std::is_sorted(ts.begin(), ts.end()));
  EXPECT_EQ(ts.size(), 7u);
}

TEST(LiveRoundTrip, WindowTrimsOldEventsAndKeepsTables) {
  LiveModelOptions o;
  o.windowNs = 1 * kSec;
  LiveEncoderConfig c = testConfig();
  c.windowSeconds = 1;
  Pipe p(c, o);
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "main"));
  for (uint64_t s = 0; s < 4; ++s) {
    for (int i = 0; i < 10; ++i)
      p.enc.onSample(s * kSec + 1000 + i * 1000, 100, 101, 0, 0, std::vector<uint64_t>{0x400010 + s});
    p.enc.onTimeAdvance((s + 1) * kSec);
    p.pump();
  }
  auto m = p.model.snapshot();
  const uint64_t span = m->header.end_ts - m->header.start_ts;
  EXPECT_LE(span, kSec);
  for (uint64_t ts : m->samplesByThread[0].ts) EXPECT_GE(ts, m->header.start_ts);
  EXPECT_EQ(m->samplesByThread[0].ts.size(), 10u);  // only the last second's samples
  EXPECT_EQ(m->modules.size(), 2u);                 // definitions are kept
  EXPECT_GE(m->nodes.size(), 5u);                   // including the stacks of trimmed samples
  // The encoder trimmed its batches too.
  EXPECT_LE(p.enc.stats().retainedBatches, 3u);
}

TEST(LiveKeyframe, LateSubscriberEqualsIncrementalSubscriber) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "main"));
  p.enc.onThread(thread(100, 102, "worker"));
  for (int i = 0; i < 200; ++i) {
    p.enc.onSample(2000 + i * 10, 100, 101 + i % 2, i % 4, 0,
                   std::vector<uint64_t>{0x400010 + static_cast<uint64_t>(i % 7), 0x400100});
    if (i % 50 == 49) p.pump();
  }
  p.enc.onScopeEvent({2100, 100, 101, trace::kScopeBegin, "S", "", 0, 0});
  p.enc.onTimeAdvance(5000);
  p.pump();

  LiveModel late;
  LiveKeyframe kf = p.enc.beginKeyframe();
  EXPECT_EQ(kf.seq(), p.enc.lastSeq());
  std::string chunk;
  bool first = true;
  size_t pieces = 0;
  while (kf.next(chunk)) {
    EXPECT_LE(chunk.size(), model::live_wire::kMaxEventBytes);
    ASSERT_TRUE(late.apply(bytesOf(chunk), first).ok());
    first = false;
    ++pieces;
  }
  EXPECT_GE(pieces, 2u);  // definitions, then the retained batches

  auto a = p.model.snapshot();
  auto b = late.snapshot();
  EXPECT_EQ(a->threads.size(), b->threads.size());
  EXPECT_EQ(a->nodes.size(), b->nodes.size());
  EXPECT_EQ(a->frames.size(), b->frames.size());
  EXPECT_EQ(a->strings.size(), b->strings.size());
  EXPECT_EQ(a->modules.size(), b->modules.size());
  EXPECT_EQ(a->scopeEvents.size(), b->scopeEvents.size());
  ASSERT_EQ(a->samplesByThread.size(), b->samplesByThread.size());
  for (size_t t = 0; t < a->samplesByThread.size(); ++t) {
    EXPECT_EQ(a->samplesByThread[t].ts, b->samplesByThread[t].ts);
    EXPECT_EQ(a->samplesByThread[t].callstack, b->samplesByThread[t].callstack);
  }
  EXPECT_EQ(a->header.end_ts, b->header.end_ts);
  EXPECT_EQ(a->header.clock_frequency_hz, b->header.clock_frequency_hz);

  // And the late subscriber continues with the next batch.
  p.enc.onSample(6000, 100, 101, 0, 0, std::vector<uint64_t>{0x400010});
  p.enc.flush();
  LiveBatch next;
  ASSERT_EQ(p.enc.waitBatch(kf.seq() + 1, next, 0ms), LiveWait::Ready);
  ASSERT_TRUE(late.apply(bytesOf(*next.data), false).ok());
  EXPECT_EQ(late.snapshot()->samplesByThread[0].ts.back(), 6000u);
}

TEST(LiveKeyframe, KeyframeResetsAModelThatHadOtherContent) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "main"));
  p.enc.onSample(2000, 100, 101, 0, 0, std::vector<uint64_t>{0x400010});
  p.pump();
  LiveModel m;
  // Old content that the keyframe must replace.
  Pipe other;
  sessionAndTarget(other.enc, 555);
  other.enc.onThread(thread(555, 556, "other"));
  other.enc.onSample(1, 555, 556, 0, 0, std::vector<uint64_t>{0x400010});
  other.pump();
  ASSERT_TRUE(m.apply(bytesOf(*[&] {
                LiveBatch b;
                other.enc.waitBatch(1, b, 0ms);
                return b.data;
              }()),
                      true)
                  .ok());
  EXPECT_EQ(m.snapshot()->processes[0].pid, 555u);

  LiveKeyframe kf = p.enc.beginKeyframe();
  std::string chunk;
  bool first = true;
  while (kf.next(chunk)) {
    ASSERT_TRUE(m.apply(bytesOf(chunk), first).ok());
    first = false;
  }
  auto snap = m.snapshot();
  ASSERT_EQ(snap->processes.size(), 1u);
  EXPECT_EQ(snap->processes[0].pid, 100u);
  EXPECT_EQ(snap->threads.size(), 1u);
}

TEST(LiveRetention, OldBatchesBecomeAGap) {
  LiveEncoderConfig c = testConfig();
  c.maxRetainedBytes = 4096;
  LiveEncoder enc(c);
  sessionAndTarget(enc);
  enc.onThread(thread(100, 101, "main"));
  for (int b = 0; b < 20; ++b) {
    for (int i = 0; i < 100; ++i)
      enc.onSample(2000 + b * 1000 + i, 100, 101, 0, 0, std::vector<uint64_t>{0x400010 + static_cast<uint64_t>(i)});
    enc.flush();
  }
  EXPECT_EQ(enc.lastSeq(), 20u);
  EXPECT_FALSE(enc.retains(1));
  EXPECT_TRUE(enc.retains(enc.lastSeq()));
  EXPECT_TRUE(enc.retains(enc.lastSeq() + 1));
  EXPECT_FALSE(enc.retains(enc.lastSeq() + 2));
  LiveBatch out;
  EXPECT_EQ(enc.waitBatch(1, out, 0ms), LiveWait::Gap);
  EXPECT_EQ(enc.waitBatch(21, out, 0ms), LiveWait::Timeout);
  enc.finish();
  EXPECT_EQ(enc.waitBatch(21, out, 0ms), LiveWait::Ended);
  EXPECT_TRUE(enc.ended());
  enc.onSample(1, 100, 101, 0, 0, {});  // ignored after the end
  EXPECT_EQ(enc.lastSeq(), 20u);
}

TEST(LiveRetention, QuietCaptureStillProducesHeartbeats) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.pump();
  const uint64_t before = p.enc.lastSeq();
  p.enc.onTimeAdvance(2'000'000'000ull);
  EXPECT_EQ(p.enc.flush(), before + 1);
  EXPECT_EQ(p.enc.flush(), 0u);  // nothing new: no batch
  p.enc.onTimeAdvance(3'000'000'000ull);
  EXPECT_EQ(p.enc.flush(), before + 2);
}

TEST(LiveCaps, OverflowIdentitiesAreCountedAndTheModelStaysValid) {
  LiveEncoderConfig c = testConfig();
  c.maxStrings = 8;
  c.maxThreads = 4;
  c.maxFrames = 6;
  c.maxNodes = 10;
  c.maxModules = 1;
  Pipe p(c);
  sessionAndTarget(p.enc);
  for (uint32_t t = 1; t <= 10; ++t) p.enc.onThread(thread(100, t, ("thr" + std::to_string(t)).c_str()));
  for (uint32_t i = 0; i < 20; ++i)
    p.enc.onSample(2000 + i, 100, 1 + i % 10, 0, 0, std::vector<uint64_t>{0x900000 + i, 0x900100 + i});
  p.enc.onScopeEvent({2500, 100, 1, trace::kScopeBegin, "A fresh scope name", "more data", 0, 0});
  p.pump();
  const LiveEncoderStats st = p.enc.stats();
  EXPECT_GT(st.overflows, 0u);
  EXPECT_LE(st.nodes, 10u);
  EXPECT_LE(st.frames, 6u);
  EXPECT_LE(st.threads, 4u);
  auto m = p.model.snapshot();
  // Every sample's callstack is a valid node, every node's frame valid, all strings resolvable.
  for (const auto& cols : m->samplesByThread)
    for (auto cs : cols.callstack) EXPECT_TRUE(!cs.valid() || cs.get() < m->nodes.size());
  for (const auto& n : m->nodes) EXPECT_TRUE(!n.frame.valid() || n.frame.get() < m->frames.size());
  EXPECT_EQ(m->strings.get(StringIdx(model::live_wire::kOverflowString)), model::live_wire::kOverflowName);
}

TEST(LiveModelWire, UnknownRecordsAreSkippedAndGarbageIsRejected) {
  LiveModel m;
  std::string batch;
  model::live_wire::putRecord(batch, static_cast<model::live_wire::RecordType>(999), "abcd", 4);
  const uint64_t ts = 42;
  model::live_wire::putRecord(batch, model::live_wire::RecordType::TimeAdvance, &ts, 8);
  ASSERT_TRUE(m.apply(bytesOf(batch), true).ok());
  EXPECT_EQ(m.stats().unknownRecords, 1u);
  EXPECT_EQ(m.stats().nowTs, 42u);

  std::string torn = batch.substr(0, batch.size() - 3);
  EXPECT_FALSE(m.apply(bytesOf(torn), false).ok());
  std::string badSamples;
  const uint32_t count = 1000;
  model::live_wire::putRecord(badSamples, model::live_wire::RecordType::Samples, &count, 4);
  EXPECT_FALSE(m.apply(bytesOf(badSamples), false).ok());
  // A node whose parent does not precede it.
  std::string badNode;
  trace::CallstackNodeRec nrec{CallstackIdx(5), FrameIdx(0)};
  model::live_wire::putIndexedRecord(badNode, model::live_wire::RecordType::NodeDef, 2, &nrec, sizeof(nrec));
  EXPECT_FALSE(m.apply(bytesOf(badNode), false).ok());
  // The model is still usable.
  EXPECT_NE(m.snapshot(), nullptr);
}

TEST(LiveSplit, SplitRecordsCutsOnlyAtRecordBoundaries) {
  std::string blob;
  for (int i = 0; i < 10; ++i) {
    const std::string payload(100, 'x');
    model::live_wire::putRecord(blob, model::live_wire::RecordType::StringDef, payload.data(), payload.size());
  }
  const auto pieces = splitRecords(blob, 350);  // 108 bytes per record: three per piece
  ASSERT_EQ(pieces.size(), 4u);
  size_t total = 0;
  for (auto p : pieces) {
    EXPECT_EQ(p.size() % 108, 0u);
    EXPECT_LE(p.size(), 350u);
    total += p.size();
  }
  EXPECT_EQ(total, blob.size());
  EXPECT_EQ(splitRecords(blob, 10).size(), 10u);  // a record larger than the limit is one piece
  EXPECT_TRUE(splitRecords("", 10).empty());
}

// The snapshot is a model the analysis code accepts, built the way the viewer builds one for a file.
TEST(LiveSnapshot, AnalysisAcceptsTheSnapshot) {
  Pipe p;
  sessionAndTarget(p.enc);
  p.enc.onThread(thread(100, 101, "main"));
  p.enc.onThread(thread(100, 102, "worker"));
  for (int i = 0; i < 400; ++i) {
    const uint32_t tid = 101 + i % 2;
    p.enc.onSample(2000 + i * 100, 100, tid, tid - 101, 0,
                   std::vector<uint64_t>{0x400010 + static_cast<uint64_t>(i % 5) * 0x10, 0x400200, 0x400300});
  }
  for (int i = 0; i < 20; ++i) {
    capture::LiveContextSwitch cs;
    cs.ts = 2000 + i * 1000 + 50;
    cs.cpu = 0;
    cs.oldPid = 100;
    cs.oldTid = 101;
    cs.newPid = 100;
    cs.newTid = 102;
    cs.oldState = 5;
    cs.oldStack = std::vector<uint64_t>{0x400010, 0x400200};
    p.enc.onContextSwitch(cs);
    cs.ts += 400;
    cs.oldTid = 102;
    cs.newTid = 101;
    cs.oldStack = {};
    p.enc.onContextSwitch(cs);
  }
  p.enc.onScopeEvent({2200, 100, 101, trace::kScopeBegin, "Outer", "", 0, 0x400215});
  p.enc.onScopeEvent({2300, 100, 101, trace::kScopeBegin, "Inner", "", 0, 0x400215});
  p.enc.onScopeEvent({2400, 100, 101, trace::kScopeEnd, "", "", 0, 0});
  p.enc.onScopeEvent({2500, 100, 101, trace::kScopeEnd, "", "", 0, 0});
  p.enc.onTimeAdvance(60000);
  p.pump();

  auto snap = p.model.snapshot();
  TraceModel m = *snap;  // the viewer works on its own copy of the snapshot's contents
  ASSERT_TRUE(analysis::buildThreadStates(m).ok());
  size_t intervals = 0;
  for (const auto& v : m.statesByThread) intervals += v.size();
  EXPECT_GT(intervals, 0u);

  auto paired = analysis::pairScopes(m);
  ASSERT_TRUE(paired.ok());
  EXPECT_EQ(paired->byThread[0].size(), 2u);

  symbols::SymbolService svc(std::vector<std::string>{}, std::filesystem::path{});
  analysis::QueryContext ctx(m, svc);
  auto rows = analysis::bottomUpFunctions(ctx, analysis::Filter{});
  ASSERT_TRUE(rows.ok());
  EXPECT_FALSE(rows->empty());
  auto tree = analysis::topDownTree(ctx, analysis::Filter{});
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(tree->sampleCount, 400u);

  auto lod = analysis::TimelineLod::prepare(m);
  ASSERT_TRUE(lod.ok());
  EXPECT_TRUE(lod->build().ok());
}

}  // namespace
}  // namespace tachyon::control
