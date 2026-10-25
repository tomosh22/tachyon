#include "inst/decoder.h"

#include "../control/test_util.h"
#include "packed.h"
#include "wire_builder.h"

#include <gtest/gtest.h>

#include <cstring>

#include <algorithm>
#include <fstream>

namespace tachyon::inst {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using testing_util::pv;
using testing_util::StreamBuilder;

DecodedInstrumentation decodeBytes(const StreamBuilder& b, DecoderConfig config = {}) {
  InstDecoder decoder(std::move(config));
  EXPECT_TRUE(decoder.addBytes(b.bytes(), "test").ok());
  return decoder.finish();
}

bool hasWarning(const DecodedInstrumentation& d, const std::string& needle) {
  return std::any_of(d.warnings.begin(), d.warnings.end(),
                     [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

TEST(Decoder, ScopesGetTheirStringsInlineDataColorAndCaller) {
  StreamBuilder b;
  b.header(77, 1'000'000'000, 5);
  const uint64_t outer = b.string("outer");
  const uint64_t inner = b.string("inner");
  b.scopeBegin(100, 1, outer, "", 0xAABBCCFF, 0x1111);
  b.scopeBegin(110, 1, inner, "payload", 0x01020304, 0x2222);
  b.scopeEnd(120, 1);
  b.scopeEnd(130, 1);
  const DecodedInstrumentation d = decodeBytes(b);
  ASSERT_EQ(d.scopes.size(), 4u);
  const trace::ScopeEventRec b0 = d.scopes[0], b1 = d.scopes[1], e0 = d.scopes[2], e1 = d.scopes[3];
  EXPECT_EQ(b0.kind, trace::kScopeBegin);
  EXPECT_EQ(d.strings.get(b0.id), "outer");
  EXPECT_EQ(pv(b0.data).get(), 0u);  // no data: the empty string
  EXPECT_EQ(pv(b0.color_rgba), 0xAABBCCFFu);
  EXPECT_EQ(pv(b0.caller_addr), 0x1111u);
  EXPECT_EQ(d.strings.get(b1.id), "inner");
  EXPECT_EQ(d.strings.get(b1.data), "payload");
  EXPECT_EQ(pv(b1.caller_addr), 0x2222u);
  EXPECT_EQ(e0.kind, trace::kScopeEnd);
  EXPECT_EQ(pv(e0.id).get(), 0u);
  EXPECT_EQ(pv(e0.data).get(), 0u);
  EXPECT_EQ(pv(e1.ts), 130u);
  EXPECT_EQ(d.stats.unmatchedScopeEnds, 0u);
  EXPECT_EQ(d.stats.unclosedScopes, 0u);
  EXPECT_TRUE(d.warnings.empty());
  ASSERT_EQ(d.streams.size(), 1u);
  EXPECT_EQ(d.streams[0].pid, 77u);
  EXPECT_EQ(d.streams[0].processStartTs, 5u);
  EXPECT_EQ(d.streams[0].clockMapper, "identity");
  EXPECT_EQ(d.minTs, 100u);
  EXPECT_EQ(d.maxTs, 130u);
}

TEST(Decoder, LockWaitsKeepTheSentinelColorAndHexData) {
  StreamBuilder b;
  b.header();
  const uint64_t name = b.string("Tachyon.LockAcquire");
  b.scopeBegin(10, 3, name, "0x7ffd1234abcd", 0x4C4F43FF);
  b.scopeEnd(20, 3);
  const DecodedInstrumentation d = decodeBytes(b);
  const trace::ScopeEventRec begin = d.scopes[0];
  EXPECT_EQ(pv(begin.color_rgba), 0x4C4F43FFu);
  EXPECT_EQ(d.strings.get(begin.data), "0x7ffd1234abcd");
}

TEST(Decoder, RecordsAreSortedByTimestampAndStableWithinAThread) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  // Frames of different threads interleave at drain granularity: thread 2's batch arrives after thread 1's.
  b.scopeBegin(100, 1, k);
  b.scopeEnd(100, 1);  // same timestamp: order must stay Begin, End
  b.scopeBegin(100, 1, k);
  b.scopeEnd(300, 1);
  b.scopeBegin(150, 2, k);
  b.scopeEnd(250, 2);
  const DecodedInstrumentation d = decodeBytes(b);
  ASSERT_EQ(d.scopes.size(), 6u);
  uint64_t last = 0;
  for (const auto& rec : d.scopes) {
    const uint64_t ts = rec.ts;
    EXPECT_GE(ts, last);
    last = ts;
  }
  // Thread 1's events at ts 100: Begin, End, Begin.
  std::vector<uint8_t> kinds;
  for (const auto& rec : d.scopes) {
    const trace::ScopeEventRec r = rec;
    if (r.tid == 1 && r.ts == 100) kinds.push_back(r.kind);
  }
  EXPECT_EQ(kinds, (std::vector<uint8_t>{trace::kScopeBegin, trace::kScopeEnd, trace::kScopeBegin}));
}

TEST(Decoder, TimestampsGoingBackwardsOnAThreadAreClamped) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(500, 1, k);
  b.scopeEnd(400, 1);  // earlier than its Begin
  b.scopeBegin(450, 2, k);
  b.scopeEnd(460, 2);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.clampedTimestamps, 1u);
  EXPECT_TRUE(hasWarning(d, "went backwards"));
  // Thread 1 stays Begin then End after the sort.
  std::vector<uint8_t> kinds;
  for (const auto& rec : d.scopes) {
    const trace::ScopeEventRec r = rec;
    if (r.tid == 1) kinds.push_back(r.kind);
  }
  EXPECT_EQ(kinds, (std::vector<uint8_t>{trace::kScopeBegin, trace::kScopeEnd}));
}

TEST(Decoder, FrameMarksMarkersPlotsAndThreadNames) {
  StreamBuilder b;
  b.header();
  const uint64_t frame = b.string("Frame");
  const uint64_t mark = b.string("spawned");
  const uint64_t plot = b.string("fps");
  b.threadName(5, 9, "render");
  b.frameMark(10, 9, frame, 41);
  b.marker(20, 9, mark, "boss", 0xFF0000FF);
  b.plot(30, plot, 59.5);
  b.plot(25, plot, 60.0);
  const DecodedInstrumentation d = decodeBytes(b);
  ASSERT_EQ(d.frameMarks.size(), 1u);
  const trace::FrameMarkRec fm = d.frameMarks[0];
  EXPECT_EQ(d.strings.get(fm.name), "Frame");
  EXPECT_EQ(pv(fm.frame_number), 41u);
  ASSERT_EQ(d.markers.size(), 1u);
  const trace::MarkerRec mk = d.markers[0];
  EXPECT_EQ(d.strings.get(mk.name), "spawned");
  EXPECT_EQ(d.strings.get(mk.data), "boss");
  EXPECT_EQ(pv(mk.color_rgba), 0xFF0000FFu);
  ASSERT_EQ(d.plots.size(), 2u);
  const trace::PlotSampleRec p0 = d.plots[0], p1 = d.plots[1];
  EXPECT_EQ(pv(p0.ts), 25u);  // sorted
  EXPECT_DOUBLE_EQ(pv(p0.value), 60.0);
  EXPECT_DOUBLE_EQ(pv(p1.value), 59.5);
  ASSERT_EQ(d.threadNames.size(), 1u);
  const trace::ThreadNameRec tn = d.threadNames[0];
  EXPECT_EQ(pv(tn.tid), 9u);
  EXPECT_EQ(d.strings.get(tn.name), "render");
}

TEST(Decoder, DroppedFramesBecomeMarkers) {
  StreamBuilder b;
  b.header();
  b.dropped(1000, 4, 123);
  const DecodedInstrumentation d = decodeBytes(b);
  ASSERT_EQ(d.markers.size(), 1u);
  const trace::MarkerRec m = d.markers[0];
  EXPECT_EQ(d.strings.get(m.name), kDroppedMarkerName);
  EXPECT_EQ(d.strings.get(m.data), "123");
  EXPECT_EQ(pv(m.tid), 4u);
  EXPECT_EQ(pv(m.ts), 1000u);
  EXPECT_EQ(pv(m.color_rgba), kDroppedMarkerColor);
  EXPECT_EQ(d.stats.droppedRecords, 123u);
  EXPECT_TRUE(hasWarning(d, "123 instrumentation records were dropped"));
}

TEST(Decoder, StreamSwitchResendsAreDeduplicatedAndKeysAreScopedToTheirStream) {
  StreamBuilder b;
  b.header(10);
  const uint64_t k = b.string("shared");
  b.threadName(5, 1, "worker");
  b.scopeBegin(100, 1, k);
  b.scopeEnd(110, 1);
  // The agent connection was lost and the SDK reconnected: new header, every StringDef and ThreadName again.
  b.header(10);
  b.stringAs(k, "shared");
  const uint64_t other = 0xDEAD0000ull;
  b.stringAs(other, "second-stream-only");
  b.threadName(5, 1, "worker");  // identical re-send
  b.threadName(200, 1, "worker2");
  b.scopeBegin(300, 1, k);
  b.scopeBegin(310, 1, other);
  b.scopeEnd(320, 1);
  b.scopeEnd(330, 1);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.streams, 2u);
  EXPECT_EQ(d.stats.threadNames, 2u);
  EXPECT_EQ(d.stats.duplicateThreadNames, 1u);
  EXPECT_EQ(d.scopes.size(), 6u);
  EXPECT_EQ(d.stats.unmatchedScopeEnds, 0u);
  EXPECT_EQ(d.stats.unclosedScopes, 0u);
  const trace::ScopeEventRec last = d.scopes[4];
  EXPECT_EQ(d.strings.get(last.id), "");  // an End
  // A key defined only in stream 2 means that string there.
  bool found = false;
  for (const auto& rec : d.scopes) {
    const trace::ScopeEventRec r = rec;
    if (r.kind == trace::kScopeBegin && d.strings.get(r.id) == "second-stream-only") found = true;
  }
  EXPECT_TRUE(found);
}

TEST(Decoder, ScopeOpenedInOneStreamMayCloseInTheNext) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("long");
  b.scopeBegin(100, 1, k);
  b.header();  // stream switch
  b.stringAs(k, "long");
  b.scopeEnd(200, 1);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.unmatchedScopeEnds, 0u);
  EXPECT_EQ(d.stats.unclosedScopes, 0u);
}

