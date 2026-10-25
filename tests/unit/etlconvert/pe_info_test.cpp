#include "test_util.h"

#include <fstream>

namespace tachyon::etlconvert {
namespace {

using namespace testing;

TEST(PeInfo, ReadsRsdsFromPe32Plus) {
  PeSpec spec;
  const ByteVec pe = buildPe(spec);
  auto info = parsePe(pe);
  ASSERT_TRUE(info.ok()) << info.error().toString();
  EXPECT_TRUE(info->is64);
  EXPECT_EQ(info->machine, 0x8664);
  EXPECT_EQ(info->timeDateStamp, spec.timeDateStamp);
  EXPECT_EQ(info->sizeOfImage, spec.sizeOfImage);
  EXPECT_EQ(info->checksum, 0xABCDu);
  ASSERT_TRUE(info->hasCodeView);
  EXPECT_EQ(std::memcmp(info->guid, spec.guid, 16), 0);
  EXPECT_EQ(info->age, 3u);
  EXPECT_EQ(info->pdbPath, spec.pdb);
}

TEST(PeInfo, ReadsRsdsFromPe32) {
  PeSpec spec;
  spec.is64 = false;
  spec.age = 1;
  auto info = parsePe(buildPe(spec));
  ASSERT_TRUE(info.ok()) << info.error().toString();
  EXPECT_FALSE(info->is64);
  ASSERT_TRUE(info->hasCodeView);
  EXPECT_EQ(info->age, 1u);
  EXPECT_EQ(info->pdbPath, spec.pdb);
}

TEST(PeInfo, NoDebugDirectoryIsNotAnError) {
  PeSpec spec;
  spec.codeView = false;
  auto info = parsePe(buildPe(spec));
  ASSERT_TRUE(info.ok());
  EXPECT_FALSE(info->hasCodeView);
  EXPECT_EQ(info->timeDateStamp, spec.timeDateStamp);
}

TEST(PeInfo, RejectsNonPe) {
  const ByteVec junk(256, std::byte{0x41});
  auto info = parsePe(junk);
  ASSERT_FALSE(info.ok());
  EXPECT_EQ(info.error().code, ErrorCode::CorruptData);
  EXPECT_FALSE(parsePe(Bytes{}).ok());
}

TEST(PeInfo, TruncatedFilesFailCleanlyAtEveryLength) {
  PeSpec spec;
  const ByteVec pe = buildPe(spec);
  for (size_t len = 0; len < 0x440 + 24; len += 7) {
    auto info = parsePe(Bytes(pe.data(), len));
    if (info.ok()) EXPECT_FALSE(info->hasCodeView) << len;  // the RSDS record is not fully present
  }
}

TEST(PeInfo, HostileValuesDoNotCrash) {
  PeSpec spec;
  const ByteVec pe = buildPe(spec);
  // Absurd e_lfanew, section count, debug directory pointing outside the file.
  ByteVec a = pe;
  const uint32_t huge = 0xFFFFFF00;
  std::memcpy(a.data() + 0x3C, &huge, 4);
  EXPECT_FALSE(parsePe(a).ok());
  ByteVec b = pe;
  const uint16_t sections = 5000;
  std::memcpy(b.data() + 0x84 + 2, &sections, 2);
  EXPECT_FALSE(parsePe(b).ok());
  ByteVec c = pe;
  std::memcpy(c.data() + 0x400 + 24, &huge, 4);  // PointerToRawData
  auto info = parsePe(c);
  ASSERT_TRUE(info.ok());
  EXPECT_FALSE(info->hasCodeView);
}

TEST(PeInfo, LongPdbPathIsBounded) {
  PeSpec spec;
  spec.pdb = std::string(300, 'p');
  auto info = parsePe(buildPe(spec));
  ASSERT_TRUE(info.ok());
  EXPECT_EQ(info->pdbPath.size(), 300u);
}

TEST(PeInfo, FunctionRangesFromPdata) {
  PeSpec spec;
  spec.functions = {{0x3000, 0x3040}, {0x2000, 0x2100}, {0x2100, 0x2180}};
  const ByteVec pe = buildPe(spec);
  PeReadFn read = [&](uint64_t off, std::span<std::byte> dst) {
    if (off > pe.size() || pe.size() - off < dst.size()) return false;
    std::memcpy(dst.data(), pe.data() + off, dst.size());
    return true;
  };
  auto r = readPeFunctionRanges(read, pe.size());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  ASSERT_EQ(r->size(), 3u);
  EXPECT_EQ((*r)[0].beginRva, 0x2000u);
  EXPECT_EQ((*r)[2].endRva, 0x3040u);

  PeSpec none;
  const ByteVec pe2 = buildPe(none);
  PeReadFn read2 = [&](uint64_t off, std::span<std::byte> dst) {
    if (off > pe2.size() || pe2.size() - off < dst.size()) return false;
    std::memcpy(dst.data(), pe2.data() + off, dst.size());
    return true;
  };
  auto empty = readPeFunctionRanges(read2, pe2.size());
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty->empty());
}

TEST(PeInfo, ParsesFileFromDisk) {
  PeSpec spec;
  const ByteVec pe = buildPe(spec);
  const auto path = tempPath("fake.dll");
  {
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good());
    f.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
  }
  auto info = parsePeFile(path);
  ASSERT_TRUE(info.ok()) << info.error().toString();
  EXPECT_TRUE(info->hasCodeView);
  EXPECT_EQ(parsePeFile(tempPath("missing.dll")).error().code, ErrorCode::NotFound);
}

}  // namespace
}  // namespace tachyon::etlconvert
