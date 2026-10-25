#include "inst/merge.h"

#include "../control/test_util.h"
#include "inst/sidecar_paths.h"
#include "packed.h"
#include "trace/trace_reader.h"
#include "trace_builder.h"
#include "wire_builder.h"

#include <gtest/gtest.h>

#include <cstring>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <tuple>

namespace tachyon::inst {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using testing_util::BaseTrace;
using testing_util::pv;
using testing_util::StreamBuilder;
using testing_util::writeBaseTrace;
using namespace trace;

std::vector<char> readBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

size_t countFiles(const fs::path& dir) {
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    (void)e;
    ++n;
  }
  return n;
}

bool hasWarning(const MergeReport& r, const std::string& needle) {
  return std::any_of(r.warnings.begin(), r.warnings.end(),
                     [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

// A stream with two threads of nested scopes, a frame mark, a marker, a plot and thread names.
StreamBuilder sampleInstrumentation(uint64_t t0 = 1500) {
  StreamBuilder b;
  b.header(1234, 1'000'000'000, t0);
  const uint64_t outer = b.string("frame");
  const uint64_t inner = b.string("update");
  const uint64_t fm = b.string("Frame");
  const uint64_t mk = b.string("boss");
  const uint64_t plot = b.string("fps");
  b.threadName(t0, 1234, "main");
  b.threadName(t0, 1235, "worker");
  b.scopeBegin(t0 + 10, 1234, outer);
  b.scopeBegin(t0 + 20, 1234, inner, "data");
  b.scopeBegin(t0 + 25, 1235, inner);
  b.scopeEnd(t0 + 30, 1235);
  b.scopeEnd(t0 + 40, 1234);
  b.frameMark(t0 + 45, 1234, fm, 1);
  b.marker(t0 + 46, 1234, mk, "spawn");
  b.plot(t0 + 47, plot, 59.9);
  b.scopeEnd(t0 + 50, 1234);
  return b;
}

// Everything but the instrumentation chunks must survive a merge unchanged.
void expectSamplingDataEqual(const fs::path& before, const fs::path& after) {
  auto a = TraceReader::open(before);
  auto b = TraceReader::open(after);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  // The merge appends strings; the trace's own keep their indices.
  const StringTable sa = a->readStrings().value(), sb = b->readStrings().value();
  EXPECT_LE(sa.size(), sb.size());
  for (uint32_t i = 0; i < sa.size(); ++i) EXPECT_EQ(sa.get(StringIdx(i)), sb.get(StringIdx(i)));
  EXPECT_EQ(a->readFrames().value(), b->readFrames().value());
  EXPECT_EQ(a->readCallstackNodes().value(), b->readCallstackNodes().value());
  EXPECT_EQ(a->readProcesses().value(), b->readProcesses().value());
  EXPECT_EQ(a->readThreads().value(), b->readThreads().value());
  EXPECT_EQ(a->readModuleInfos().value(), b->readModuleInfos().value());
  EXPECT_EQ(a->readSamples().value(), b->readSamples().value());
  EXPECT_EQ(a->readContextSwitches().value(), b->readContextSwitches().value());
  EXPECT_EQ(a->readReadyEvents().value(), b->readReadyEvents().value());
  EXPECT_EQ(a->readClockCalibrations().value(), b->readClockCalibrations().value());
}

TEST(Merge, ToANewFileKeepsTheInputAndAddsInstrumentation) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const auto original = readBytes(base.path);
  const fs::path inst = sampleInstrumentation().write(dir.path() / "app.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "merged.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_TRUE(merged->wroteOutput);
  EXPECT_EQ(merged->outputPath, opts.outPath);
  EXPECT_EQ(readBytes(base.path), original);  // the input is untouched
  EXPECT_EQ(merged->scopeEvents, 6u);
  EXPECT_EQ(merged->fiberEvents, 0u);
  EXPECT_EQ(merged->frameMarks, 1u);
  EXPECT_EQ(merged->markers, 1u);
  EXPECT_EQ(merged->plots, 1u);
  EXPECT_EQ(merged->threadNames, 2u);

  expectSamplingDataEqual(base.path, opts.outPath);
  auto reader = TraceReader::open(opts.outPath);
  ASSERT_TRUE(reader.ok());
  EXPECT_NE(reader->header().flags & kFlagHasInstrumentation, 0u);
  EXPECT_EQ(reader->header().flags & kFlagPartial, 0u);
  EXPECT_EQ(reader->header().clock_frequency_hz, base.clockHz);
  EXPECT_EQ(reader->header().platform, kPlatformLinux);

  const StringTable strings = reader->readStrings().value();
  const auto scopes = reader->readScopeEvents().value();
  ASSERT_EQ(scopes.size(), 6u);
  const ScopeEventRec first = scopes[0];
  EXPECT_EQ(first.kind, kScopeBegin);
  EXPECT_EQ(strings.get(first.id), "frame");
  EXPECT_EQ(pv(first.tid), 1234u);
  EXPECT_EQ(pv(first.ts), 1510u);
  // New strings were appended after the trace's own, the old ones kept their indices.
  EXPECT_EQ(strings.get(StringIdx(1)), "app.exe");
  EXPECT_GT(pv(first.id).get(), 3u);
  const ScopeEventRec second = scopes[1];
  EXPECT_EQ(strings.get(second.data), "data");
  const auto balance = checkScopeBalance(scopes);
  EXPECT_EQ(balance.threads, 2u);
  EXPECT_EQ(balance.begins, 3u);
  EXPECT_EQ(balance.ends, 3u);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);

  const auto marks = reader->readFrameMarks().value();
  ASSERT_EQ(marks.size(), 1u);
  const FrameMarkRec mark = marks[0];
  EXPECT_EQ(strings.get(mark.name), "Frame");
  const auto markers = reader->readMarkers().value();
  ASSERT_EQ(markers.size(), 1u);
  const MarkerRec marker = markers[0];
  EXPECT_EQ(strings.get(marker.data), "spawn");
  const auto plots = reader->readPlotSamples().value();
  ASSERT_EQ(plots.size(), 1u);
  const PlotSampleRec plot = plots[0];
  EXPECT_EQ(strings.get(plot.name), "fps");
  EXPECT_DOUBLE_EQ(pv(plot.value), 59.9);
  const auto names = reader->readThreadNames().value();
  ASSERT_EQ(names.size(), 2u);

  const auto info = nlohmann::json::parse(reader->readCaptureInfo().value());
  EXPECT_EQ(info["sampling_hz"], 8000);  // the original document survives
  EXPECT_EQ(info["agent_version"], "test");
  ASSERT_TRUE(info.contains("instrumentation"));
  const auto& merge0 = info["instrumentation"]["merges"][0];
  EXPECT_EQ(merge0["scope_events"], 6);
  EXPECT_EQ(merge0["sources"].size(), 1u);
  EXPECT_EQ(merge0["sources"][0]["hash"].get<std::string>().size(), 16u);
  EXPECT_EQ(info["instrumentation"]["merge_count"], 1);
  EXPECT_EQ(merge0["streams"].size(), 1u);
  EXPECT_EQ(merge0["streams"][0]["pid"], 1234);
}

TEST(Merge, InPlaceGoesThroughATemporaryFileAndLeavesNothingBehind) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path copy = dir.path() / "copy.tachyon";
  fs::copy_file(base.path, copy);
  const fs::path inst = sampleInstrumentation().write(dir.path() / "app.tachyon-inst");
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(copy, std::span<const MergeInput>(&in, 1));
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_EQ(merged->outputPath, copy);
  EXPECT_TRUE(merged->wroteOutput);
  // Only the three files of this test remain: no *.tmp.
  EXPECT_EQ(countFiles(dir.path()), 3u);
  auto reader = TraceReader::open(copy);
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(reader->readScopeEvents().value().size(), 6u);
  expectSamplingDataEqual(base.path, copy);
}

TEST(Merge, OutPathEqualToTheInputCountsAsInPlace) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path inst = sampleInstrumentation().write(dir.path() / "app.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "." / "base.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_EQ(merged->outputPath, base.path);
  EXPECT_EQ(countFiles(dir.path()), 2u);
}

TEST(Merge, NothingToMergeInPlaceLeavesTheFileByteIdentical) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const auto original = readBytes(base.path);
  StreamBuilder empty;
  empty.header();
  const fs::path inst = empty.write(dir.path() / "empty.tachyon-inst");
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1));
  ASSERT_TRUE(merged.ok());
  EXPECT_FALSE(merged->wroteOutput);
  EXPECT_TRUE(hasWarning(*merged, "no instrumentation records"));
  EXPECT_EQ(readBytes(base.path), original);
}

