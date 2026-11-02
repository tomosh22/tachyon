// WP-70: the spool (writer, reader, session.json, record streams) and the drain thread over fake rings.
#include "capture/linux_perf/drain.h"
#include "capture/linux_perf/spool.h"
#include "linux_perf_test_util.h"

#include <gtest/gtest.h>

#include <fstream>
#include <set>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;

using ptest::FakeRing;
using ptest::RefRing;
using ptest::TempDir;
using ptest::waitFor;

const RecordLayout kLayout{};

std::vector<uint8_t> samples(uint32_t pid, uint64_t firstTime, int count) {
  RecordBuilder b(kLayout);
  for (int i = 0; i < count; ++i) {
    RecordBuilder::Sample s;
    s.ip = 0x401000 + static_cast<uint64_t>(i);
    s.id = {pid, pid, firstTime + static_cast<uint64_t>(i) * 100, 0, 0, 0};
    s.chain = {kContextUser, s.ip, 0x401500};
    b.sample(s);
  }
  return b.take();
}

std::vector<uint8_t> commRecord(uint32_t pid, uint64_t time, const char* name) {
  RecordBuilder b(kLayout);
  b.comm(pid, pid, name, false, {pid, pid, time, 0, 0, 0});
  return b.take();
}

TEST(Spool, WriterReaderRoundTrip) {
  TempDir dir;
  const auto path = dir.path() / "data.bin";
  {
    auto w = SpoolWriter::create(path, SpoolKind::Data, kLayout);
    ASSERT_TRUE(w.ok());
    ASSERT_TRUE(w->append(0, samples(1, 100, 3)).ok());
    ASSERT_TRUE(w->append(1, samples(2, 150, 2)).ok());
    ASSERT_TRUE(w->append(0, std::span<const uint8_t>()).ok()) << "empty batches are ignored";
    ASSERT_TRUE(w->close().ok());
  }
  auto f = SpoolFile::open(path);
  ASSERT_TRUE(f.ok()) << f.error().toString();
  EXPECT_EQ(f->kind(), SpoolKind::Data);
  EXPECT_EQ(f->layout(), kLayout);
  EXPECT_FALSE(f->truncated());
  ASSERT_EQ(f->batches().size(), 2u);
  EXPECT_EQ(f->batches()[0].ring, 0u);
  EXPECT_EQ(f->batches()[1].ring, 1u);

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(f->read(f->batches()[1], bytes).ok());
  EXPECT_EQ(bytes, samples(2, 150, 2));
}

TEST(Spool, TruncatedTailKeepsWhatIsReadable) {
  TempDir dir;
  const auto path = dir.path() / "data.bin";
  {
    auto w = SpoolWriter::create(path, SpoolKind::Data, kLayout);
    ASSERT_TRUE(w.ok());
    ASSERT_TRUE(w->append(0, samples(1, 100, 4)).ok());
    ASSERT_TRUE(w->append(0, samples(1, 900, 4)).ok());
    ASSERT_TRUE(w->close().ok());
  }
  const auto size = fs::file_size(path);
  fs::resize_file(path, size - 20);  // cut into the second batch's payload
  auto f = SpoolFile::open(path);
  ASSERT_TRUE(f.ok());
  EXPECT_TRUE(f->truncated());
  ASSERT_EQ(f->batches().size(), 2u);
  EXPECT_LT(f->batches()[1].length, samples(1, 900, 4).size());

  // Cut inside the second batch header: only the first batch survives.
  fs::resize_file(path, sizeof(SpoolFileHeader) + sizeof(SpoolBatchHeader) + samples(1, 100, 4).size() + 5);
  auto g = SpoolFile::open(path);
  ASSERT_TRUE(g.ok());
  EXPECT_TRUE(g->truncated());
  EXPECT_EQ(g->batches().size(), 1u);
}

