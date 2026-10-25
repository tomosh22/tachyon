// Hostile/corrupt inputs must produce error Results, never exceptions or huge allocations.
#include "test_util.h"
#include "trace/trace_reader.h"
#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

namespace tachyon::trace {
namespace {

using testing_util::TempFile;

constexpr uint64_t kWrap = uint64_t(1) << 61;  // times 24 wraps to a multiple of 2^64

void patchU64(const std::filesystem::path& p, uint64_t offset, uint64_t value) {
  std::fstream io(p, std::ios::in | std::ios::out | std::ios::binary);
  io.seekp(static_cast<std::streamoff>(offset));
  io.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

uint64_t readU64(const std::filesystem::path& p, uint64_t offset) {
  std::ifstream io(p, std::ios::binary);
  io.seekg(static_cast<std::streamoff>(offset));
  uint64_t v = 0;
  io.read(reinterpret_cast<char*>(&v), sizeof(v));
  return v;
}

void writeFile(const std::filesystem::path& p) {
  FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  auto w = TraceWriter::open(p, h);
  ASSERT_TRUE(w.ok());
  std::vector<MarkerRec> markers(10, MarkerRec{1, 2, StringIdx(0), StringIdx(0), 3});
  w->append(markers);
  SampleBatch s;
  for (uint32_t i = 0; i < 5000; ++i) s.push(i, 1, CallstackIdx(0), 0);
  w->append(s);
  ASSERT_TRUE(w->finish().ok());
}

TEST(TraceCorrupt, WrappedFooterRecordCountDoesNotThrow) {
  TempFile f;
  writeFile(f.path());
  uint64_t footer;
  {
    auto r = TraceReader::open(f.path());
    ASSERT_TRUE(r.ok());
    footer = r->header().footer_offset;
  }
  const uint64_t uncompressed = readU64(f.path(), footer + 8);
  patchU64(f.path(), footer + 24, uncompressed / sizeof(FooterRec) + kWrap);
  // Must not throw; the bad footer is rejected and the reader falls back to scanning.
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->isPartial());
  auto m = r->readMarkers();
  ASSERT_TRUE(m.ok());
  EXPECT_EQ(m->size(), 10u);
}

TEST(TraceCorrupt, WrappedDataChunkRecordCountIsAnError) {
  TempFile f;
  writeFile(f.path());
  uint64_t footer = 0, markerOffset = 0;
  size_t markerIdx = 0;
  {
    auto r = TraceReader::open(f.path());
    ASSERT_TRUE(r.ok());
    footer = r->header().footer_offset;
    for (size_t i = 0; i < r->chunks().size(); ++i)
      if (r->chunks()[i].type == static_cast<uint32_t>(ChunkType::Markers)) {
        markerOffset = r->chunks()[i].offset;
        markerIdx = i;
      }
  }
  ASSERT_NE(markerOffset, 0u);
  const uint64_t wrapped = 10 + kWrap;
  patchU64(f.path(), markerOffset + 24, wrapped);                                   // chunk header
  patchU64(f.path(), footer + sizeof(ChunkHeader) + markerIdx * 24 + 16, wrapped);  // footer record
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  auto m = r->readMarkers();
  // The footer is rejected (count over the per-chunk maximum), the scan drops the bad chunk: no data, no
  // throw.
  EXPECT_TRUE(r->isPartial());
  if (m.ok()) EXPECT_TRUE(m->empty());
  // The per-chunk decoders reject wrapped counts directly too.
  std::vector<MarkerRec> out;
  FooterRec rec{static_cast<uint32_t>(ChunkType::Markers), 1, markerOffset, wrapped};
  std::vector<std::byte> payload(10 * sizeof(MarkerRec));
  EXPECT_FALSE(TraceReader::decodeRecords<MarkerRec>(rec, payload, out).ok());
  SampleBatch sb;
  rec.record_count = (uint64_t(1) << 62) + 5000 / 20;  // * 20 wraps to the payload size
  EXPECT_FALSE(TraceReader::decodeSamples(rec, std::vector<std::byte>(5000), sb).ok());
}

TEST(TraceCorrupt, DecompressionBombIsRejectedBeforeAllocation) {
  TempFile f;
  writeFile(f.path());
  uint64_t samplesOffset = 0;
  {
    auto r = TraceReader::open(f.path());
    ASSERT_TRUE(r.ok());
    for (const FooterRec& c : r->chunks())
      if (c.type == static_cast<uint32_t>(ChunkType::Samples)) samplesOffset = c.offset;
  }
  ASSERT_NE(samplesOffset, 0u);
  patchU64(f.path(), samplesOffset + 8, uint64_t(1) << 29);  // claim 512 MiB from a tiny zstd frame
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  std::vector<std::byte> out;
  for (const FooterRec& c : r->chunks())
    if (c.type == static_cast<uint32_t>(ChunkType::Samples)) {
      EXPECT_FALSE(r->readChunk(c, out).ok());
      EXPECT_LT(out.capacity(), size_t(1) << 20);
    }
  patchU64(f.path(), samplesOffset + 8, uint64_t(1) << 40);  // beyond the cap: rejected structurally
  auto r2 = TraceReader::open(f.path());
  ASSERT_TRUE(r2.ok());
  EXPECT_FALSE(r2->readSamples().ok());
}

TEST(TraceCorrupt, HugeOffsetsDoNotWrap) {
  TempFile f;
  writeFile(f.path());
  patchU64(f.path(), 8 + 4 + 4 + 8 + 8 + 8, ~uint64_t(0) - 8);  // FileHeader::footer_offset
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->isPartial());
  std::vector<std::byte> out;
  const FooterRec bad{static_cast<uint32_t>(ChunkType::Markers), 1, ~uint64_t(0) - 8, 1};
  EXPECT_FALSE(r->readChunk(bad, out).ok());
}

TEST(TraceCorrupt, TablesFlushSplitAtMaxRecords) {
  TempFile f;
  auto w = TraceWriter::open(f.path(), FileHeader{});
  ASSERT_TRUE(w.ok());
  for (uint64_t i = 1; i < kMaxRecordsPerChunk + 5; ++i) w->internString(std::to_string(i));  // + "" at 0
  for (uint64_t i = 0; i < kMaxRecordsPerChunk + 3; ++i) w->internFrame(ModuleIdx(0), i);
  for (uint32_t i = 0; i < kMaxRecordsPerChunk + 2; ++i) w->internCallstackNode(CallstackIdx(0), FrameIdx(i));
  ASSERT_TRUE(w->finish().ok());
  auto r = TraceReader::open(f.path());
  ASSERT_TRUE(r.ok());
  std::vector<uint64_t> strings, frames, nodes;
  for (const FooterRec& c : r->chunks()) {
    if (c.type == static_cast<uint32_t>(ChunkType::Strings)) strings.push_back(c.record_count);
    if (c.type == static_cast<uint32_t>(ChunkType::Frames)) frames.push_back(c.record_count);
    if (c.type == static_cast<uint32_t>(ChunkType::CallstackNodes)) nodes.push_back(c.record_count);
  }
  EXPECT_EQ(strings, (std::vector<uint64_t>{kMaxRecordsPerChunk, 5}));
  EXPECT_EQ(frames, (std::vector<uint64_t>{kMaxRecordsPerChunk, 3}));
  EXPECT_EQ(nodes, (std::vector<uint64_t>{kMaxRecordsPerChunk, 3}));  // root + (kMax + 2) nodes
  auto table = r->readStrings();
  ASSERT_TRUE(table.ok());
  EXPECT_EQ(table->size(), kMaxRecordsPerChunk + 5);
  EXPECT_EQ(table->get(StringIdx(static_cast<uint32_t>(kMaxRecordsPerChunk + 4))),
            std::to_string(kMaxRecordsPerChunk + 4));
}

}  // namespace
}  // namespace tachyon::trace