TEST(Merge, NothingToMergeWithOutStillProducesAValidCopyWithoutTheFlag) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  StreamBuilder empty;
  empty.header();
  const fs::path inst = empty.write(dir.path() / "empty.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  EXPECT_TRUE(merged->wroteOutput);
  auto reader = TraceReader::open(opts.outPath);
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(reader->header().flags & kFlagHasInstrumentation, 0u);
  expectSamplingDataEqual(base.path, opts.outPath);
}

TEST(Merge, RecordsOutsideTheWindowAreKeptAndTheRangeIsExtended) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon", 1000, 2000);
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("early");
  b.scopeBegin(500, 1, k);  // before the trace
  b.scopeEnd(700, 1);
  b.scopeBegin(1500, 1, k);  // inside
  b.scopeEnd(1600, 1);
  b.scopeBegin(2500, 1, k);  // after
  b.scopeEnd(2600, 1);
  const fs::path inst = b.write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged->scopeEvents, 6u);
  EXPECT_EQ(merged->clippedRecords, 0u);
  EXPECT_EQ(merged->startTs, 500u);
  EXPECT_EQ(merged->endTs, 2600u);
  auto reader = TraceReader::open(opts.outPath);
  EXPECT_EQ(reader->header().start_ts, 500u);
  EXPECT_EQ(reader->header().end_ts, 2600u);
}