TEST(Decoder, UnresolvedStringKeyIsReportedNotFatal) {
  StreamBuilder b;
  b.header();
  b.scopeBegin(10, 1, 0xBAD);
  b.scopeEnd(20, 1);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.unresolvedStrings, 1u);
  EXPECT_EQ(d.scopes.size(), 2u);
  const trace::ScopeEventRec r = d.scopes[0];
  EXPECT_EQ(d.strings.get(r.id), "<unresolved string>");
  EXPECT_TRUE(hasWarning(d, "no StringDef"));
}

TEST(Decoder, BalanceIsCountedPerThreadInWireOrder) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeEnd(5, 1);  // nothing open
  b.scopeBegin(10, 1, k);
  b.scopeBegin(11, 1, k);
  b.scopeEnd(12, 1);
  b.scopeBegin(20, 2, k);  // thread 2 never closes
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.unmatchedScopeEnds, 1u);
  EXPECT_EQ(d.stats.unclosedScopes, 2u);  // one on thread 1, one on thread 2
  EXPECT_TRUE(hasWarning(d, "unbalanced"));
}

TEST(Decoder, FiberSwitchesKeepEachFibersScopesSeparate) {
  using K = FiberEventKind;
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("work");
  b.fiber(1, 1, K::Register, 100);
  b.fiber(1, 1, K::Register, 1);
  b.fiber(1, 1, K::Register, 2);
  b.scopeBegin(10, 1, k);                  // opened on the thread's own context (fiber 100)
  b.fiber(11, 1, K::SwitchBegin, 100, 1);  // leave 100 for fiber 1
  b.fiber(12, 1, K::SwitchEnd, 1);
  b.scopeBegin(13, 1, k);
  b.scopeEnd(14, 1);
  b.fiber(15, 1, K::SwitchBegin, 1, 100);  // back to 100
  b.fiber(16, 1, K::SwitchEnd, 100);
  b.scopeEnd(17, 1);  // closes the scope opened before the switch
  b.fiber(18, 1, K::Unregister, 1);
  b.fiber(18, 1, K::Unregister, 2);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.unmatchedScopeEnds, 0u);
  EXPECT_EQ(d.stats.unclosedScopes, 0u);
  EXPECT_EQ(d.fibers.size(), 9u);
  const trace::FiberEventRec f = d.fibers[3];
  EXPECT_EQ(pv(f.fiber_id), 100u);
  EXPECT_EQ(pv(f.other_fiber_id), 1u);
  EXPECT_EQ(f.kind, trace::kFiberSwitchBegin);
}

