// Corrupt / truncated ELF and DWARF must produce errors or empty answers, never a crash (WP-72). Real fixture
// files are truncated at many offsets and mutated (random bytes, aimed at the debug sections and the section
// header table); every variant is loaded and queried. Run under ASan/UBSan in the linux-asan preset.
#include "dwarf_test_util.h"

#include <unistd.h>

#include <random>

namespace tachyon::symbols {
namespace {

using namespace dwarftest;
namespace fs = std::filesystem;

std::vector<char> slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, const std::vector<char>& bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool acceptableLoadError(ErrorCode c) {
  return c == ErrorCode::CorruptData || c == ErrorCode::NotFound || c == ErrorCode::IoError ||
         c == ErrorCode::Unsupported;
}

// Loads `file` and exercises every query. Returns true when the load succeeded.
bool loadAndQuery(const fs::path& file, const std::vector<uint64_t>& rvas) {
  SymbolStringTable strings;
  DwarfSymbolProvider provider(strings, makeLibdwOpener());
  const Result<void> r = provider.load(model::Module{}, file);
  if (!r.ok()) {
    EXPECT_TRUE(acceptableLoadError(r.error().code)) << r.error().toString();
    return false;
  }
  for (const uint64_t rva : rvas) {
    const ResolvedAddress a = provider.resolve(rva);
    if (a.resolved) {
      (void)strings.get(a.function);
      for (const InlineFrame& f : a.inlines) (void)strings.get(f.function);
      auto lines = provider.linesForFunction(a.functionStartRva);
      if (!lines.ok()) {
        EXPECT_TRUE(acceptableLoadError(lines.error().code) ||
                    lines.error().code == ErrorCode::InvalidArgument);
      }
    }
    (void)provider.functionBounds(rva);
  }
  return true;
}

// TACHYON_FUZZ_SEED / TACHYON_FUZZ_SCALE (environment) run a heavier, differently seeded campaign locally.
uint64_t fuzzSeed(uint64_t fallback) {
  const char* v = std::getenv("TACHYON_FUZZ_SEED");
  return v != nullptr ? std::strtoull(v, nullptr, 10) ^ fallback : fallback;
}
int fuzzScale() {
  const char* v = std::getenv("TACHYON_FUZZ_SCALE");
  return v != nullptr ? std::max(1, std::atoi(v)) : 1;
}

class DwarfRobustness : public testing::Test {
 protected:
  void SetUp() override {
    m_dir = fs::temp_directory_path() / ("tachyon-dwarf-fuzz-" + std::to_string(::getpid()));
    fs::create_directories(m_dir);
    m_orig = fixture("dwarf_fixture_nopie");
    m_bytes = slurp(m_orig);
    ASSERT_GT(m_bytes.size(), 4096u);
    for (const char* sym :
         {"hot_c", "with_inlines", "split_fn", "_ZN2fx6Widget4workEi", "main", "_Z7cold_fni"}) {
      if (auto rva = symbolRva(m_orig, sym)) {
        for (uint64_t d = 0; d < 0x60; d += 3) m_rvas.push_back(*rva + d);
      }
    }
    m_rvas.push_back(0);
    m_rvas.push_back(~0ull);
    ASSERT_TRUE(loadAndQuery(m_orig, m_rvas));
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(m_dir, ec);
  }