TEST(Merge, ClippingDropsRecordsOutsideTheWindowAndKeepsTheRange) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon", 1000, 2000);
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(500, 1, k);
  b.scopeEnd(1200, 1);  // the Begin is clipped: an orphan End remains
  b.scopeBegin(1500, 1, k);
  b.scopeEnd(1600, 1);
  b.scopeBegin(2500, 1, k);
  b.plot(3000, k, 1.0);
  const fs::path inst = b.write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  opts.clipToTraceRange = true;
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged->scopeEvents, 3u);
  EXPECT_EQ(merged->plots, 0u);
  EXPECT_EQ(merged->clippedRecords, 3u);
  EXPECT_TRUE(hasWarning(*merged, "outside the trace window"));
  EXPECT_EQ(merged->startTs, 1000u);
  EXPECT_EQ(merged->endTs, 2000u);
  auto reader = TraceReader::open(opts.outPath);
  EXPECT_EQ(reader->header().start_ts, 1000u);
  EXPECT_EQ(reader->header().end_ts, 2000u);
  const auto scopeRecs = reader->readScopeEvents().value();
  for (const auto& rec : scopeRecs) {
    const uint64_t ts = rec.ts;
    EXPECT_GE(ts, 1000u);
    EXPECT_LE(ts, 2000u);
  }
}

TEST(Merge, ClippingNeedsAKnownWindowOtherwiseTheRangeIsFilledFromTheRecords) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon", 0, 0);  // a header without a time range
  const fs::path inst = sampleInstrumentation(1500).write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  opts.clipToTraceRange = true;
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged->clippedRecords, 0u);
  EXPECT_EQ(merged->scopeEvents, 6u);
  EXPECT_EQ(merged->startTs, 1510u);  // thread names (at 1500) do not count
  EXPECT_EQ(merged->endTs, 1550u);
}