TEST(Decoder, ScopeLeftOpenInAFiberIsCountedAsUnclosed) {
  using K = FiberEventKind;
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("work");
  b.fiber(1, 1, K::SwitchEnd, 5);
  b.scopeBegin(10, 1, k);
  b.fiber(20, 1, K::SwitchBegin, 5, 6);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.unclosedScopes, 1u);
}

TEST(Decoder, TruncatedTailKeepsEarlierFramesAndWarns) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(10, 1, k);
  b.scopeEnd(20, 1);
  std::vector<uint8_t> bytes = b.bytes();
  bytes.resize(bytes.size() - 5);  // the last frame loses its tail
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addBytes(bytes, "cut").ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.scopes.size(), 1u);
  EXPECT_EQ(d.stats.truncatedTailBytes, sizeof(FrameHeader) + sizeof(ScopeEndPayload) - 5);
  EXPECT_TRUE(hasWarning(d, "truncated"));
}

TEST(Decoder, TruncationInsideTheHeaderBytesOfAFrame) {
  StreamBuilder b;
  b.header();
  b.scopeEnd(20, 1);
  std::vector<uint8_t> bytes = b.bytes();
  bytes.resize(sizeof(FrameHeader) + sizeof(StreamHeaderPayload) + 3);
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addBytes(bytes, "cut").ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_TRUE(d.empty());
  EXPECT_EQ(d.stats.truncatedTailBytes, 3u);
}