  fs::path m_dir, m_orig;
  std::vector<char> m_bytes;
  std::vector<uint64_t> m_rvas;
};

TEST_F(DwarfRobustness, TruncatedFilesNeverCrash) {
  const fs::path f = m_dir / "trunc.elf";
  size_t loaded = 0, total = 0;
  const size_t step = std::max<size_t>(1, m_bytes.size() / 400);
  std::vector<size_t> cuts;
  for (size_t cut = 0; cut < m_bytes.size(); cut += step) cuts.push_back(cut);
  for (size_t i = 0; i < 80; ++i) cuts.push_back(i);                       // inside the ELF header
  for (size_t i = 1; i < 40; ++i) cuts.push_back(m_bytes.size() - i * 7);  // inside the section header table
  for (const size_t cut : cuts) {
    spit(f, std::vector<char>(m_bytes.begin(), m_bytes.begin() + static_cast<std::ptrdiff_t>(cut)));
    ++total;
    if (loadAndQuery(f, m_rvas)) ++loaded;
  }
  // Cutting off the section header table (the tail of the file) always fails; everything before that fails
  // too because the headers point past the end.
  EXPECT_LT(loaded, total);
}

// Random byte corruption inside the .debug_* sections of `orig`.
void mutateDebugSections(const fs::path& orig, const fs::path& scratch, uint64_t seed, int rounds,
                         const std::vector<uint64_t>& rvas) {
  auto elf = ElfFile::open(orig);
  ASSERT_TRUE(elf.ok());
  std::vector<std::pair<uint64_t, uint64_t>> targets;  // (offset, size) of the .debug_* sections
  for (const ElfSection& s : elf.value().sections()) {
    if (s.name.rfind(".debug_", 0) == 0 && s.size > 0) targets.emplace_back(s.offset, s.size);
  }
  ASSERT_FALSE(targets.empty());
  const std::vector<char> original = slurp(orig);

  std::mt19937_64 rng(seed);
  size_t loaded = 0;
  for (int round = 0; round < rounds; ++round) {
    std::vector<char> bytes = original;
    const int flips = 1 + static_cast<int>(rng() % 6);
    for (int k = 0; k < flips; ++k) {
      const auto& t = targets[rng() % targets.size()];
      const size_t at = static_cast<size_t>(t.first + rng() % t.second);
      switch (rng() % 3) {
        case 0: bytes[at] = static_cast<char>(rng()); break;
        case 1: bytes[at] = static_cast<char>(bytes[at] ^ (1u << (rng() % 8))); break;
        default: bytes[at] = static_cast<char>(0xFF); break;
      }
    }
    spit(scratch, bytes);
    if (loadAndQuery(scratch, rvas)) ++loaded;
  }
  // Most single-byte corruptions of debug info still load (libdw decodes lazily); the point is that nothing
  // crashes.
  EXPECT_GT(loaded, 0u);
}

TEST_F(DwarfRobustness, MutatedDebugSectionsNeverCrash) {
  mutateDebugSections(m_orig, m_dir / "mut.elf", fuzzSeed(0x7AC4F00D), 600 * fuzzScale(), m_rvas);
}

TEST_F(DwarfRobustness, MutatedGccDebugSectionsNeverCrash) {
  const fs::path gcc = fixture("dwarf_fixture_split");
  if (!fs::exists(gcc)) GTEST_SKIP() << "g++ not available: no gcc-built fixture";
  std::vector<uint64_t> rvas;
  for (const char* sym : {"split_fn", "split_fn.cold", "with_inlines", "main", "main.cold"}) {
    if (auto rva = symbolRva(gcc, sym)) {
      for (uint64_t d = 0; d < 0x60; d += 3) rvas.push_back(*rva + d);
    }
  }
  mutateDebugSections(gcc, m_dir / "mut-gcc.elf", fuzzSeed(0x6CC), 300 * fuzzScale(), rvas);
}

TEST_F(DwarfRobustness, MutatedHeadersAndSectionTableNeverCrash) {
  std::mt19937_64 rng(fuzzSeed(0xBADC0DE));
  const fs::path f = m_dir / "hdr.elf";
  const uint64_t shoff = [&] {
    uint64_t v = 0;
    std::memcpy(&v, m_bytes.data() + 40, 8);
    return v;
  }();
  for (int round = 0; round < 800 * fuzzScale(); ++round) {
    std::vector<char> bytes = m_bytes;
    const int flips = 1 + static_cast<int>(rng() % 4);
    for (int k = 0; k < flips; ++k) {
      size_t at;
      switch (rng() % 3) {
        case 0: at = rng() % 64; break;           // ELF header
        case 1: at = 64 + rng() % 56 * 4; break;  // program headers
        default: at = static_cast<size_t>(shoff) + rng() % (bytes.size() - static_cast<size_t>(shoff)); break;
      }
      if (at >= bytes.size()) continue;
      bytes[at] = static_cast<char>(rng());
    }
    spit(f, bytes);
    (void)loadAndQuery(f, m_rvas);
  }
}

TEST_F(DwarfRobustness, GarbageAndEmptyFilesAreRejected) {
  const fs::path f = m_dir / "garbage.elf";
  spit(f, {});
  EXPECT_FALSE(loadAndQuery(f, m_rvas));
  std::mt19937_64 rng(1);
  std::vector<char> junk(8192);
  for (char& c : junk) c = static_cast<char>(rng());
  spit(f, junk);
  EXPECT_FALSE(loadAndQuery(f, m_rvas));
  std::memcpy(junk.data(),
              "\x7f"
              "ELF\x02\x01\x01",
              7);  // valid magic, random rest
  spit(f, junk);
  (void)loadAndQuery(f, m_rvas);
  EXPECT_FALSE(loadAndQuery(m_dir / "does-not-exist.elf", m_rvas));
}

TEST_F(DwarfRobustness, ThirtyTwoBitAndBigEndianElfAreUnsupported) {
  const fs::path f = m_dir / "class32.elf";
  std::vector<char> bytes = m_bytes;
  bytes[4] = 1;  // ELFCLASS32
  spit(f, bytes);
  SymbolStringTable strings;
  DwarfSymbolProvider p(strings, makeLibdwOpener());
  EXPECT_EQ(p.load(model::Module{}, f).error().code, ErrorCode::Unsupported);
  bytes = m_bytes;
  bytes[5] = 2;  // big endian
  spit(f, bytes);
  EXPECT_EQ(p.load(model::Module{}, f).error().code, ErrorCode::Unsupported);
}

}  // namespace
}  // namespace tachyon::symbols
