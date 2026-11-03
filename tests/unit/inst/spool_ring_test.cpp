// WP-60: the ring mode of the spool (flight recorder) and the merge of a rotated spool.
#include "inst/spool.h"

#include "../control/test_util.h"
#include "inst/decoder.h"
#include "inst/merge.h"
#include "packed.h"
#include "trace/trace_reader.h"
#include "trace_builder.h"
#include "wire_builder.h"

#include <gtest/gtest.h>

#include <set>

namespace tachyon::inst {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using testing_util::StreamBuilder;

constexpr uint32_t kTid = 7;

// One frame at a time into the spool, as the ingest does.
void feed(Spool& spool, const StreamBuilder& b) {
  FrameAssembler a;
  a.feed(b.bytes().data(), b.bytes().size(), [&](const uint8_t* f, size_t n) { spool.appendFrame(f, n); });
}

uint64_t directoryBytes(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec))
    if (entry.is_regular_file(ec)) total += entry.file_size(ec);
  return total;
}

size_t fileCount(const fs::path& dir) {
  size_t n = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) n += entry.is_regular_file(ec) ? 1 : 0;
  return n;
}

// A header, the strings, a thread name, then `pairs` Begin/End pairs with timestamps from `firstTs` in steps
// of 10, as one connection would send them. Returns the key of the scope name.
struct Feeder {
  uint64_t ts = 1000;
  uint64_t key = 0;
  void start(Spool& spool) {
    StreamBuilder head;
    head.header();
    key = head.string("ring_scope");
    head.threadName(ts, kTid, "ring-thread");
    feed(spool, head);
  }
  void pairs(Spool& spool, uint64_t count) {
    StreamBuilder b;
    for (uint64_t i = 0; i < count; ++i) {
      b.scopeBegin(ts, kTid, key, "payload-payload-payload");
      b.scopeEnd(ts + 5, kTid);
      ts += 10;
    }
    feed(spool, b);
  }
};

TEST(SpoolRing, KeepsTheNewestDataWithinTheBoundAndDropsTheOldest) {
  TempDir dir;
  SpoolOptions options;
  options.ringBytes = 1 << 20;
  options.segmentBytes = 128 << 10;
  auto budget = std::make_shared<SpoolBudget>(1ull << 30);
  auto spool = Spool::create(dir.path() / "inst-1.tachyon-inst", 0, budget, options);
  ASSERT_TRUE(spool.ok());
  EXPECT_TRUE((*spool)->ringMode());

  Feeder f;
  f.start(**spool);
  for (int round = 0; round < 60; ++round) f.pairs(**spool, 2000);  // about 10 MiB in total
  const uint64_t lastTs = f.ts - 10;

  const SpoolStats mid = (*spool)->stats();
  EXPECT_FALSE(mid.overflowed);
  EXPECT_EQ(mid.framesDropped, 0u) << "a ring never refuses a frame because of its size";
  EXPECT_GT(mid.segmentsDiscarded, 0u);
  EXPECT_LE(mid.retainedBytes, options.ringBytes);
  EXPECT_LE(budget->used(), options.ringBytes) << "discarded segments give their bytes back";

  ASSERT_TRUE((*spool)->close().ok());
  // The disk holds no more than the bound, in a bounded number of files.
  EXPECT_LE(directoryBytes(dir.path()), options.ringBytes);
  EXPECT_LE(fileCount(dir.path()), 9u);
  const std::vector<SpoolSegment> segments = (*spool)->segments();
  ASSERT_GE(segments.size(), 2u);
  for (const SpoolSegment& seg : segments) EXPECT_TRUE(fs::exists(seg.file)) << seg.file;

  // Decoding the retained segments in order gives the newest records, every string resolved, the thread
  // named, and nothing out of order.
  InstDecoder decoder;
  for (const SpoolSegment& seg : segments) ASSERT_TRUE(decoder.addFile(seg.file, seg.bytes).ok());
  const DecodedInstrumentation data = decoder.finish();
  EXPECT_EQ(data.stats.unresolvedStrings, 0u);
  EXPECT_EQ(data.stats.malformedFrames, 0u);
  EXPECT_EQ(data.stats.streams, segments.size());  // every segment starts with its own StreamHeader
  ASSERT_FALSE(data.scopes.empty());
  EXPECT_EQ(trace::ScopeEventRec(data.scopes.back()).ts, lastTs + 5);
  EXPECT_GT(trace::ScopeEventRec(data.scopes.front()).ts, 1000u + 10 * 20000u)
      << "the oldest records are gone";
  EXPECT_EQ(data.strings.get(trace::ScopeEventRec(data.scopes.front()).id), "ring_scope");
  ASSERT_FALSE(data.threadNames.empty());
  EXPECT_EQ(data.strings.get(trace::ThreadNameRec(data.threadNames.front()).name), "ring-thread");
  const ScopeBalance balance = checkScopeBalance(data.scopes);
  EXPECT_EQ(balance.outOfOrder, 0u);
  EXPECT_LE(balance.unmatchedEnds, 1u);  // only the cut at the start of the oldest segment
}