TEST(Decoder, UnknownFramesAreSkippedByLength) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  const uint8_t junk[40] = {1, 2, 3};
  b.frameRaw(77, junk, sizeof(junk));
  b.scopeBegin(10, 1, k);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.unknownFrames, 1u);
  EXPECT_EQ(d.scopes.size(), 1u);
  EXPECT_TRUE(hasWarning(d, "unknown type"));
}

TEST(Decoder, AnOversizedFrameIsSkippedAndTheStreamGoesOn) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(10, 1, k);
  const std::vector<uint8_t> big(3u << 20, 7);
  b.frameRaw(99, big.data(), big.size());
  b.scopeEnd(20, 1);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.scopes.size(), 2u);
  EXPECT_EQ(d.stats.unknownFrames, 1u);
  EXPECT_EQ(d.stats.corruptBytes, 0u);
  EXPECT_EQ(d.stats.unclosedScopes, 0u);
}

TEST(Decoder, MalformedFramesAreSkipped) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  const uint8_t shortPayload[10] = {};
  b.frame(FrameType::ScopeBegin, shortPayload, sizeof(shortPayload));
  // data_len claims more bytes than the frame holds.
  ScopeBeginPayload p{};
  p.ts = 1;
  p.tid = 1;
  p.id_key = k;
  p.data_len = 500;
  b.frame(FrameType::ScopeBegin, &p, sizeof(p));
  b.scopeBegin(30, 1, k);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_EQ(d.stats.malformedFrames, 2u);
  EXPECT_EQ(d.scopes.size(), 1u);
}

