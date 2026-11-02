// ELF reader, symbol table and range index (WP-72): pure logic, no libdw.
#include "dwarf_test_util.h"
#include "symbols/dwarf/elf_symbol_table.h"
#include "symbols/dwarf/range_index.h"

#include <cstring>

namespace tachyon::symbols {
namespace {

using namespace dwarftest;

TEST(ElfCrc32, KnownVectorAndIncrementalUse) {
  const char* text = "123456789";
  const auto* bytes = reinterpret_cast<const uint8_t*>(text);
  EXPECT_EQ(elfCrc32(bytes, 9), 0xCBF43926u);
  EXPECT_EQ(elfCrc32(bytes + 4, 5, elfCrc32(bytes, 4)), 0xCBF43926u);
  EXPECT_EQ(elfCrc32(bytes, 0), 0u);
}

TEST(RangeIndex, NestedAndOverlappingRangesResolveToTheGreatestStart) {
  RangeIndex idx;
  idx.build({{0x100, 0x400, 1},
             {0x180, 0x200, 2},
             {0x1c0, 0x1d0, 3},
             {0x500, 0x500, 4} /*empty*/,
             {0x600, 0x700, 5}});
  EXPECT_EQ(idx.size(), 4u);
  EXPECT_EQ(idx.find(0x0ff), nullptr);
  EXPECT_EQ(idx.find(0x100)->payload, 1u);
  EXPECT_EQ(idx.find(0x17f)->payload, 1u);
  EXPECT_EQ(idx.find(0x180)->payload, 2u);
  EXPECT_EQ(idx.find(0x1c8)->payload, 3u);
  EXPECT_EQ(idx.find(0x1d0)->payload, 2u);
  EXPECT_EQ(idx.find(0x200)->payload, 1u);  // back in the outer range after the nested one ends
  EXPECT_EQ(idx.find(0x3ff)->payload, 1u);
  EXPECT_EQ(idx.find(0x400), nullptr);
  EXPECT_EQ(idx.find(0x650)->payload, 5u);
  EXPECT_EQ(idx.find(0x700), nullptr);

  std::vector<uint32_t> all;
  idx.forEachContaining(0x1c8, [&](const IndexedRange& r) {
    all.push_back(r.payload);
    return false;
  });
  EXPECT_EQ(all, (std::vector<uint32_t>{3, 2, 1}));  // innermost first
}

TEST(RangeIndex, EmptyIndexFindsNothing) {
  RangeIndex idx;
  idx.build({});
  EXPECT_EQ(idx.find(0), nullptr);
  EXPECT_EQ(idx.find(~0ull), nullptr);
}

ElfSymbol sym(const char* name, uint64_t value, uint64_t size, uint64_t sectionEnd, uint8_t binding = 1) {
  ElfSymbol s;
  s.name = name;
  s.value = value;
  s.size = size;
  s.sectionEnd = sectionEnd;
  s.binding = binding;
  return s;
}

TEST(ElfSymbolTable, SizedSymbolsCoverExactlyTheirSize) {
  ElfSymbolTable t;
  t.build({sym("a", 0x1000, 0x20, 0x2000), sym("b", 0x1040, 0x10, 0x2000)}, 0);
  EXPECT_EQ(t.find(0x1000)->name, "a");
  EXPECT_EQ(t.find(0x101f)->name, "a");
  EXPECT_EQ(t.find(0x1020), nullptr);  // gap between sized symbols
  EXPECT_EQ(t.find(0x1044)->name, "b");
  EXPECT_EQ(t.find(0x1050), nullptr);
  EXPECT_EQ(t.find(0xfff), nullptr);
}

TEST(ElfSymbolTable, UnsizedSymbolsAreBoundedByTheNextSymbolAndTheSectionEnd) {
  ElfSymbolTable t;
  t.build({sym("first", 0x1000, 0, 0x1100), sym("second", 0x1040, 0, 0x1100), sym("last", 0x1080, 0, 0x1100)},
          0);
  EXPECT_EQ(t.find(0x103f)->name, "first");
  EXPECT_EQ(t.find(0x1040)->name, "second");
  EXPECT_EQ(t.find(0x107f)->name, "second");
  EXPECT_EQ(t.find(0x10ff)->name, "last");
  EXPECT_EQ(t.find(0x1100), nullptr);  // never unbounded to infinity: clipped at the section end
  EXPECT_EQ(t.find(0x9000), nullptr);
  EXPECT_EQ(t.entries()[2].size, 0x80u);
}

TEST(ElfSymbolTable, UnsizedSymbolWithoutAnyBoundIsDropped) {
  ElfSymbolTable t;
  t.build({sym("lonely", 0x1000, 0, 0)}, 0);  // no section end, no next symbol
  EXPECT_EQ(t.size(), 0u);
  EXPECT_EQ(t.find(0x1000), nullptr);
}

TEST(ElfSymbolTable, AliasesPreferGlobalThenWeakThenLocal) {
  ElfSymbolTable t;
  t.build({sym("local_alias", 0x1000, 0x10, 0x2000, 0), sym("weak_alias", 0x1000, 0x10, 0x2000, 2),
           sym("global_name", 0x1000, 0x10, 0x2000, 1)},
          0);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t.find(0x1004)->name, "global_name");
}

TEST(ElfSymbolTable, NestedSymbolsResolveToTheInnermost) {
  ElfSymbolTable t;
  t.build({sym("outer", 0x1000, 0x100, 0x2000), sym("inner", 0x1040, 0x20, 0x2000)}, 0);
  EXPECT_EQ(t.find(0x1030)->name, "outer");
  EXPECT_EQ(t.find(0x1050)->name, "inner");
  EXPECT_EQ(t.find(0x1060)->name, "outer");  // after the nested symbol the outer one is back
}

TEST(ElfSymbolTable, RvasAreRelativeToTheLoadBias) {
  ElfSymbolTable t;
  t.build({sym("f", 0x401000, 0x20, 0x402000), sym("below", 0x300000, 0x20, 0x402000)}, 0x400000);
  EXPECT_EQ(t.size(), 1u);
  EXPECT_EQ(t.find(0x1000)->name, "f");
  EXPECT_EQ(t.find(0x1000)->rva, 0x1000u);
  EXPECT_EQ(t.find(0x401000), nullptr);
}

TEST(ElfFileOpen, ReportsMissingNonElfAndTruncatedFiles) {
  EXPECT_EQ(ElfFile::open("/nonexistent/definitely/not/here").error().code, ErrorCode::NotFound);

  const auto dir = std::filesystem::temp_directory_path() / "tachyon-elf-open-test";
  std::filesystem::create_directories(dir);
  const auto text = dir / "text.bin";
  {
    std::ofstream out(text, std::ios::binary);
    out << "this is not an ELF file at all";
  }
  EXPECT_EQ(ElfFile::open(text).error().code, ErrorCode::CorruptData);

  // A real binary cut inside its header, inside its section headers, and with its program headers gone.
  const auto real = fixture("dwarf_fixture_pie");
  const uint64_t size = std::filesystem::file_size(real);
  std::ifstream in(real, std::ios::binary);
  std::vector<char> bytes(size);
  in.read(bytes.data(), static_cast<std::streamsize>(size));
  for (const uint64_t cut : {uint64_t{8}, uint64_t{40}, uint64_t{200}, size / 2, size - 100}) {
    const auto cutFile = dir / "cut.bin";
    {
      std::ofstream out(cutFile, std::ios::binary);
      out.write(bytes.data(), static_cast<std::streamsize>(cut));
    }
    auto r = ElfFile::open(cutFile);
    ASSERT_FALSE(r.ok()) << "cut at " << cut;
    EXPECT_EQ(r.error().code, ErrorCode::CorruptData) << "cut at " << cut;
  }
  std::filesystem::remove_all(dir);
}

TEST(ElfFileOpen, ReadsSectionsBuildIdAndLoadBias) {
  auto pie = ElfFile::open(fixture("dwarf_fixture_pie"));
  auto nopie = ElfFile::open(fixture("dwarf_fixture_nopie"));
  auto nodebug = ElfFile::open(fixture("dwarf_fixture_nodebug"));
  ASSERT_TRUE(pie.ok() && nopie.ok() && nodebug.ok());
  EXPECT_EQ(pie.value().type(), kElfEtDyn);
  EXPECT_EQ(nopie.value().type(), kElfEtExec);
  EXPECT_EQ(pie.value().loadBias(), 0u);
  EXPECT_EQ(nopie.value().loadBias() % 4096, 0u);
  EXPECT_NE(nopie.value().loadBias(), 0u);
  EXPECT_TRUE(pie.value().hasDebugInfo());
  EXPECT_FALSE(nodebug.value().hasDebugInfo());
  EXPECT_NE(pie.value().findSection(".text"), nullptr);
  EXPECT_EQ(pie.value().findSection(".no_such_section"), nullptr);
  const auto bid = pie.value().buildId();
  ASSERT_TRUE(bid.has_value());
  EXPECT_EQ(bid->size(), 20u);  // sha1
  EXPECT_FALSE(pie.value().debugLink().has_value());
}

TEST(ElfFileOpen, CodeSymbolsCarrySizesAndSectionEnds) {
  auto elf = ElfFile::open(fixture("dwarf_fixture_nopie"));
  ASSERT_TRUE(elf.ok());
  auto syms = elf.value().readCodeSymbols();
  ASSERT_TRUE(syms.ok());
  const ElfSymbol* hot = nullptr;
  for (const ElfSymbol& s : syms.value()) {
    if (s.name == "hot_c") hot = &s;
  }
  ASSERT_NE(hot, nullptr);
  EXPECT_GT(hot->size, 0u);
  EXPECT_GE(hot->value, elf.value().loadBias());
  EXPECT_GT(hot->sectionEnd, hot->value);
}

}  // namespace
}  // namespace tachyon::symbols