TEST(Spool, GarbageFilesAreRejectedOrClipped) {
  TempDir dir;
  {
    std::ofstream out(dir.path() / "tiny.bin", std::ios::binary);
    out << "abc";
  }
  EXPECT_FALSE(SpoolFile::open(dir.path() / "tiny.bin").ok());
  {
    std::ofstream out(dir.path() / "wrong.bin", std::ios::binary);
    out << std::string(100, 'x');
  }
  auto wrong = SpoolFile::open(dir.path() / "wrong.bin");
  ASSERT_FALSE(wrong.ok());
  EXPECT_EQ(wrong.error().code, ErrorCode::CorruptData);
  EXPECT_EQ(SpoolFile::open(dir.path() / "missing.bin").error().code, ErrorCode::NotFound);

  // A valid header followed by a batch header with a nonsense length.
  const auto path = dir.path() / "badlen.bin";
  {
    auto w = SpoolWriter::create(path, SpoolKind::Meta, kLayout);
    ASSERT_TRUE(w.ok());
    ASSERT_TRUE(w->append(0, commRecord(1, 5, "ok")).ok());
    ASSERT_TRUE(w->close().ok());
  }
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    SpoolBatchHeader bad{kSpoolBatchMagic, 0, ~uint64_t{0}};
    out.write(reinterpret_cast<const char*>(&bad), sizeof(bad));
  }
  auto f = SpoolFile::open(path);
  ASSERT_TRUE(f.ok());
  EXPECT_TRUE(f->truncated());
  EXPECT_EQ(f->batches().size(), 1u);
}

TEST(Spool, SessionInfoJsonRoundTrip) {
  SessionInfo s;
  s.layout = RecordLayout{kBackendSampleType, true};
  s.startTs = 111;
  s.endTs = 222;
  s.samplingHz = 8000;
  s.mode = "launch";
  s.rootPid = 4321;
  s.followChildren = true;
  s.childFilter = "worker*";
  s.exe = "/bin/prog";
  s.args = {"--a", "b c"};
  s.cpuCount = 8;
  s.ringCount = 8;
  s.plan = "system-wide";
  s.kernelStacks = false;
  s.excludeKernel = true;
  s.kernelBuildId = true;
  s.paranoid = -1;
  s.maxSampleRate = 100000;
  s.cpuTimeMaxPercent = 25;
  s.circularMb = 64;
  s.kernelRelease = "6.8.0";
  s.hostname = "host";
  s.agentVersion = "v";
  s.drainedBytes = 99;
  s.ringOverruns = 1;
  s.corruptBatches = 2;
  s.warnings = {"w1", "w2"};
  s.finished = true;
  auto back = SessionInfo::fromJson(s.toJson());
  ASSERT_TRUE(back.ok()) << back.error().toString();
  EXPECT_EQ(back->toJson(), s.toJson());
  EXPECT_EQ(back->args, s.args);
  EXPECT_EQ(back->rootPid, 4321u);
  EXPECT_FALSE(SessionInfo::fromJson("not json").ok());
  EXPECT_FALSE(SessionInfo::fromJson("[1,2]").ok());
  EXPECT_FALSE(SessionInfo::fromJson("{\"version\":7}").ok());
}

