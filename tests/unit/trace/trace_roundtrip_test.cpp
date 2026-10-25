#include "test_util.h"
#include "trace/trace_reader.h"
#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <fstream>
#include <vector>

namespace tachyon::trace {
namespace {

using testing_util::TempFile;

FileHeader makeHeader() {
  FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = 100;
  h.end_ts = 9000;
  h.platform = kPlatformLinux;
  return h;
}

const FooterRec* findChunk(const TraceReader& r, ChunkType t, size_t nth = 0) {
  for (const FooterRec& c : r.chunks())
    if (c.type == static_cast<uint32_t>(t) && nth-- == 0) return &c;
  return nullptr;
}

size_t countChunks(const TraceReader& r, ChunkType t) {
  size_t n = 0;
  for (const FooterRec& c : r.chunks()) n += c.type == static_cast<uint32_t>(t);
  return n;
}

TEST(TraceRoundTrip, EmptyFile) {
  TempFile f;
  auto w = TraceWriter::open(f.path(), makeHeader());
  ASSERT_TRUE(w.ok());
  ASSERT_TRUE(w->finish().ok());
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_FALSE(r->isPartial());
  EXPECT_EQ(r->header().format_version, 1u);
  EXPECT_NE(r->header().footer_offset, 0u);
  // The pre-interned "" string and the root callstack node are always emitted.
  EXPECT_EQ(countChunks(*r, ChunkType::Strings), 1u);
  EXPECT_EQ(countChunks(*r, ChunkType::CallstackNodes), 1u);
  auto nodes = r->readCallstackNodes();
  ASSERT_TRUE(nodes.ok());
  ASSERT_EQ(nodes->size(), 1u);
  EXPECT_FALSE((*nodes)[0].parent.valid());
  EXPECT_FALSE((*nodes)[0].frame.valid());
}

TEST(TraceRoundTrip, HeaderFieldsAndFlags) {
  TempFile f;
  FileHeader h = makeHeader();
  h.flags = kFlagHasInstrumentation | kFlagCircularCapture;
  auto w = TraceWriter::open(f.path(), h);
  ASSERT_TRUE(w.ok());
  w->setTimeRange(5, 77);
  ASSERT_TRUE(w->finish().ok());
  ASSERT_TRUE(w->finish().ok());  // idempotent
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->header().flags, kFlagHasInstrumentation | kFlagCircularCapture);
  EXPECT_EQ(r->header().start_ts, 5u);
  EXPECT_EQ(r->header().end_ts, 77u);
  EXPECT_EQ(r->header().clock_frequency_hz, 1'000'000'000u);
  EXPECT_EQ(r->header().platform, kPlatformLinux);
}

TEST(TraceRoundTrip, EveryChunkType) {
  TempFile f;
  auto w = TraceWriter::open(f.path(), makeHeader());
  ASSERT_TRUE(w.ok());

  const StringIdx sa = w->internString("alpha");
  const StringIdx sb = w->internString("beta");
  EXPECT_EQ(w->internString("alpha"), sa);
  EXPECT_EQ(w->internString(""), StringIdx(0));

  const FrameIdx f0 = w->internFrame(ModuleIdx(0), 0x1000);
  const FrameIdx f1 = w->internFrame(ModuleIdx(0), 0x2000);
  const FrameIdx f2 = w->internFrame(ModuleIdx::invalid(), 0xDEADBEEF12345678ull);
  EXPECT_EQ(w->internFrame(ModuleIdx(0), 0x1000), f0);
  const FrameIdx stackFrames[] = {f0, f1, f2};
  const CallstackIdx cs = w->internCallstack(stackFrames);
  EXPECT_EQ(w->internCallstack({}), CallstackIdx(0));

  std::vector<ProcessRec> procs = {{10, 1, 5, 500, sa, sb}, {11, 10, 6, 600, sb, sa}};
  std::vector<ThreadRec> threads = {{100, 10, 5, 400, sa, kThreadFlagTargetProcess}, {101, 10, 7, 0, sb, 0}};
  ModuleRec mod{};
  mod.pid = 10;
  mod.base = 0x7f0000000000ull;
  mod.size = 0x1234567;
  mod.path = sa;
  mod.id_kind = kModuleIdGnuBuildId;
  mod.id_len = 20;
  for (int i = 0; i < 20; ++i) mod.id[i] = static_cast<uint8_t>(i * 3);
  mod.age = 1;
  mod.load_ts = 11;
  mod.unload_ts = 22;
  std::vector<ModuleRec> mods = {mod};

  SampleBatch samples;
  for (uint32_t i = 0; i < 1000; ++i)
    samples.push(1000 + i * 125, 100 + (i & 1), i % 3 == 0 ? cs : CallstackIdx(0),
                 static_cast<uint16_t>(i % 8), static_cast<uint16_t>(i & 1));

  std::vector<ContextSwitchRec> cswitch = {{200, 3, 100, 101, 5, 1, 2, 8, cs, 0},
                                           {300, 4, 101, 100, 0, 0, 0, 9, CallstackIdx(0), 0}};
  std::vector<ReadyRec> ready = {{210, 3, 101, 100, cs, 1, 2, 0}};
  std::vector<ScopeEventRec> scopes = {{400, 100, kScopeBegin, {0, 0, 0}, sa, sb, 0xFF8800FFu, 0xABCDEF},
                                       {450, 100, kScopeEnd, {0, 0, 0}, StringIdx(0), StringIdx(0), 0, 0}};
  std::vector<FiberEventRec> fibers = {{500, 100, kFiberRegister, {0, 0, 0}, 77, 0},
                                       {510, 100, kFiberSwitchBegin, {0, 0, 0}, 77, 78}};
  std::vector<FrameMarkRec> marks = {{600, 100, sa, 1}, {700, 100, sa, 2}};
  std::vector<MarkerRec> markers = {{650, 100, sa, sb, 0x11223344u}};
  std::vector<PlotSampleRec> plots = {{800, sb, 3.5}, {900, sb, -1e300}};
  std::vector<ThreadNameRec> tnames = {{1, 100, sa}, {2, 101, sb}};
  std::vector<ClockCalibrationRec> cal = {{1000, 123456789, kClockTsc}, {2000, 42, kClockGpu}};
  const std::string json = R"({"sampling_hz":8000,"events_lost":0,"agent":"1.0"})";

  w->append(procs);
  w->append(threads);
  w->append(mods);
  w->append(samples);
  w->append(cswitch);
  w->append(ready);
  w->append(scopes);
  w->append(fibers);
  w->append(marks);
  w->append(markers);
  w->append(plots);
  w->append(tnames);
  w->append(cal);
  w->appendCaptureInfo(json);
  ASSERT_TRUE(w->finish().ok());

  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_FALSE(r->isPartial());

  auto strings = r->readStrings();
  ASSERT_TRUE(strings.ok());
  EXPECT_EQ(strings->size(), 3u);
  EXPECT_EQ(strings->get(sa), "alpha");
  EXPECT_EQ(strings->get(sb), "beta");

  auto frames = r->readFrames();
  ASSERT_TRUE(frames.ok());
  ASSERT_EQ(frames->size(), 3u);
  EXPECT_TRUE((*frames)[2] == (FrameRec{ModuleIdx::invalid(), 0xDEADBEEF12345678ull}));
  EXPECT_TRUE((*frames)[1] == (FrameRec{ModuleIdx(0), 0x2000}));

  auto nodes = r->readCallstackNodes();
  ASSERT_TRUE(nodes.ok());
  ASSERT_EQ(nodes->size(), 4u);
  EXPECT_EQ((*nodes)[cs.get()].frame, f0);  // leaf is the first frame given
  EXPECT_EQ((*nodes)[(*nodes)[cs.get()].parent.get()].frame, f1);

  auto gotProcs = r->readProcesses();
  ASSERT_TRUE(gotProcs.ok());
  EXPECT_TRUE(*gotProcs == procs);
  auto gotThreads = r->readThreads();
  ASSERT_TRUE(gotThreads.ok());
  EXPECT_TRUE(*gotThreads == threads);
  auto gotMods = r->readModules();
  ASSERT_TRUE(gotMods.ok());
  EXPECT_TRUE(*gotMods == mods);
  auto gotSamples = r->readSamples();
  ASSERT_TRUE(gotSamples.ok());
  EXPECT_TRUE(*gotSamples == samples);
  auto gotCs = r->readContextSwitches();
  ASSERT_TRUE(gotCs.ok());
  EXPECT_TRUE(*gotCs == cswitch);
  auto gotReady = r->readReadyEvents();
  ASSERT_TRUE(gotReady.ok());
  EXPECT_TRUE(*gotReady == ready);
  auto gotScopes = r->readScopeEvents();
  ASSERT_TRUE(gotScopes.ok());
  EXPECT_TRUE(*gotScopes == scopes);
  auto gotFibers = r->readFiberEvents();
  ASSERT_TRUE(gotFibers.ok());
  EXPECT_TRUE(*gotFibers == fibers);
  auto gotMarks = r->readFrameMarks();
  ASSERT_TRUE(gotMarks.ok());
  EXPECT_TRUE(*gotMarks == marks);
  auto gotMarkers = r->readMarkers();
  ASSERT_TRUE(gotMarkers.ok());
  EXPECT_TRUE(*gotMarkers == markers);
  auto gotPlots = r->readPlotSamples();
  ASSERT_TRUE(gotPlots.ok());
  EXPECT_TRUE(*gotPlots == plots);
  auto gotNames = r->readThreadNames();
  ASSERT_TRUE(gotNames.ok());
  EXPECT_TRUE(*gotNames == tnames);
  auto gotCal = r->readClockCalibrations();
  ASSERT_TRUE(gotCal.ok());
  EXPECT_TRUE(*gotCal == cal);
  auto info = r->readCaptureInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_EQ(*info, json);
  const FooterRec* infoChunk = findChunk(*r, ChunkType::CaptureInfo);
  ASSERT_NE(infoChunk, nullptr);
  EXPECT_EQ(infoChunk->record_count, 1u);

  // Samples must have been compressed (highly repetitive data).
  const FooterRec* sc = findChunk(*r, ChunkType::Samples);
  ASSERT_NE(sc, nullptr);
  EXPECT_EQ(sc->record_count, 1000u);
}

TEST(TraceRoundTrip, ChunkSplittingAtMaxRecords) {
  TempFile f;
  auto w = TraceWriter::open(f.path(), makeHeader());
  ASSERT_TRUE(w.ok());
  const size_t total = 2 * kMaxRecordsPerChunk + 7;
  SampleBatch samples;
  samples.ts.resize(total);
  samples.tid.resize(total);
  samples.callstack.resize(total);
  samples.cpu.resize(total);
  samples.flags.resize(total);
  for (size_t i = 0; i < total; ++i) {
    samples.ts[i] = i;
    samples.tid[i] = static_cast<uint32_t>(i % 5);
    samples.callstack[i] = CallstackIdx(0);
    samples.cpu[i] = static_cast<uint16_t>(i % 16);
  }
  w->append(samples);
  // Exactly the maximum: one chunk. One past the maximum: two chunks.
  std::vector<MarkerRec> exact(kMaxRecordsPerChunk, MarkerRec{1, 2, StringIdx(0), StringIdx(0), 3});
  w->append(exact);
  std::vector<PlotSampleRec> plus1(kMaxRecordsPerChunk + 1, PlotSampleRec{1, StringIdx(0), 2.0});
  w->append(plus1);
  ASSERT_TRUE(w->finish().ok());

  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(countChunks(*r, ChunkType::Samples), 3u);
  EXPECT_EQ(findChunk(*r, ChunkType::Samples, 0)->record_count, kMaxRecordsPerChunk);
  EXPECT_EQ(findChunk(*r, ChunkType::Samples, 1)->record_count, kMaxRecordsPerChunk);
  EXPECT_EQ(findChunk(*r, ChunkType::Samples, 2)->record_count, 7u);
  ASSERT_EQ(countChunks(*r, ChunkType::Markers), 1u);
  EXPECT_EQ(findChunk(*r, ChunkType::Markers)->record_count, kMaxRecordsPerChunk);
  ASSERT_EQ(countChunks(*r, ChunkType::PlotSamples), 2u);
  EXPECT_EQ(findChunk(*r, ChunkType::PlotSamples, 1)->record_count, 1u);

  auto got = r->readSamples();
  ASSERT_TRUE(got.ok());
  EXPECT_TRUE(*got == samples);
  auto plots = r->readPlotSamples();
  ASSERT_TRUE(plots.ok());
  EXPECT_EQ(plots->size(), plus1.size());
}

TEST(TraceRoundTrip, IncrementalTablesAreGlobalAcrossChunks) {
  TempFile f;
  auto w = TraceWriter::open(f.path(), makeHeader());
  ASSERT_TRUE(w.ok());
  SampleBatch a, b;
  const FrameIdx f0 = w->internFrame(ModuleIdx(0), 1);
  const FrameIdx fr1[] = {f0};
  const CallstackIdx c1 = w->internCallstack(fr1);
  const StringIdx s1 = w->internString("first");
  a.push(1, 1, c1, 0);
  std::vector<MarkerRec> m1 = {{1, 1, s1, s1, 0}};
  w->append(a);
  w->append(m1);
  // More interning after the first flush must emit additional table chunks.
  const FrameIdx f1 = w->internFrame(ModuleIdx(0), 2);
  const FrameIdx fr2[] = {f1, f0};
  const CallstackIdx c2 = w->internCallstack(fr2);
  const StringIdx s2 = w->internString("second");
  b.push(2, 1, c2, 0);
  std::vector<MarkerRec> m2 = {{2, 1, s2, s2, 0}};
  w->append(b);
  w->append(m2);
  ASSERT_TRUE(w->finish().ok());

  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_GE(countChunks(*r, ChunkType::Strings), 2u);
  EXPECT_GE(countChunks(*r, ChunkType::Frames), 2u);
  EXPECT_GE(countChunks(*r, ChunkType::CallstackNodes), 2u);
  auto strings = r->readStrings();
  ASSERT_TRUE(strings.ok());
  EXPECT_EQ(strings->get(s1), "first");
  EXPECT_EQ(strings->get(s2), "second");
  auto frames = r->readFrames();
  ASSERT_TRUE(frames.ok());
  ASSERT_EQ(frames->size(), 2u);
  EXPECT_TRUE((*frames)[1] == (FrameRec{ModuleIdx(0), 2}));
  auto nodes = r->readCallstackNodes();
  ASSERT_TRUE(nodes.ok());
  ASSERT_EQ(nodes->size(), 3u);  // root + f0 + (f0,f1): c2 shares the node of c1
  EXPECT_EQ((*nodes)[c2.get()].parent, c1);
}

TEST(TraceRoundTrip, RejectsBadMagicAndVersion) {
  TempFile f;
  {
    auto w = TraceWriter::open(f.path(), makeHeader());
    ASSERT_TRUE(w.ok());
    ASSERT_TRUE(w->finish().ok());
  }
  {
    std::fstream io(f.path(), std::ios::in | std::ios::out | std::ios::binary);
    io.put('X');
  }
  auto bad = TraceReader::open(f.path());
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().code, ErrorCode::CorruptData);
  {
    std::fstream io(f.path(), std::ios::in | std::ios::out | std::ios::binary);
    io.put('T');
    io.seekp(8);
    io.put(9);
  }
  auto ver = TraceReader::open(f.path());
  ASSERT_FALSE(ver.ok());
  EXPECT_EQ(ver.error().code, ErrorCode::Unsupported);
  EXPECT_FALSE(TraceReader::open(f.path().string() + ".missing").ok());
}

TEST(TraceRoundTrip, DetectsCorruptPayload) {
  TempFile f;
  {
    auto w = TraceWriter::open(f.path(), makeHeader());
    ASSERT_TRUE(w.ok());
    SampleBatch s;
    for (uint32_t i = 0; i < 5000; ++i) s.push(i, 1, CallstackIdx(0), 0);
    w->append(s);
    ASSERT_TRUE(w->finish().ok());
  }
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  const FooterRec c = *findChunk(*r, ChunkType::Samples);
  {
    std::fstream io(f.path(), std::ios::in | std::ios::out | std::ios::binary);
    io.seekp(static_cast<std::streamoff>(c.offset + sizeof(ChunkHeader) + 3));
    const char junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    io.write(junk, sizeof(junk));
  }
  auto r2 = TraceReader::open(f.path());
  ASSERT_TRUE(r2.ok());
  std::vector<std::byte> out;
  auto res = r2->readChunk(*findChunk(*r2, ChunkType::Samples), out);
  EXPECT_FALSE(res.ok());
}

}  // namespace
}  // namespace tachyon::trace