TEST(Merge, MergingTwiceUnitesTheRecordsInTimestampOrder) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path copy = dir.path() / "work.tachyon";
  fs::copy_file(base.path, copy);
  const fs::path first = sampleInstrumentation(1500).write(dir.path() / "first.tachyon-inst");
  const fs::path second =
      sampleInstrumentation(1525).write(dir.path() / "second.tachyon-inst");  // overlaps in time
  const MergeInput in1{first}, in2{second};
  ASSERT_TRUE(mergeInstrumentation(copy, std::span<const MergeInput>(&in1, 1)).ok());
  auto merged = mergeInstrumentation(copy, std::span<const MergeInput>(&in2, 1));
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_EQ(merged->existingInstrumentationRecords, 6u + 1u + 1u + 1u + 2u);
  auto reader = TraceReader::open(copy);
  ASSERT_TRUE(reader.ok());
  const auto scopes = reader->readScopeEvents().value();
  ASSERT_EQ(scopes.size(), 12u);
  uint64_t last = 0;
  for (const auto& rec : scopes) {
    const uint64_t ts = rec.ts;
    EXPECT_GE(ts, last);
    last = ts;
  }
  EXPECT_EQ(checkScopeBalance(scopes).outOfOrder, 0u);
  const StringTable strings = reader->readStrings().value();
  size_t frames = 0;
  for (const auto& rec : scopes) {
    const ScopeEventRec r = rec;
    if (r.kind == kScopeBegin && strings.get(r.id) == "frame") ++frames;
  }
  EXPECT_EQ(frames, 2u);
  const auto info = nlohmann::json::parse(reader->readCaptureInfo().value());
  EXPECT_EQ(info["instrumentation"]["merge_count"], 2);
}

std::vector<std::tuple<uint32_t, uint64_t, std::string>> readNames(const fs::path& path) {
  auto reader = TraceReader::open(path);
  EXPECT_TRUE(reader.ok());
  std::vector<std::tuple<uint32_t, uint64_t, std::string>> out;
  if (!reader.ok()) return out;
  const StringTable strings = reader->readStrings().value();
  const auto names = reader->readThreadNames().value();
  for (const auto& rec : names) {
    const ThreadNameRec r = rec;
    out.emplace_back(r.tid, r.ts, std::string(strings.get(r.name)));
  }
  return out;
}

StreamBuilder namesAndOneScope() {
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("work");
  b.threadName(100, 5, "old");             // before the window, replaced by the next one
  b.threadName(500, 5, "renamed-before");  // the name thread 5 has when the window opens
  b.threadName(2000, 6, "inside");
  b.threadName(5000, 5, "after");       // after the window
  b.threadName(300, 7, "late-attach");  // set long before the window, never renamed
  b.scopeBegin(1500, 6, k);
  b.scopeEnd(1600, 6);
  return b;
}

TEST(Merge, ThreadNamesAreNotClippedTheLastNameBeforeTheWindowIsKeptAtItsStart) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon", 1000, 3000);
  const fs::path inst = namesAndOneScope().write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  opts.clipToTraceRange = true;  // a flush / circular capture
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_EQ(merged->threadNames, 4u);  // "old" is superseded; nothing is counted as clipped
  EXPECT_EQ(merged->clippedRecords, 0u);
  EXPECT_EQ(merged->startTs, 1000u);
  EXPECT_EQ(merged->endTs, 3000u);
  const auto names = readNames(opts.outPath);
  ASSERT_EQ(names.size(), 4u);
  using Row = std::tuple<uint32_t, uint64_t, std::string>;
  EXPECT_EQ(names[0], (Row{7, 1000, "late-attach"}));
  EXPECT_EQ(names[1], (Row{5, 1000, "renamed-before"}));
  EXPECT_EQ(names[2], (Row{6, 2000, "inside"}));
  EXPECT_EQ(names[3], (Row{5, 3000, "after"}));
}

TEST(Merge, ThreadNamesNeverWidenTheRangeAndAreClampedIntoIt) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon", 1000, 3000);
  const fs::path inst = namesAndOneScope().write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";  // default: extend instead of clip
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_EQ(merged->startTs, 1000u);  // the names at 100 and 5000 did not move the range
  EXPECT_EQ(merged->endTs, 3000u);
  auto reader = TraceReader::open(opts.outPath);
  EXPECT_EQ(reader->header().start_ts, 1000u);
  EXPECT_EQ(reader->header().end_ts, 3000u);
  for (const auto& [tid, ts, name] : readNames(opts.outPath)) {
    EXPECT_GE(ts, 1000u) << name;
    EXPECT_LE(ts, 3000u) << name;
  }
}