TEST(Decoder, FileThatIsNotAStreamIsRejected) {
  TempDir dir;
  const fs::path path = dir.path() / "junk.tachyon-inst";
  {
    std::ofstream out(path, std::ios::binary);
    out << "this is definitely not a wire stream, just text";
  }
  InstDecoder decoder;
  const auto r = decoder.addFile(path);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::CorruptData);
}

TEST(Decoder, StreamHeaderWithWrongMagicIsRejected) {
  StreamBuilder b;
  StreamHeaderPayload sh{};
  sh.magic = 0x12345678;
  b.frame(FrameType::StreamHeader, &sh, sizeof(sh));
  InstDecoder decoder;
  EXPECT_FALSE(decoder.addBytes(b.bytes(), "bad").ok());
}

TEST(Decoder, MissingFileIsNotFound) {
  InstDecoder decoder;
  const auto r = decoder.addFile("/nonexistent/dir/nothing.tachyon-inst");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(Decoder, EmptyFileIsAcceptedWithANote) {
  TempDir dir;
  const fs::path path = dir.path() / "empty.tachyon-inst";
  std::ofstream(path).close();
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(path).ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_TRUE(d.empty());
  EXPECT_TRUE(hasWarning(d, "empty file"));
}

TEST(Decoder, UnsupportedWireVersionIsSkippedWithANote) {
  StreamBuilder b;
  b.header(1, 1'000'000'000, 1, /*wireVersion=*/99);
  b.scopeEnd(1, 1);
  const DecodedInstrumentation d = decodeBytes(b);
  EXPECT_TRUE(d.empty());
  EXPECT_TRUE(hasWarning(d, "unsupported wire version 99"));
}

TEST(Decoder, ImplausibleLengthMidStreamStopsTheSourceButKeepsWhatCameBefore) {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(10, 1, k);
  std::vector<uint8_t> bytes = b.bytes();
  FrameHeader bad{3, 0x7FFFFFFF};
  const size_t at = bytes.size();
  bytes.resize(at + sizeof(bad) + 20);
  std::memcpy(bytes.data() + at, &bad, sizeof(bad));
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addBytes(bytes, "corrupt").ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.scopes.size(), 1u);
  EXPECT_GT(d.stats.corruptBytes, 0u);
  EXPECT_TRUE(hasWarning(d, "corrupt"));
}

TEST(Decoder, SnapshotOfAGrowingFileReadsOnlyTheRequestedPrefix) {
  TempDir dir;
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(10, 1, k);
  const size_t boundary = b.bytes().size();
  b.scopeEnd(20, 1);
  const fs::path path = b.write(dir.path() / "grow.tachyon-inst");
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(path, boundary).ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.scopes.size(), 1u);
  EXPECT_EQ(d.stats.truncatedTailBytes, 0u);
}

TEST(Decoder, ClockMapperFactoryOverridesTheDefault) {
  struct Plus1000 final : IClockMapper {
    uint64_t toTraceTicks(uint64_t t) const override { return t + 1000; }
    std::string describe() const override { return "plus1000"; }
  };
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(10, 1, k);
  b.plot(20, k, 1.0);
  DecoderConfig config;
  config.clockMapperFactory = [](const StreamInfo&) { return std::make_unique<Plus1000>(); };
  const DecodedInstrumentation d = decodeBytes(b, config);
  const trace::ScopeEventRec s = d.scopes[0];
  EXPECT_EQ(pv(s.ts), 1010u);
  const trace::PlotSampleRec p = d.plots[0];
  EXPECT_EQ(pv(p.ts), 1020u);
  EXPECT_EQ(d.streams[0].clockMapper, "plus1000");
  EXPECT_EQ(d.minTs, 1010u);
}