TEST(Spool, RecordStreamSkipsUnsupportedAndCountsMalformed) {
  TempDir dir;
  const auto path = dir.path() / "meta.bin";
  {
    auto w = SpoolWriter::create(path, SpoolKind::Meta, kLayout);
    ASSERT_TRUE(w.ok());
    RecordBuilder b(kLayout);
    b.comm(1, 1, "one", false, {1, 1, 10, 0, 0, 0});
    b.raw(kRecordKsymbol, 0, 16, {1, 1, 11, 0, 0, 0});
    b.comm(2, 2, "two", false, {2, 2, 12, 0, 0, 0});
    ASSERT_TRUE(w->append(0, b.bytes()).ok());
    // A batch whose second record has a bogus size: everything after it is dropped, the first record
    // survives.
    std::vector<uint8_t> bad = commRecord(3, 13, "three");
    const std::vector<uint8_t> junk = {1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // size 3
    bad.insert(bad.end(), junk.begin(), junk.end());
    ASSERT_TRUE(w->append(0, bad).ok());
    ASSERT_TRUE(w->close().ok());
  }
  auto f = SpoolFile::open(path);
  ASSERT_TRUE(f.ok());
  std::vector<RecordStream::Extent> extents;
  for (const auto& b : f->batches()) extents.push_back({&f.value(), b});
  RecordStream stream(kLayout, extents);
  std::vector<std::string> names;
  while (stream.advance()) names.emplace_back(stream.current().name);
  EXPECT_EQ(names, (std::vector<std::string>{"one", "two", "three"}));
  EXPECT_EQ(stream.skippedUnsupported(), 1u);
  EXPECT_EQ(stream.malformedBatches(), 1u);
}

TEST(Spool, SpoolSetFindsFilesInOrder) {
  TempDir dir;
  SessionInfo info;
  info.layout = kLayout;
  ASSERT_TRUE(writeSessionInfo(dir.path(), info).ok());
  for (uint32_t i : {2u, 0u, 10u, 1u}) {
    auto w = SpoolWriter::create(spoolDataPath(dir.path(), i), SpoolKind::Data, kLayout);
    ASSERT_TRUE(w.ok());
  }
  {
    auto w = SpoolWriter::create(spoolMetaPath(dir.path()), SpoolKind::Meta, kLayout);
    ASSERT_TRUE(w.ok());
  }
  auto set = SpoolSet::openDirectory(dir.path());
  ASSERT_TRUE(set.ok()) << set.error().toString();
  ASSERT_EQ(set->data.size(), 4u);
  EXPECT_EQ(set->data[0].filename(), "data-000000.bin");
  EXPECT_EQ(set->data[3].filename(), "data-000010.bin");
  EXPECT_EQ(set->meta.filename(), "meta.bin");
  EXPECT_FALSE(SpoolSet::openDirectory(dir.path() / "nope").ok());
}

// --- drain -----------------------------------------------------------------------------------------------

TEST(Drain, SplitsSamplesFromMetaAndTagsRings) {
  TempDir dir;
  FakeRing ring0, ring1;
  DrainConfig dc;
  dc.layout = kLayout;
  dc.spoolDir = dir.path();
  DescendantTracker tracker;
  tracker.setRoots({100}, true);
  RefRing r0(ring0), r1(ring1);
  Drain real(dc, {&r0, &r1}, &tracker);
  ASSERT_TRUE(real.open().ok());
  real.start();

  std::vector<uint8_t> mixed = samples(100, 1000, 2);
  RecordBuilder b(kLayout);
  b.comm(100, 100, "root", true, {100, 100, 1500, 0, 0, 0});
  b.fork(101, 100, 101, 100, 1600, {100, 100, 1600, 0, 0, 0});
  b.lost(0, 7, {100, 100, 1700, 0, 0, 0});
  b.throttle(false, 1800, 0, 0, {100, 100, 1800, 0, 0, 0});
  mixed.insert(mixed.end(), b.bytes().begin(), b.bytes().end());
  ring0.push(mixed);
  ring1.push(samples(200, 1200, 3));

  ASSERT_TRUE(waitFor([&] { return real.stats().samples == 5; }));
  ASSERT_TRUE(real.finish().ok());
  const DrainStats st = real.stats();
  EXPECT_EQ(st.samples, 5u);
  EXPECT_EQ(st.metaRecords, 4u);
  EXPECT_EQ(st.lostRecords, 1u);
  EXPECT_EQ(st.lostEvents, 7u);
  EXPECT_EQ(st.throttleRecords, 1u);
  EXPECT_FALSE(st.ioFailed);
  EXPECT_EQ(tracker.pids(), (std::vector<uint32_t>{100, 101})) << "a followed fork joins the tracked set";

  auto meta = SpoolFile::open(spoolMetaPath(dir.path()));
  ASSERT_TRUE(meta.ok());
  EXPECT_EQ(meta->kind(), SpoolKind::Meta);
  auto data = SpoolFile::open(spoolDataPath(dir.path(), 0));
  ASSERT_TRUE(data.ok());
  uint64_t dataBytes = 0;
  std::set<uint32_t> rings;
  for (const auto& batch : data->batches()) {
    dataBytes += batch.length;
    rings.insert(batch.ring);
  }
  EXPECT_EQ(rings, (std::set<uint32_t>{0, 1}));
  EXPECT_EQ(dataBytes, samples(100, 1000, 2).size() + samples(200, 1200, 3).size());
}

TEST(Drain, TrackerIgnoresThreadsAndUnrelatedProcesses) {
  DescendantTracker t;
  t.setRoots({10}, true);
  t.onFork(11, 10, 12);  // a thread (tid != pid)
  t.onFork(20, 99, 20);  // child of an unrelated process
  t.onFork(30, 10, 30);
  t.onFork(31, 30, 31);  // grandchild
  EXPECT_EQ(t.pids(), (std::vector<uint32_t>{10, 30, 31}));
  DescendantTracker nofollow;
  nofollow.setRoots({10}, false);
  nofollow.onFork(30, 10, 30);
  EXPECT_EQ(nofollow.pids(), std::vector<uint32_t>{10});
}

TEST(Drain, MalformedChunkIsCountedAndGoodRecordsBeforeItKept) {
  TempDir dir;
  FakeRing ring;
  RefRing r(ring);
  DrainConfig dc;
  dc.layout = kLayout;
  dc.spoolDir = dir.path();
  Drain drain(dc, {&r});
  ASSERT_TRUE(drain.open().ok());
  drain.start();
  std::vector<uint8_t> chunk = samples(5, 10, 2);
  chunk.insert(chunk.end(), {1, 0, 0, 0, 0, 0, 5, 0});  // header with size 5
  ring.push(chunk);
  ASSERT_TRUE(waitFor([&] { return drain.stats().malformedChunks == 1; }));
  ASSERT_TRUE(drain.finish().ok());
  EXPECT_EQ(drain.stats().samples, 2u);
}

TEST(Drain, CircularModeRotatesAndDropsOldSegments) {
  TempDir dir;
  FakeRing ring;
  RefRing r(ring);
  DrainConfig dc;
  dc.layout = kLayout;
  dc.spoolDir = dir.path();
  dc.circularMb = 2;  // segments of 1 MiB (the minimum), at most ~2 MiB kept
  Drain drain(dc, {&r});
  ASSERT_TRUE(drain.open().ok());
  drain.start();
  const std::vector<uint8_t> chunk = samples(5, 10, 8000);  // ~ 0.5 MB
  uint64_t pushed = 0;
  for (int i = 0; i < 24; ++i) {
    ring.push(chunk);
    pushed += 8000;
    ASSERT_TRUE(waitFor([&] { return drain.stats().samples == pushed; }));
  }
  ASSERT_TRUE(drain.finish().ok());
  const DrainStats st = drain.stats();
  EXPECT_GT(st.segmentsWritten, 3u);
  EXPECT_GT(st.segmentsDropped, 0u);
  uint64_t total = 0;
  size_t files = 0;
  for (const auto& e : fs::directory_iterator(dir.path())) {
    if (e.path().filename().string().rfind("data-", 0) == 0) {
      total += fs::file_size(e.path());
      ++files;
    }
  }
  EXPECT_LE(total, 4u << 20) << "the retained segments stay near the budget";
  EXPECT_EQ(files, drain.segments().size());
  EXPECT_TRUE(fs::exists(spoolMetaPath(dir.path()))) << "metadata is never rotated away";
}

TEST(Drain, SealForFlushReturnsSealedSegmentsAndReleaseDeletesThem) {
  TempDir dir;
  FakeRing ring;
  RefRing r(ring);
  DrainConfig dc;
  dc.layout = kLayout;
  dc.spoolDir = dir.path();
  dc.circularMb = 64;
  Drain drain(dc, {&r});
  ASSERT_TRUE(drain.open().ok());
  drain.start();

  auto empty = drain.sealForFlush();
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty->empty()) << "nothing recorded yet: nothing to seal";

  ring.push(samples(5, 100, 10));
  ASSERT_TRUE(waitFor([&] { return drain.stats().samples == 10; }));
  auto sealed = drain.sealForFlush();
  ASSERT_TRUE(sealed.ok());
  ASSERT_EQ(sealed->size(), 1u);
  EXPECT_EQ((*sealed)[0].firstTs, 100u);
  EXPECT_EQ((*sealed)[0].lastTs, 100u + 9 * 100);
  auto f = SpoolFile::open((*sealed)[0].path);
  ASSERT_TRUE(f.ok());
  EXPECT_FALSE(f->truncated());

  ring.push(samples(5, 5000, 3));
  ASSERT_TRUE(waitFor([&] { return drain.stats().samples == 13; }));
  drain.release(*sealed);
  EXPECT_FALSE(fs::exists((*sealed)[0].path));
  ASSERT_TRUE(drain.finish().ok());
  EXPECT_EQ(drain.segments().size(), 1u) << "the live segment survives the release";
}

}  // namespace
}  // namespace tachyon::capture::perf