TEST(Merge, MergingTheSameSidecarTwiceIsRefusedUnlessForced) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path copy = dir.path() / "work.tachyon";
  fs::copy_file(base.path, copy);
  const fs::path inst = sampleInstrumentation().write(dir.path() / "a.tachyon-inst");
  const MergeInput in{inst};
  ASSERT_TRUE(mergeInstrumentation(copy, std::span<const MergeInput>(&in, 1)).ok());
  const auto afterFirst = readBytes(copy);

  auto again = mergeInstrumentation(copy, std::span<const MergeInput>(&in, 1));
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().code, ErrorCode::InvalidArgument);
  EXPECT_NE(again.error().message.find("already merged"), std::string::npos) << again.error().message;
  EXPECT_NE(again.error().message.find("--force"), std::string::npos);
  EXPECT_EQ(readBytes(copy), afterFirst);  // untouched, no temp file
  EXPECT_EQ(countFiles(dir.path()), 3u);

  MergeOptions force;
  force.force = true;
  auto forced = mergeInstrumentation(copy, std::span<const MergeInput>(&in, 1), force);
  ASSERT_TRUE(forced.ok()) << forced.error().toString();
  EXPECT_TRUE(hasWarning(*forced, "merged again"));
  auto reader = TraceReader::open(copy);
  EXPECT_EQ(reader->readScopeEvents().value().size(), 12u);
}

TEST(Merge, CaptureInfoListsEveryMergeAndKeepsEarlierOnes) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path copy = dir.path() / "work.tachyon";
  fs::copy_file(base.path, copy);
  const fs::path first = sampleInstrumentation(1500).write(dir.path() / "first.tachyon-inst");
  const fs::path second = sampleInstrumentation(1700).write(dir.path() / "second.tachyon-inst");
  const MergeInput in1{first}, in2{second};
  ASSERT_TRUE(mergeInstrumentation(copy, std::span<const MergeInput>(&in1, 1)).ok());
  ASSERT_TRUE(mergeInstrumentation(copy, std::span<const MergeInput>(&in2, 1)).ok());
  auto reader = TraceReader::open(copy);
  const auto info = nlohmann::json::parse(reader->readCaptureInfo().value());
  const auto& merges = info["instrumentation"]["merges"];
  ASSERT_EQ(merges.size(), 2u);
  EXPECT_EQ(info["instrumentation"]["merge_count"], 2);
  EXPECT_EQ(merges[0]["scope_events"], 6);
  EXPECT_EQ(merges[1]["scope_events"], 6);
  EXPECT_NE(merges[0]["sources"][0]["hash"], merges[1]["sources"][0]["hash"]);
  EXPECT_EQ(info["sampling_hz"], 8000);
}

TEST(Merge, NumberedSiblingsAreMergedInNumericOrder) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path stem = dir.path() / "app-1234.tachyon-inst";
  // The SDK lost the agent twice: three sidecar phases, the later ones in numbered files.
  auto phase = [&](const fs::path& path, uint64_t t0, const char* name) {
    StreamBuilder b;
    b.header(1234, 1'000'000'000, 1);
    const uint64_t k = b.string(name);
    b.scopeBegin(t0, 1, k);
    b.scopeEnd(t0 + 5, 1);
    b.write(path);
  };
  phase(stem, 1100, "phase0");
  phase(numberedSidecarPath(stem, 1), 1200, "phase1");
  phase(numberedSidecarPath(stem, 10), 1400, "phase10");
  phase(numberedSidecarPath(stem, 2), 1300, "phase2");
  std::vector<MergeInput> inputs;
  for (const fs::path& p : expandSidecarSiblings(stem)) inputs.push_back({p});
  ASSERT_EQ(inputs.size(), 4u);
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  auto merged = mergeInstrumentation(base.path, inputs, opts);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  ASSERT_EQ(merged->streams.size(), 4u);
  // The files were read in numeric order of their suffix (2 before 10), not lexicographic order.
  const char* expectedFiles[4] = {"app-1234.tachyon-inst", "app-1234.1.tachyon-inst",
                                  "app-1234.2.tachyon-inst", "app-1234.10.tachyon-inst"};
  for (size_t i = 0; i < 4; ++i)
    EXPECT_EQ(fs::path(merged->streams[i].source).filename().string(), expectedFiles[i]);
  auto reader = TraceReader::open(opts.outPath);
  const StringTable strings = reader->readStrings().value();
  std::vector<std::string> names;
  const auto scopeRecs = reader->readScopeEvents().value();
  for (const auto& rec : scopeRecs) {
    const ScopeEventRec r = rec;
    if (r.kind == kScopeBegin) names.emplace_back(strings.get(r.id));
  }
  EXPECT_EQ(names, (std::vector<std::string>{"phase0", "phase1", "phase2", "phase10"}));
}