TEST(Decoder, StreamInAnotherClockIsScaledToTheTraceClock) {
  StreamBuilder b;
  b.header(1, 1'000'000'000);  // ns
  const uint64_t k = b.string("s");
  b.scopeBegin(1'000'000'000, 1, k);
  DecoderConfig config;
  config.traceClockHz = 10'000'000;  // QPC
  const DecodedInstrumentation d = decodeBytes(b, config);
  const trace::ScopeEventRec s = d.scopes[0];
  EXPECT_EQ(pv(s.ts), 10'000'000u);
  EXPECT_TRUE(hasWarning(d, "scaled by the frequency ratio"));
}

TEST(Decoder, StreamInAnotherClockUsesTheTracesCalibration) {
  StreamBuilder b;
  b.header(1, 3'000'000'000ull);  // a TSC
  const uint64_t k = b.string("s");
  b.scopeBegin(3000, 1, k);
  DecoderConfig config;
  config.traceClockHz = 1'000'000'000;
  trace::ClockCalibrationRec a{}, c{};
  a.trace_ts = 100;
  a.other_clock_ts = 300;
  a.other_clock_kind = trace::kClockTsc;
  c.trace_ts = 1100;
  c.other_clock_ts = 3300;
  c.other_clock_kind = trace::kClockTsc;
  config.calibrations = {a, c};
  const DecodedInstrumentation d = decodeBytes(b, config);
  const trace::ScopeEventRec s = d.scopes[0];
  EXPECT_EQ(pv(s.ts), 1000u);
  EXPECT_FALSE(hasWarning(d, "scaled"));
}

TEST(Decoder, SeveralSourcesAreCombinedInOrder) {
  TempDir dir;
  StreamBuilder a, c;
  a.header(1);
  const uint64_t ka = a.string("from-a");
  a.scopeBegin(100, 1, ka);
  a.scopeEnd(300, 1);
  c.header(2);
  const uint64_t kc = c.string("from-c");
  c.scopeBegin(200, 2, kc);
  c.scopeEnd(250, 2);
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(a.write(dir.path() / "a.tachyon-inst")).ok());
  ASSERT_TRUE(decoder.addFile(c.write(dir.path() / "c.tachyon-inst")).ok());
  const DecodedInstrumentation d = decoder.finish();
  EXPECT_EQ(d.stats.sources, 2u);
  EXPECT_EQ(d.stats.streams, 2u);
  ASSERT_EQ(d.scopes.size(), 4u);
  const uint32_t order[4] = {1, 2, 2, 1};
  for (size_t i = 0; i < 4; ++i) {
    const trace::ScopeEventRec r = d.scopes[i];
    EXPECT_EQ(pv(r.tid), order[i]);
  }
}

TEST(Decoder, EqualStringsShareOneLocalIndex) {
  StreamBuilder b;
  b.header();
  const uint64_t k1 = b.string("same");
  const uint64_t k2 = b.string("same");  // another pointer, same text
  b.scopeBegin(1, 1, k1, "d");
  b.scopeBegin(2, 1, k2, "d");
  const DecodedInstrumentation d = decodeBytes(b);
  const trace::ScopeEventRec a = d.scopes[0], c = d.scopes[1];
  EXPECT_EQ(pv(a.id).get(), pv(c.id).get());
  EXPECT_EQ(pv(a.data).get(), pv(c.data).get());
}

}  // namespace
}  // namespace tachyon::inst
