// WP-34: readImageBytes / readModuleBytes on synthesized PE and ELF64 images and on the test binary itself.
#include "disasm/disassembler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(__linux__)
#include <link.h>
#endif

namespace tachyon::disasm {
namespace {

namespace fs = std::filesystem;

template <typename T>
void put(std::vector<uint8_t>& b, size_t off, T v) {
  if (b.size() < off + sizeof v) b.resize(off + sizeof v);
  std::memcpy(b.data() + off, &v, sizeof v);
}

fs::path writeTemp(const std::string& name, const std::vector<uint8_t>& bytes) {
  const fs::path p = fs::temp_directory_path() / ("tachyon_wp34_" + name);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return p;
}

uint8_t pattern(size_t fileOffset) {
  return static_cast<uint8_t>((fileOffset * 7 + 3) & 0xFF);
}

// Minimal PE32+: headers [0, 0x200), .text VA 0x1000 VS 0x300 raw 0x200 @0x200, .data VA 0x2000 VS 0x100
// raw 0x100 @0x400. Bytes of the file follow `pattern` outside the header structures.
std::vector<uint8_t> makePe() {
  std::vector<uint8_t> b(0x500);
  for (size_t i = 0; i < b.size(); ++i) b[i] = pattern(i);
  b[0] = 'M';
  b[1] = 'Z';
  put<uint32_t>(b, 0x3C, 0x80);
  std::memcpy(b.data() + 0x80, "PE\0\0", 4);
  put<uint16_t>(b, 0x84, 0x8664);
  put<uint16_t>(b, 0x86, 2);    // sections
  put<uint16_t>(b, 0x94, 240);  // optional header size
  put<uint16_t>(b, 0x98, 0x20B);
  put<uint32_t>(b, 0x98 + 60, 0x200);  // SizeOfHeaders
  const size_t sh = 0x98 + 240;
  std::memcpy(b.data() + sh, ".text\0\0\0", 8);
  put<uint32_t>(b, sh + 8, 0x300);
  put<uint32_t>(b, sh + 12, 0x1000);
  put<uint32_t>(b, sh + 16, 0x200);
  put<uint32_t>(b, sh + 20, 0x200);
  std::memcpy(b.data() + sh + 40, ".data\0\0\0", 8);
  put<uint32_t>(b, sh + 40 + 8, 0x100);
  put<uint32_t>(b, sh + 40 + 12, 0x2000);
  put<uint32_t>(b, sh + 40 + 16, 0x100);
  put<uint32_t>(b, sh + 40 + 20, 0x400);
  return b;
}

// Minimal ELF64: PT_LOAD #1 offset 0 vaddr 0x401000 filesz memsz 0x400; PT_LOAD #2 offset 0x400 vaddr
// 0x402400 filesz 0x100 memsz 0x200. RVA 0 = vaddr 0x401000.
std::vector<uint8_t> makeElf() {
  std::vector<uint8_t> b(0x500);
  for (size_t i = 0; i < b.size(); ++i) b[i] = pattern(i);
  std::memcpy(b.data(),
              "\x7f"
              "ELF",
              4);
  b[4] = 2;  // 64-bit
  b[5] = 1;  // little endian
  put<uint64_t>(b, 32, 64);
  put<uint16_t>(b, 54, 56);
  put<uint16_t>(b, 56, 2);
  auto phdr = [&](size_t i, uint64_t offset, uint64_t vaddr, uint64_t filesz, uint64_t memsz) {
    const size_t o = 64 + i * 56;
    put<uint32_t>(b, o, 1);
    put<uint32_t>(b, o + 4, 5);
    put<uint64_t>(b, o + 8, offset);
    put<uint64_t>(b, o + 16, vaddr);
    put<uint64_t>(b, o + 24, vaddr);
    put<uint64_t>(b, o + 32, filesz);
    put<uint64_t>(b, o + 40, memsz);
    put<uint64_t>(b, o + 48, 0x1000);
  };
  phdr(0, 0, 0x401000, 0x400, 0x400);
  phdr(1, 0x400, 0x402400, 0x100, 0x200);
  return b;
}

TEST(ModuleBytesTest, PeMapsRvasThroughSectionHeaders) {
  const std::vector<uint8_t> file = makePe();
  const fs::path p = writeTemp("pe.dll", file);

  auto text = readImageBytes(p, 0x1010, 16);
  ASSERT_TRUE(text.ok()) << text.error().toString();
  ASSERT_EQ(text->size(), 16u);
  for (size_t i = 0; i < 16; ++i) EXPECT_EQ((*text)[i], file[0x200 + 0x10 + i]);

  // Straddles the end of the raw data (0x1200): file bytes, then zero fill up to the virtual size (0x1300).
  auto tail = readImageBytes(p, 0x11F0, 0x30);
  ASSERT_TRUE(tail.ok());
  ASSERT_EQ(tail->size(), 0x30u);
  for (size_t i = 0; i < 0x10; ++i) EXPECT_EQ((*tail)[i], file[0x200 + 0x1F0 + i]);
  for (size_t i = 0x10; i < 0x30; ++i) EXPECT_EQ((*tail)[i], 0u) << i;

  // The read stops where the mapping ends (virtual end 0x1300; the next mapped byte is 0x2000).
  auto cut = readImageBytes(p, 0x12F0, 0x40);
  ASSERT_TRUE(cut.ok());
  EXPECT_EQ(cut->size(), 0x10u);

  auto data = readImageBytes(p, 0x2004, 8);
  ASSERT_TRUE(data.ok());
  for (size_t i = 0; i < 8; ++i) EXPECT_EQ((*data)[i], file[0x400 + 4 + i]);

  auto headers = readImageBytes(p, 0, 2);
  ASSERT_TRUE(headers.ok());
  EXPECT_EQ((*headers)[0], 'M');
  EXPECT_EQ((*headers)[1], 'Z');

  auto gap = readImageBytes(p, 0x1500, 4);
  ASSERT_FALSE(gap.ok());
  EXPECT_EQ(gap.error().code, ErrorCode::NotFound);

  model::Module mod;
  mod.path = "C:\\does\\not\\exist.dll";
  EXPECT_FALSE(readModuleBytes(mod, 0x1010, 4).ok());
  auto viaOverride = readModuleBytes(mod, 0x1010, 4, p);
  ASSERT_TRUE(viaOverride.ok());
  EXPECT_EQ((*viaOverride)[0], file[0x210]);
  fs::remove(p);
}

TEST(ModuleBytesTest, PeTruncatedFileGivesWhatExists) {
  std::vector<uint8_t> file = makePe();
  file.resize(0x280);  // cut in the middle of .text's raw data
  const fs::path p = writeTemp("pe_trunc.dll", file);
  auto r = readImageBytes(p, 0x1070, 0x40);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->size(), 0x10u);  // file bytes 0x270..0x280
  fs::remove(p);
}

TEST(ModuleBytesTest, ElfMapsRvasWithTheLowestLoadSegmentAsBase) {
  const std::vector<uint8_t> file = makeElf();
  const fs::path p = writeTemp("elf.so", file);

  auto a = readImageBytes(p, 0x100, 8);  // vaddr 0x401100 -> offset 0x100
  ASSERT_TRUE(a.ok()) << a.error().toString();
  for (size_t i = 0; i < 8; ++i) EXPECT_EQ((*a)[i], file[0x100 + i]);

  // Second segment: rva 0x1400 -> vaddr 0x402400 -> offset 0x400; bytes beyond filesz are zero (bss).
  auto b = readImageBytes(p, 0x14F0, 0x30);
  ASSERT_TRUE(b.ok());
  ASSERT_EQ(b->size(), 0x30u);
  for (size_t i = 0; i < 0x10; ++i) EXPECT_EQ((*b)[i], file[0x4F0 + i]);
  for (size_t i = 0x10; i < 0x30; ++i) EXPECT_EQ((*b)[i], 0u);

  // The gap between the segments and the area past the second one are not mapped.
  auto gap = readImageBytes(p, 0x800, 4);
  ASSERT_FALSE(gap.ok());
  EXPECT_EQ(gap.error().code, ErrorCode::NotFound);
  EXPECT_FALSE(readImageBytes(p, 0x1600, 4).ok());
  fs::remove(p);
}

TEST(ModuleBytesTest, RejectsOtherFormatsAndMissingFiles) {
  const fs::path p = writeTemp("junk.bin", std::vector<uint8_t>(256, 0x42));
  auto r = readImageBytes(p, 0, 4);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
  fs::remove(p);
  auto missing = readImageBytes(p, 0, 4);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, ErrorCode::IoError);