TEST(Merge, TruncatedSidecarMergesTheCompleteFramesAndWarns) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  StreamBuilder b = sampleInstrumentation();
  std::vector<uint8_t> bytes = b.bytes();
  bytes.resize(bytes.size() - 7);  // the process died in the middle of the last frame
  const fs::path inst = dir.path() / "torn.tachyon-inst";
  std::ofstream(inst, std::ios::binary)
      .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged->scopeEvents, 5u);
  EXPECT_TRUE(hasWarning(*merged, "truncated"));
  EXPECT_TRUE(hasWarning(*merged, "still open"));  // the last End never arrived
  EXPECT_EQ(merged->decode.unclosedScopes, 1u);
}

TEST(Merge, DroppedRecordsBecomeMarkersAndWarnings) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  StreamBuilder b = sampleInstrumentation();
  b.dropped(1590, 1234, 17);
  const fs::path inst = b.write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  EXPECT_TRUE(hasWarning(*merged, "17 instrumentation records were dropped"));
  auto reader = TraceReader::open(opts.outPath);
  const StringTable strings = reader->readStrings().value();
  bool found = false;
  const auto markerRecs = reader->readMarkers().value();
  for (const auto& rec : markerRecs) {
    const MarkerRec m = rec;
    if (strings.get(m.name) == kDroppedMarkerName) {
      found = true;
      EXPECT_EQ(strings.get(m.data), "17");
    }
  }
  EXPECT_TRUE(found);
  const auto info = nlohmann::json::parse(reader->readCaptureInfo().value());
  EXPECT_EQ(info["instrumentation"]["merges"][0]["dropped_records"], 17);
}

TEST(Merge, UnbalancedScopesAreWarnings) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("s");
  b.scopeBegin(1500, 1, k);
  b.scopeEnd(1510, 1);
  b.scopeEnd(1520, 1);       // one End too many
  b.scopeBegin(1530, 2, k);  // never closed
  const fs::path inst = b.write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged->decode.unmatchedScopeEnds, 1u);
  EXPECT_EQ(merged->decode.unclosedScopes, 1u);
  EXPECT_TRUE(hasWarning(*merged, "scope ends had no open scope"));
  EXPECT_TRUE(hasWarning(*merged, "still open"));
}

TEST(Merge, MissingCaptureInfoIsCreatedAndNonJsonIsPreserved) {
  TempDir dir;
  const BaseTrace noInfo =
      writeBaseTrace(dir.path() / "noinfo.tachyon", 1000, 2'000'000, 1'000'000'000, false);
  const fs::path inst = sampleInstrumentation().write(dir.path() / "a.tachyon-inst");
  const MergeInput in{inst};
  MergeOptions opts;
  opts.outPath = dir.path() / "out1.tachyon";
  ASSERT_TRUE(mergeInstrumentation(noInfo.path, std::span<const MergeInput>(&in, 1), opts).ok());
  auto reader = TraceReader::open(opts.outPath);
  const auto info = nlohmann::json::parse(reader->readCaptureInfo().value());
  EXPECT_TRUE(info.contains("instrumentation"));

  // A CaptureInfo that is not a JSON object stays readable.
  const fs::path weird = dir.path() / "weird.tachyon";
  TraceWriter w = TraceWriter::open(weird,
                                    [] {
                                      FileHeader h;
                                      h.clock_frequency_hz = 1'000'000'000;
                                      h.start_ts = 1000;
                                      h.end_ts = 2000;
                                      return h;
                                    }())
                      .value();
  w.appendCaptureInfo("not json at all");
  ASSERT_TRUE(w.finish().ok());
  opts.outPath = dir.path() / "out2.tachyon";
  ASSERT_TRUE(mergeInstrumentation(weird, std::span<const MergeInput>(&in, 1), opts).ok());
  auto reader2 = TraceReader::open(opts.outPath);
  const auto info2 = nlohmann::json::parse(reader2->readCaptureInfo().value());
  EXPECT_EQ(info2["original_capture_info"], "not json at all");
  EXPECT_TRUE(info2.contains("instrumentation"));
}

TEST(Merge, CircularFlagAndOtherHeaderFieldsSurvive) {
  TempDir dir;
  const BaseTrace base =
      writeBaseTrace(dir.path() / "base.tachyon", 1000, 2'000'000, 10'000'000, true, kFlagCircularCapture);
  StreamBuilder b = sampleInstrumentation();
  const fs::path inst = b.write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok());
  auto reader = TraceReader::open(opts.outPath);
  EXPECT_NE(reader->header().flags & kFlagCircularCapture, 0u);
  EXPECT_NE(reader->header().flags & kFlagHasInstrumentation, 0u);
  EXPECT_EQ(reader->header().clock_frequency_hz, 10'000'000u);
  // The stream's clock (1 GHz) differs from the trace's (10 MHz): scaled, with a warning.
  EXPECT_TRUE(hasWarning(*merged, "scaled by the frequency ratio"));
  const ScopeEventRec first = reader->readScopeEvents().value()[0];
  EXPECT_EQ(pv(first.ts), 15u);  // 1510 ns -> 15 ticks of 100 ns
}