TEST(SpoolRing, SmallStreamStaysInOneSegment) {
  TempDir dir;
  SpoolOptions options;
  options.ringBytes = 8 << 20;
  auto spool = Spool::create(dir.path() / "inst-2.tachyon-inst", 0, nullptr, options);
  ASSERT_TRUE(spool.ok());
  Feeder f;
  f.start(**spool);
  f.pairs(**spool, 100);
  ASSERT_TRUE((*spool)->close().ok());
  const auto segments = (*spool)->segments();
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].file.filename(), "inst-2.ring-000001.tachyon-inst");
  EXPECT_EQ((*spool)->stats().segmentsDiscarded, 0u);
}

TEST(SpoolRing, ASnapshotPinsItsSegmentsWhileTheRingKeepsRotating) {
  TempDir dir;
  SpoolOptions options;
  options.ringBytes = 512 << 10;
  options.segmentBytes = 64 << 10;
  auto spool = Spool::create(dir.path() / "inst-3.tachyon-inst", 0, nullptr, options);
  ASSERT_TRUE(spool.ok());
  Feeder f;
  f.start(**spool);
  f.pairs(**spool, 4000);
  auto snap = (*spool)->snapshot();
  ASSERT_TRUE(snap.ok());
  const std::vector<SpoolSegment> pinned = snap->segments;
  ASSERT_FALSE(pinned.empty());

  for (int i = 0; i < 40; ++i) f.pairs(**spool, 2000);  // far more than the ring holds
  EXPECT_GT((*spool)->stats().segmentsDiscarded, 0u);
  for (const SpoolSegment& seg : pinned) EXPECT_TRUE(fs::exists(seg.file)) << "pinned: " << seg.file;
  // The pinned data still decodes while the rest of the ring moved on.
  InstDecoder decoder;
  for (const SpoolSegment& seg : pinned) ASSERT_TRUE(decoder.addFile(seg.file, seg.bytes).ok());
  EXPECT_FALSE(decoder.finish().scopes.empty());

  snap->lease.reset();
  std::set<fs::path> alive;
  for (const SpoolSegment& seg : (*spool)->segments()) alive.insert(seg.file);
  for (const SpoolSegment& seg : pinned)
    if (alive.count(seg.file) == 0)
      EXPECT_FALSE(fs::exists(seg.file)) << "released and retired: " << seg.file;
  ASSERT_TRUE((*spool)->close().ok());
  EXPECT_LE(fileCount(dir.path()), 9u);
}

TEST(SpoolRing, ANewStreamHeaderStartsFreshStringsAndNames) {
  TempDir dir;
  SpoolOptions options;
  options.ringBytes = 512 << 10;
  options.segmentBytes = 64 << 10;
  auto spool = Spool::create(dir.path() / "inst-4.tachyon-inst", 0, nullptr, options);
  ASSERT_TRUE(spool.ok());
  Feeder first;
  first.start(**spool);
  first.pairs(**spool, 500);
  // The SDK switched streams: new header, strings and names re-sent (with a different key set).
  StreamBuilder second;
  second.header();
  const uint64_t key = second.string("second_scope");
  second.threadName(first.ts, kTid, "renamed");
  feed(**spool, second);
  Feeder cont;
  cont.ts = first.ts;
  cont.key = key;
  for (int i = 0; i < 30; ++i) cont.pairs(**spool, 2000);
  ASSERT_TRUE((*spool)->close().ok());
  InstDecoder decoder;
  for (const SpoolSegment& seg : (*spool)->segments()) ASSERT_TRUE(decoder.addFile(seg.file, seg.bytes).ok());
  const DecodedInstrumentation data = decoder.finish();
  EXPECT_EQ(data.stats.unresolvedStrings, 0u);
  std::set<std::string> names;
  for (const auto& rec : data.scopes)
    names.insert(std::string(data.strings.get(trace::ScopeEventRec(rec).id)));
  names.erase("");
  EXPECT_EQ(names, std::set<std::string>{"second_scope"});
}

TEST(SpoolRing, TheSharedBudgetMakesARingDiscardItsOldestSegments) {
  TempDir dir;
  SpoolOptions options;
  options.ringBytes = 4 << 20;
  options.segmentBytes = 256 << 10;
  auto budget = std::make_shared<SpoolBudget>(1 << 20);  // far below the ring
  auto spool = Spool::create(dir.path() / "inst-5.tachyon-inst", 0, budget, options);
  ASSERT_TRUE(spool.ok());
  Feeder f;
  f.start(**spool);
  for (int i = 0; i < 20; ++i) f.pairs(**spool, 3000);
  ASSERT_TRUE((*spool)->close().ok());
  EXPECT_LE(budget->used(), (1u << 20) + 4096);  // the strings a new segment starts with are not refused
  EXPECT_GT((*spool)->stats().segmentsDiscarded, 0u);
  EXPECT_LE(directoryBytes(dir.path()), (1u << 20) + options.segmentBytes + (64 << 10));
}

