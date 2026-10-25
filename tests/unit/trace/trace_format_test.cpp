#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace tachyon::trace {
namespace {

TEST(TraceFormat, RecordSizes) {
  EXPECT_EQ(sizeof(FileHeader), 64u);
  EXPECT_EQ(sizeof(ChunkHeader), 32u);
  EXPECT_EQ(sizeof(FrameRec), 12u);
  EXPECT_EQ(sizeof(CallstackNodeRec), 8u);
  EXPECT_EQ(sizeof(ModuleRec), 78u);
  EXPECT_EQ(sizeof(FooterRec), 24u);
}

TEST(TraceFormat, SoaSampleRowIsTwentyBytes) {
  EXPECT_EQ(kSampleBytes, sizeof(uint64_t) + 2 * sizeof(uint32_t) + 2 * sizeof(uint16_t));
}

TEST(TraceFormat, HeaderDefaults) {
  FileHeader h;
  EXPECT_EQ(std::memcmp(h.magic, "TACHYON\0", 8), 0);
  EXPECT_EQ(h.format_version, 1u);
  EXPECT_EQ(h.footer_offset, 0u);
}

TEST(TraceFormat, ChunkTypeValues) {
  EXPECT_EQ(static_cast<uint32_t>(ChunkType::Strings), 1u);
  EXPECT_EQ(static_cast<uint32_t>(ChunkType::CaptureInfo), 17u);
  EXPECT_EQ(static_cast<uint32_t>(ChunkType::Footer), 0xFFFFu);
  EXPECT_EQ(kMaxRecordsPerChunk, 1'048'576u);
}

TEST(TraceFormat, RecordsAreTriviallyCopyable) {
  static_assert(std::is_trivially_copyable_v<FileHeader>);
  static_assert(std::is_trivially_copyable_v<ModuleRec>);
  static_assert(std::is_trivially_copyable_v<ScopeEventRec>);
  SUCCEED();
}

}  // namespace
}  // namespace tachyon::trace