TEST(Merge, FailuresLeaveTheInputUntouchedAndNoTempFiles) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon");
  const auto original = readBytes(base.path);
  const size_t files = countFiles(dir.path());

  // Instrumentation file that does not exist.
  MergeInput missing{dir.path() / "nope.tachyon-inst"};
  auto r1 = mergeInstrumentation(base.path, std::span<const MergeInput>(&missing, 1));
  ASSERT_FALSE(r1.ok());
  EXPECT_EQ(r1.error().code, ErrorCode::NotFound);

  // Not an instrumentation stream.
  const fs::path junk = dir.path() / "junk.tachyon-inst";
  std::ofstream(junk) << "junk junk junk junk junk junk";
  MergeInput junkIn{junk};
  auto r2 = mergeInstrumentation(base.path, std::span<const MergeInput>(&junkIn, 1));
  ASSERT_FALSE(r2.ok());
  EXPECT_EQ(r2.error().code, ErrorCode::CorruptData);

  // Trace that does not exist.
  const fs::path inst = sampleInstrumentation().write(dir.path() / "a.tachyon-inst");
  MergeInput ok1{inst};
  auto r3 = mergeInstrumentation(dir.path() / "gone.tachyon", std::span<const MergeInput>(&ok1, 1));
  EXPECT_FALSE(r3.ok());

  EXPECT_EQ(readBytes(base.path), original);
  EXPECT_EQ(countFiles(dir.path()), files + 2u);  // the two inst files created above
}

// A hand-built partial file whose string table has a duplicate: the merge cannot keep the indices and says
// so.
TEST(Merge, TraceWithDuplicateStringsIsRejectedWithoutDamage) {
  TempDir dir;
  const fs::path path = dir.path() / "dup.tachyon";
  {
    FileHeader h;
    h.clock_frequency_hz = 1'000'000'000;
    h.start_ts = 1;
    h.end_ts = 2;
    h.flags = kFlagPartial;
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    std::vector<char> payload;
    for (const char* s : {"", "a", "a"}) {
      const uint32_t len = static_cast<uint32_t>(std::strlen(s));
      payload.insert(payload.end(), reinterpret_cast<const char*>(&len),
                     reinterpret_cast<const char*>(&len) + 4);
      payload.insert(payload.end(), s, s + len);
    }
    ChunkHeader ch;
    ch.type = static_cast<uint32_t>(ChunkType::Strings);
    ch.uncompressed_size = ch.compressed_size = payload.size();
    ch.record_count = 3;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(&ch), sizeof(ch));
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  }
  const auto original = readBytes(path);
  const fs::path inst = sampleInstrumentation().write(dir.path() / "a.tachyon-inst");
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(path, std::span<const MergeInput>(&in, 1));
  ASSERT_FALSE(merged.ok());
  EXPECT_EQ(merged.error().code, ErrorCode::Unsupported);
  EXPECT_EQ(readBytes(path), original);
  EXPECT_EQ(countFiles(dir.path()), 2u);
}

