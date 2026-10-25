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
  h.platform = kPlatformLinux;
  return h;
}

std::vector<ThreadRec> threads() {
  return {{1, 1, 0, 0, StringIdx(0), 0}, {2, 1, 0, 0, StringIdx(0), 0}};
}

SampleBatch samples(size_t n) {
  SampleBatch s;
  for (size_t i = 0; i < n; ++i) s.push(i, 1, CallstackIdx(0), 0);
  return s;
}

// Writes an unfinished file (writer destroyed without finish()) with three data chunks.
void writeUnfinished(const std::filesystem::path& p) {
  auto w = TraceWriter::open(p, makeHeader());
  ASSERT_TRUE(w.ok());
  w->append(std::span<const ThreadRec>(threads()));
  w->append(samples(100));
  w->append(samples(50));
}

TEST(TracePartial, UnfinishedFileIsRecovered) {
  TempFile f;
  writeUnfinished(f.path());
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_TRUE(r->isPartial());
  EXPECT_NE(r->header().flags & kFlagPartial, 0u);
  EXPECT_EQ(r->header().footer_offset, 0u);
  auto s = r->readSamples();
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->size(), 150u);
  auto t = r->readThreads();
  ASSERT_TRUE(t.ok());
  EXPECT_TRUE(*t == threads());
  auto strings = r->readStrings();
  ASSERT_TRUE(strings.ok());
}

TEST(TracePartial, TruncatedLastChunkIsDropped) {
  TempFile f;
  writeUnfinished(f.path());
  auto full = TraceReader::open(f.path());
  ASSERT_TRUE(full.ok());
  const size_t n = full->chunks().size();
  const FooterRec last = full->chunks().back();
  // Cut in the middle of the last chunk's payload.
  std::filesystem::resize_file(f.path(), last.offset + sizeof(ChunkHeader) + 5);
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->isPartial());
  EXPECT_EQ(r->chunks().size(), n - 1);
  auto s = r->readSamples();
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->size(), 100u);
}

TEST(TracePartial, TruncatedChunkHeaderAndGarbageTail) {
  TempFile f;
  writeUnfinished(f.path());
  auto full = TraceReader::open(f.path());
  ASSERT_TRUE(full.ok());
  const FooterRec last = full->chunks().back();
  std::filesystem::resize_file(f.path(), last.offset + 10);  // header cut
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->chunks().size(), full->chunks().size() - 1);

  // Garbage bytes after the last good chunk stop the scan without error.
  std::filesystem::resize_file(f.path(), last.offset);
  {
    std::ofstream out(f.path(), std::ios::binary | std::ios::app);
    const std::string junk(200, '\xAB');
    out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  }
  auto r2 = TraceReader::open(f.path());
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(r2->chunks().size(), full->chunks().size() - 1);
}

TEST(TracePartial, HeaderOnlyFile) {
  TempFile f;
  {
    auto w = TraceWriter::open(f.path(), makeHeader());
    ASSERT_TRUE(w.ok());
  }
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->isPartial());
  EXPECT_TRUE(r->chunks().empty());
  auto s = r->readSamples();
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->size(), 0u);
}

TEST(TracePartial, FooterTruncatedFallsBackToScan) {
  TempFile f;
  {
    auto w = TraceWriter::open(f.path(), makeHeader());
    ASSERT_TRUE(w.ok());
    w->append(samples(20));
    ASSERT_TRUE(w->finish().ok());
  }
  auto good = TraceReader::open(f.path());
  ASSERT_TRUE(good.ok());
  ASSERT_FALSE(good->isPartial());
  const uint64_t footer = good->header().footer_offset;
  std::filesystem::resize_file(f.path(), footer + 4);
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->isPartial());
  EXPECT_EQ(r->chunks().size(), good->chunks().size());
  auto s = r->readSamples();
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->size(), 20u);
}

TEST(TracePartial, FooterWrittenButHeaderNotRewritten) {
  // Simulates a crash after the footer chunk but before the header rewrite: footer_offset == 0.
  TempFile f;
  {
    auto w = TraceWriter::open(f.path(), makeHeader());
    ASSERT_TRUE(w.ok());
    w->append(samples(20));
    ASSERT_TRUE(w->finish().ok());
  }
  FileHeader h;
  {
    std::fstream io(f.path(), std::ios::in | std::ios::out | std::ios::binary);
    io.read(reinterpret_cast<char*>(&h), sizeof(h));
    h.footer_offset = 0;
    h.flags |= kFlagPartial;
    io.seekp(0);
    io.write(reinterpret_cast<const char*>(&h), sizeof(h));
  }
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->isPartial());
  auto s = r->readSamples();
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->size(), 20u);
}

}  // namespace
}  // namespace tachyon::trace
