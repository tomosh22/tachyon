#include "inst/spool.h"

#include "../control/test_util.h"
#include "inst/decoder.h"
#include "packed.h"
#include "wire_builder.h"

#include <gtest/gtest.h>

#include <cstring>

#include <fstream>
#include <iterator>

namespace tachyon::inst {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using testing_util::pv;
using testing_util::StreamBuilder;

std::vector<uint8_t> readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Feeds a built stream into a spool frame by frame, as the ingest does.
void feed(Spool& spool, const StreamBuilder& b) {
  FrameAssembler a;
  a.feed(b.bytes().data(), b.bytes().size(), [&](const uint8_t* f, size_t n) { spool.appendFrame(f, n); });
}

TEST(Spool, StoresFramesByteForByte) {
  TempDir dir;
  StreamBuilder b;
  b.header();
  const uint64_t key = b.string("work");
  for (uint64_t i = 0; i < 100; ++i) {
    b.scopeBegin(100 + i * 10, 5, key);
    b.scopeEnd(105 + i * 10, 5);
  }
  auto spool = Spool::create(dir.path() / "s.tachyon-inst", kDefaultMaxSpoolBytes);
  ASSERT_TRUE(spool.ok());
  (*spool)->noteConnection();
  feed(**spool, b);
  ASSERT_TRUE((*spool)->close().ok());
  EXPECT_EQ(readAll(dir.path() / "s.tachyon-inst"), b.bytes());
  const SpoolStats st = (*spool)->stats();
  EXPECT_EQ(st.framesWritten, b.frames());
  EXPECT_EQ(st.bytesWritten, b.bytes().size());
  EXPECT_FALSE(st.overflowed);
  EXPECT_EQ(st.connections, 1u);
}

TEST(Spool, FlushReturnsASizeAtAFrameBoundary) {
  TempDir dir;
  StreamBuilder b;
  b.header();
  const uint64_t key = b.string("w");
  b.scopeBegin(10, 1, key);
  auto spool = Spool::create(dir.path() / "s.tachyon-inst", kDefaultMaxSpoolBytes);
  ASSERT_TRUE(spool.ok());
  feed(**spool, b);
  auto size = (*spool)->flush();
  ASSERT_TRUE(size.ok());
  EXPECT_EQ(*size, b.bytes().size());
  EXPECT_EQ(fs::file_size(dir.path() / "s.tachyon-inst"), b.bytes().size());
  // More frames after a flush keep appending.
  StreamBuilder more;
  more.scopeEnd(20, 1);
  feed(**spool, more);
  ASSERT_TRUE((*spool)->close().ok());
  EXPECT_EQ(fs::file_size(dir.path() / "s.tachyon-inst"), b.bytes().size() + more.bytes().size());
}

TEST(Spool, CapDropsLaterFramesAndCloseAppendsADroppedFrame) {
  TempDir dir;
  StreamBuilder b;
  b.header();
  const uint64_t key = b.string("work");
  for (uint64_t i = 0; i < 50; ++i) b.scopeBegin(1000 + i, 7, key);
  b.scopeEnd(2000, 7);
  // Room for the header, the StringDef and ten ScopeBegins.
  StreamBuilder prefix;
  prefix.header();
  prefix.string("work");
  for (uint64_t i = 0; i < 10; ++i) prefix.scopeBegin(1000 + i, 7, key);
  auto spool = Spool::create(dir.path() / "s.tachyon-inst", prefix.bytes().size());
  ASSERT_TRUE(spool.ok());
  feed(**spool, b);
  ASSERT_TRUE((*spool)->close().ok());
  const SpoolStats st = (*spool)->stats();
  EXPECT_TRUE(st.overflowed);
  EXPECT_EQ(st.framesDropped, 41u);
  EXPECT_EQ(st.recordsDropped, 41u);

  // The file is the prefix plus one Dropped frame that decodes to a tachyon.dropped marker at the first
  // dropped record's time on its thread.
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(dir.path() / "s.tachyon-inst").ok());
  const DecodedInstrumentation out = decoder.finish();
  EXPECT_EQ(out.stats.scopeBegins, 10u);
  EXPECT_EQ(out.stats.droppedRecords, 41u);
  ASSERT_EQ(out.markers.size(), 1u);
  const trace::MarkerRec m = out.markers[0];
  EXPECT_EQ(out.strings.get(m.name), kDroppedMarkerName);
  EXPECT_EQ(out.strings.get(m.data), "41");
  EXPECT_EQ(pv(m.tid), 7u);
  EXPECT_EQ(pv(m.ts), 1010u);
}

TEST(Spool, DroppedFramesPastTheCapAddTheirOwnCount) {
  TempDir dir;
  StreamBuilder b;
  b.header();
  auto spool = Spool::create(dir.path() / "s.tachyon-inst", b.bytes().size());
  ASSERT_TRUE(spool.ok());
  StreamBuilder over;
  over.header();
  over.dropped(500, 3, 25);  // a ring overflow in the client, arriving after the cap
  over.scopeEnd(600, 3);
  feed(**spool, over);
  ASSERT_TRUE((*spool)->close().ok());
  EXPECT_EQ((*spool)->stats().recordsDropped, 26u);
}

TEST(Spool, OverflowIsStickyEvenForSmallLaterFrames) {
  TempDir dir;
  StreamBuilder head;
  head.header();
  // Room for the header and 30 more bytes: a big frame does not fit, a small one afterwards would.
  auto spool = Spool::create(dir.path() / "s.tachyon-inst", head.bytes().size() + 30);
  ASSERT_TRUE(spool.ok());
  StreamBuilder b;
  b.header();
  b.marker(10, 1, 0x42, std::string(100, 'x'));  // 134 bytes: refused
  b.scopeEnd(20, 1);                             // 20 bytes: would fit, but the stream is cut here
  feed(**spool, b);
  ASSERT_TRUE((*spool)->close().ok());
  const SpoolStats st = (*spool)->stats();
  EXPECT_TRUE(st.overflowed);
  EXPECT_EQ(st.framesDropped, 2u);
  EXPECT_EQ(st.recordsDropped, 2u);
  // The file: header, then the synthetic Dropped frame.
  InstDecoder decoder;
  ASSERT_TRUE(decoder.addFile(dir.path() / "s.tachyon-inst").ok());
  const DecodedInstrumentation out = decoder.finish();
  EXPECT_EQ(out.stats.scopeEnds, 0u);
  EXPECT_EQ(out.stats.droppedRecords, 2u);
}

TEST(Spool, TruncatedTailAndCorruptBytesAreCounted) {
  TempDir dir;
  auto spool = Spool::create(dir.path() / "s.tachyon-inst", kDefaultMaxSpoolBytes);
  ASSERT_TRUE(spool.ok());
  (*spool)->noteTruncatedTail(17);
  (*spool)->noteCorrupt(99);
  const SpoolStats st = (*spool)->stats();
  EXPECT_EQ(st.truncatedTailBytes, 17u);
  EXPECT_EQ(st.corruptBytes, 99u);
}

TEST(Spool, AppendAfterCloseIsIgnored) {
  TempDir dir;
  auto spool = Spool::create(dir.path() / "s.tachyon-inst", kDefaultMaxSpoolBytes);
  ASSERT_TRUE(spool.ok());
  ASSERT_TRUE((*spool)->close().ok());
  StreamBuilder b;
  b.header();
  feed(**spool, b);
  EXPECT_EQ((*spool)->stats().framesWritten, 0u);
  EXPECT_TRUE((*spool)->close().ok());  // idempotent
}

}  // namespace
}  // namespace tachyon::inst