TEST(Merge, PartialInputIsRecoveredIntoAFinishedFile) {
  TempDir dir;
  const fs::path path = dir.path() / "partial.tachyon";
  {
    FileHeader h;
    h.clock_frequency_hz = 1'000'000'000;
    h.start_ts = 1000;
    h.end_ts = 2000;
    TraceWriter w = TraceWriter::open(path, h).value();
    w.internString("only-string");
    SampleBatch s;
    s.push(1100, 1, CallstackIdx(0), 0);
    w.append(s);
    // No finish(): the writer crashed.
  }
  ASSERT_TRUE(TraceReader::open(path)->isPartial());
  const fs::path inst = sampleInstrumentation().write(dir.path() / "a.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  EXPECT_TRUE(hasWarning(*merged, "partial"));
  auto reader = TraceReader::open(opts.outPath);
  ASSERT_TRUE(reader.ok());
  // The capture behind the input was cut short: the flag stays, though the file now has a valid footer.
  EXPECT_TRUE(reader->isPartial());
  EXPECT_EQ(reader->readSamples().value().size(), 1u);
  EXPECT_EQ(reader->readScopeEvents().value().size(), 6u);
}

TEST(Merge, MoreThanOneChunkOfEachKindIsSplitAtTheFormatLimit) {
  TempDir dir;
  const BaseTrace base = writeBaseTrace(dir.path() / "base.tachyon", 1000, 3'000'000'000ull);
  StreamBuilder b;
  b.header();
  const uint64_t k = b.string("tick");
  constexpr uint64_t kPairs = 530'000;  // 1.06 M records: just over one chunk
  for (uint64_t i = 0; i < kPairs; ++i) {
    b.scopeBegin(2000 + i * 4, 1, k);
    b.scopeEnd(2002 + i * 4, 1);
  }
  const fs::path inst = b.write(dir.path() / "big.tachyon-inst");
  MergeOptions opts;
  opts.outPath = dir.path() / "out.tachyon";
  const MergeInput in{inst};
  auto merged = mergeInstrumentation(base.path, std::span<const MergeInput>(&in, 1), opts);
  ASSERT_TRUE(merged.ok()) << merged.error().toString();
  auto reader = TraceReader::open(opts.outPath);
  ASSERT_TRUE(reader.ok());
  size_t scopeChunks = 0;
  for (const FooterRec& c : reader->chunks())
    if (c.type == static_cast<uint32_t>(ChunkType::ScopeEvents)) ++scopeChunks;
  EXPECT_EQ(scopeChunks, 2u);
  const auto scopes = reader->readScopeEvents().value();
  ASSERT_EQ(scopes.size(), 2 * kPairs);
  const auto balance = checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);
}

TEST(CheckScopeBalance, CountsUnmatchedUnclosedAndOutOfOrder) {
  auto rec = [](uint64_t ts, uint32_t tid, uint8_t kind) {
    ScopeEventRec r;
    std::memset(static_cast<void*>(&r), 0, sizeof(r));
    r.ts = ts;
    r.tid = tid;
    r.kind = kind;
    return r;
  };
  const std::vector<ScopeEventRec> events = {
      rec(10, 1, kScopeBegin), rec(20, 1, kScopeEnd),  rec(30, 1, kScopeEnd),  // one unmatched
      rec(5, 2, kScopeBegin),  rec(4, 2, kScopeBegin),                         // out of order, two open
      rec(40, 3, kScopeBegin), rec(50, 3, kScopeEnd)};
  const ScopeBalance b = checkScopeBalance(events);
  EXPECT_EQ(b.threads, 3u);
  EXPECT_EQ(b.begins, 4u);
  EXPECT_EQ(b.ends, 3u);
  EXPECT_EQ(b.unmatchedEnds, 1u);
  EXPECT_EQ(b.unclosedBegins, 2u);
  EXPECT_EQ(b.outOfOrder, 1u);
}

}  // namespace
}  // namespace tachyon::inst