// ---- the merge of a rotated spool into a flushed trace ----

TEST(SpoolRing, MergingARotatedSpoolIntoAWindowGivesBalancedScopesInsideIt) {
  TempDir dir;
  SpoolOptions options;
  options.ringBytes = 1 << 20;
  options.segmentBytes = 128 << 10;
  auto spool = Spool::create(dir.path() / "inst-6.tachyon-inst", 0, nullptr, options);
  ASSERT_TRUE(spool.ok());
  // Nested scopes: outer(tid 7) { inner } repeated, so a window edge always cuts through something.
  StreamBuilder head;
  head.header();
  const uint64_t outer = head.string("outer");
  const uint64_t inner = head.string("inner");
  const uint64_t frameName = head.string("test.frame");
  head.threadName(1000, kTid, "ring-thread");
  feed(**spool, head);
  uint64_t ts = 1000;
  for (int round = 0; round < 60; ++round) {
    StreamBuilder b;
    for (int i = 0; i < 1500; ++i) {
      b.scopeBegin(ts, kTid, outer);
      b.scopeBegin(ts + 2, kTid, inner);
      b.scopeEnd(ts + 4, kTid);
      b.scopeEnd(ts + 8, kTid);
      b.frameMark(ts + 9, kTid, frameName, ts / 10);
      ts += 10;
    }
    feed(**spool, b);
  }
  // A scope still open when the flush happens.
  StreamBuilder open;
  open.scopeBegin(ts, kTid, outer);
  feed(**spool, open);
  auto snap = (*spool)->snapshot();
  ASSERT_TRUE(snap.ok());
  ASSERT_GE(snap->segments.size(), 2u);

  // The flushed trace's window sits inside the retained data, starting in the middle of a scope.
  const uint64_t windowStart = ts - 20000 + 3;
  const uint64_t windowEnd = ts + 1;
  const auto base = testing_util::writeBaseTrace(dir.path() / "flush.tachyon", windowStart, windowEnd);
  std::vector<MergeInput> inputs;
  for (const SpoolSegment& seg : snap->segments) inputs.push_back({seg.file, seg.bytes});
  MergeOptions merge;
  merge.clipToTraceRange = true;
  merge.balanceClippedScopes = true;
  merge.captureInfoPatch = "{\"flush_reason\":\"hitch\"}";
  auto merged = mergeInstrumentation(base.path, inputs, merge);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_GT(merged->orphanEndsDropped, 0u);
  EXPECT_EQ(merged->syntheticEnds, 1u);

  auto reader = trace::TraceReader::open(base.path);
  ASSERT_TRUE(reader.ok());
  const auto scopes = reader->readScopeEvents().value();
  ASSERT_FALSE(scopes.empty());
  const ScopeBalance balance = checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);
  for (const auto& rec : scopes) {
    const trace::ScopeEventRec r = rec;
    EXPECT_GE(r.ts, windowStart);
    EXPECT_LE(r.ts, windowEnd);
  }
  const auto marks = reader->readFrameMarks().value();
  ASSERT_FALSE(marks.empty());
  for (const auto& rec : marks) {
    const trace::FrameMarkRec m = rec;
    EXPECT_GE(m.ts, windowStart);
    EXPECT_LE(m.ts, windowEnd);
  }
  const std::string info = reader->readCaptureInfo().value();
  EXPECT_NE(info.find("\"flush_reason\":\"hitch\""), std::string::npos) << info;
}

TEST(SpoolRing, ACaptureInfoPatchAloneRewritesATraceWithoutInstrumentation) {
  TempDir dir;
  const auto base = testing_util::writeBaseTrace(dir.path() / "plain.tachyon");
  MergeOptions merge;
  merge.captureInfoPatch = "{\"flush_reason\":\"manual\",\"flush_window_s\":10}";
  auto merged = mergeInstrumentation(base.path, {}, merge);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_TRUE(merged->wroteOutput);
  auto reader = trace::TraceReader::open(base.path);
  ASSERT_TRUE(reader.ok());
  const std::string info = reader->readCaptureInfo().value();
  EXPECT_NE(info.find("\"flush_reason\":\"manual\""), std::string::npos) << info;
  EXPECT_EQ(info.find("\"instrumentation\""), std::string::npos) << "no merge entry for an empty merge";
  EXPECT_EQ(reader->header().flags & trace::kFlagHasInstrumentation, 0u);
}

}  // namespace
}  // namespace tachyon::inst