  // A PE header that points outside the file is corrupt, not a crash.
  std::vector<uint8_t> bad(128, 0);
  bad[0] = 'M';
  bad[1] = 'Z';
  put<uint32_t>(bad, 0x3C, 0x7000);
  const fs::path q = writeTemp("badpe.dll", bad);
  auto c = readImageBytes(q, 0, 4);
  ASSERT_FALSE(c.ok());
  EXPECT_EQ(c.error().code, ErrorCode::CorruptData);
  fs::remove(q);
}

// A header whose PT_LOAD claims 1<<44 bytes must not make a huge request allocate (found by the WP-34
// verifier).
TEST(ModuleBytesTest, HugeSizesAreRejectedWithoutAllocating) {
  std::vector<uint8_t> f(64 + 56, 0);
  std::memcpy(f.data(),
              "\x7f"
              "ELF",
              4);
  f[4] = 2;
  f[5] = 1;
  put<uint64_t>(f, 32, 64);
  put<uint16_t>(f, 54, 56);
  put<uint16_t>(f, 56, 1);
  put<uint32_t>(f, 64, 1);
  put<uint64_t>(f, 64 + 40, uint64_t{1} << 44);
  const fs::path p = writeTemp("big.elf", f);
  auto huge = readImageBytes(p, 0, size_t{1} << 40);
  ASSERT_FALSE(huge.ok());
  EXPECT_EQ(huge.error().code, ErrorCode::InvalidArgument);
  EXPECT_FALSE(readImageBytes(p, 0, kMaxReadBytes + 1).ok());
  // Within the limit the zero-filled mapping is served (bounded by the request).
  auto ok = readImageBytes(p, 0x1000, 4096);
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(ok->size(), 4096u);
  model::Module m;
  m.path = p.string();
  EXPECT_FALSE(readModuleBytes(m, 0, size_t{1} << 40).ok());
  fs::remove(p);
}

#if defined(__linux__)
extern "C" __attribute__((noinline, used)) int tachyonWp34Probe(int x) {
  volatile int y = x * 31 + 7;
  return y ^ 0x5A5A;
}

// The bytes the loader mapped for a function of this very test binary equal what readModuleBytes reads from
// the file at the RVA computed with the WP-72 convention (base = lowest PT_LOAD address).
TEST(ModuleBytesTest, ReadsTheRunningTestBinary) {
  struct Ctx {
    uint64_t bias = 0;
    uint64_t lowest = UINT64_MAX;
  } c;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;  // the main program only
        Ctx* ctx = static_cast<Ctx*>(data);
        ctx->bias = info->dlpi_addr;
        for (int i = 0; i < info->dlpi_phnum; ++i)
          if (info->dlpi_phdr[i].p_type == PT_LOAD)
            ctx->lowest = std::min<uint64_t>(ctx->lowest, info->dlpi_phdr[i].p_vaddr);
        return 1;
      },
      &c);
  ASSERT_NE(c.lowest, UINT64_MAX);
  const uint64_t loadBase = c.bias + (c.lowest & ~uint64_t{4095});
  const uint64_t addr = reinterpret_cast<uint64_t>(&tachyonWp34Probe);
  ASSERT_GE(addr, loadBase);
  const uint64_t rva = addr - loadBase;

  model::Module mod;
  mod.path = fs::read_symlink("/proc/self/exe").string();
  mod.base = loadBase;
  auto bytes = readModuleBytes(mod, rva, 16);
  ASSERT_TRUE(bytes.ok()) << bytes.error().toString();
  ASSERT_EQ(bytes->size(), 16u);
  EXPECT_EQ(std::memcmp(bytes->data(), reinterpret_cast<const void*>(addr), 16), 0);

  // And it disassembles to something sensible.
  const auto ins = disassemble(*bytes, rva);
  ASSERT_FALSE(ins.empty());
  EXPECT_TRUE(ins[0].valid);
}
#endif

}  // namespace
}  // namespace tachyon::disasm
