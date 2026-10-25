// Real msdia140 against the testbed's own PDB (WP-22): function/file/line attribution of hot_a & co, inline
// chains of the `inline` scenario, bounds, lines, mismatch / missing / corrupt PDB errors, thread safety, the
// SymbolService wiring, and MSVC undecoration.
#include "symbols/dia/dia_provider.h"
#include "etlconvert/pe_info.h"
#include "symbols/dia/dia_errors.h"
#include "symbols/dia/msvc_undecorate.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace tachyon::symbols {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Line (1-based) of the first source line containing `needle`, 0 if absent.
uint32_t lineOf(const fs::path& file, const std::string& needle) {
  std::ifstream in(file);
  std::string text;
  uint32_t n = 0;
  while (std::getline(in, text)) {
    ++n;
    if (text.find(needle) != std::string::npos) return n;
  }
  return 0;
}

struct Testbed {
  fs::path exe{TACHYON_TESTBED_EXE};
  fs::path pdb;
  model::Module module;
  std::vector<etlconvert::PeFunctionRange> ranges;
  uint32_t sizeOfImage = 0;
  bool ok = false;
  std::string why;
};

Testbed loadTestbed() {
  Testbed t;
  auto pe = etlconvert::parsePeFile(t.exe);
  if (!pe.ok() || !pe.value().hasCodeView) {
    t.why = "testbed has no CodeView record";
    return t;
  }
  t.sizeOfImage = pe.value().sizeOfImage;
  t.pdb = pe.value().pdbPath;
  if (!fs::exists(t.pdb)) t.pdb = fs::path(t.exe).replace_extension(".pdb");
  if (!fs::exists(t.pdb)) {
    t.why = "no PDB for the testbed";
    return t;
  }
  t.module.path = t.exe.string();
  t.module.id.kind = trace::kModuleIdPdb;
  t.module.id.len = 16;
  std::memcpy(t.module.id.bytes.data(), pe.value().guid, 16);
  t.module.id.age = pe.value().age;
  auto ranges = etlconvert::readPeFunctionRangesFromFile(t.exe);
  if (!ranges.ok() || ranges.value().empty()) {
    t.why = "no .pdata";
    return t;
  }
  t.ranges = std::move(ranges).value();
  t.ok = true;
  return t;
}

class DiaWindows : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string err;
    m_opener = makeComDiaOpener(&err);
    ASSERT_NE(m_opener, nullptr) << err;
    m_tb = loadTestbed();
    ASSERT_TRUE(m_tb.ok) << m_tb.why;
    m_provider = std::make_unique<DiaSymbolProvider>(m_strings, m_opener);
    auto r = m_provider->load(m_tb.module, m_tb.pdb);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    // Function start rva by name. Leaf functions (hot_a & co) have no .pdata entry, so walk the PDB's own
    // function extents from the start of .text instead of the exception directory.
    uint32_t hi = 0;
    for (const auto& range : m_tb.ranges) hi = std::max(hi, range.endRva);
    hi = std::min<uint32_t>(m_tb.sizeOfImage, hi + 0x10000);
    for (uint32_t rva = 0x1000; rva < hi;) {
      auto b = m_provider->functionBounds(rva);
      if (!b.ok() || b.value().size == 0) {
        ++rva;
        continue;
      }
      const std::string name(m_strings.get(b.value().name));
      const uint32_t start = static_cast<uint32_t>(b.value().startRva);
      const uint32_t end = start + static_cast<uint32_t>(b.value().size);
      m_byName.emplace(name, etlconvert::PeFunctionRange{start, end});
      rva = std::max(rva + 1, end);
    }
    std::printf("[dia] %zu functions walked in the PDB (%zu pdata entries)\n", m_byName.size(),
                m_tb.ranges.size());
  }

  SymbolStringTable m_strings;
  std::shared_ptr<IDiaSessionOpener> m_opener;
  Testbed m_tb;
  std::unique_ptr<DiaSymbolProvider> m_provider;
  std::map<std::string, etlconvert::PeFunctionRange> m_byName;
};

TEST_F(DiaWindows, ResolvesHotFunctionsToFileAndLine) {
  const fs::path source(TACHYON_TESTBED_SCENARIOS_CPP);
  for (const char* name : {"hot_a", "hot_b", "hot_c", "spin_rest"}) {
    const auto it = m_byName.find(name);
    ASSERT_NE(it, m_byName.end()) << name << " not found among " << m_byName.size() << " pdata functions";
    const ResolvedAddress a = m_provider->resolve(it->second.beginRva);
    ASSERT_TRUE(a.resolved) << name;
    EXPECT_EQ(m_strings.get(a.function), name);
    EXPECT_EQ(a.functionStartRva, it->second.beginRva);
    const std::string file = lower(std::string(m_strings.get(a.file)));
    EXPECT_NE(file.find("scenarios.cpp"), std::string::npos) << name << " file=" << file;
    const uint32_t expected = lineOf(source, std::string("uint64_t ") + name + "(");
    ASSERT_GT(expected, 0u) << name;
    EXPECT_GE(a.line, expected) << name;
    EXPECT_LE(a.line, expected + 3) << name;
    // An address inside the function body resolves to the same function.
    const uint64_t mid = it->second.beginRva + (it->second.endRva - it->second.beginRva) / 2;
    const ResolvedAddress b = m_provider->resolve(mid);
    ASSERT_TRUE(b.resolved);
    EXPECT_EQ(b.functionStartRva, it->second.beginRva);
    EXPECT_EQ(m_strings.get(b.function), name);
  }
}

TEST_F(DiaWindows, InlineScenarioYieldsInlineChains) {
  const auto it = m_byName.find("inline_hot_loop");
  ASSERT_NE(it, m_byName.end());
  const uint64_t loopStart = it->second.beginRva;
  // The optimizer splits the loop into several blocks; every .pdata block that belongs to the function
  // counts.
  std::vector<etlconvert::PeFunctionRange> blocks;
  blocks.push_back(it->second);
  for (const auto& range : m_tb.ranges) {
    const ResolvedAddress a = m_provider->resolve(range.beginRva);
    if (a.resolved && a.functionStartRva == loopStart && range.beginRva != loopStart) blocks.push_back(range);
  }
  std::map<std::string, size_t> chains;
  std::set<std::string> allNames;
  size_t withInlines = 0;
  bool burnInsideInlineA = false;
  for (const auto& block : blocks) {
    for (uint64_t rva = block.beginRva; rva < block.endRva; ++rva) {
      const ResolvedAddress a = m_provider->resolve(rva);
      ASSERT_TRUE(a.resolved);
      EXPECT_EQ(m_strings.get(a.function), "inline_hot_loop");  // the physical frame, never the inlinee
      if (a.inlines.empty()) continue;
      ++withInlines;
      std::string chain;
      int burnAt = -1;
      int inlineAAt = -1;
      for (size_t i = 0; i < a.inlines.size(); ++i) {
        const std::string name(m_strings.get(a.inlines[i].function));
        EXPECT_FALSE(name.empty());
        allNames.insert(name);
        chain += name + ":" + std::to_string(a.inlines[i].line) + " < ";
        if (name.find("::burn") != std::string::npos) burnAt = static_cast<int>(i);
        if (name.find("inline_a") != std::string::npos) inlineAAt = static_cast<int>(i);
      }
      ++chains[chain];
      if (burnAt >= 0 && inlineAAt >= 0 && burnAt < inlineAAt) burnInsideInlineA = true;  // innermost first
    }
  }
  EXPECT_GT(withInlines, 0u);
  const auto has = [&](const char* needle) {
    return std::any_of(allNames.begin(), allNames.end(),
                       [&](const std::string& n) { return n.find(needle) != std::string::npos; });
  };
  EXPECT_TRUE(has("inline_a")) << "inline names seen: " << allNames.size();
  EXPECT_TRUE(has("inline_b"));
  EXPECT_TRUE(has("inline_c"));
  EXPECT_TRUE(burnInsideInlineA) << "burn must precede (be inside) inline_a in an innermost-first chain";
  for (const auto& [chain, count] : chains) std::printf("[dia] chain x%zu: %s\n", count, chain.c_str());
}

TEST_F(DiaWindows, FunctionBoundsAreConsistentWithTheExceptionDirectory) {
  const auto it = m_byName.find("hot_a");
  ASSERT_NE(it, m_byName.end());
  const uint64_t start = it->second.beginRva;
  auto b = m_provider->functionBounds(start + 1);
  ASSERT_TRUE(b.ok()) << b.error().toString();
  EXPECT_EQ(b.value().startRva, start);
  EXPECT_EQ(b.value().size, it->second.endRva - it->second.beginRva);
  EXPECT_GT(b.value().size, 0u);
  EXPECT_EQ(m_strings.get(b.value().name), "hot_a");
  // The last byte is inside the function.
  auto last = m_provider->functionBounds(start + b.value().size - 1);
  ASSERT_TRUE(last.ok());
  EXPECT_EQ(last.value().startRva, start);
  // Where .pdata has an entry for a function (non-leaf), the PDB's extent does not run past it.
  size_t compared = 0;
  for (const auto& range : m_tb.ranges) {
    const auto bounds = m_provider->functionBounds(range.beginRva);
    if (!bounds.ok() || bounds.value().startRva != range.beginRva) continue;
    EXPECT_LE(bounds.value().startRva + bounds.value().size, range.endRva + 16u);
    ++compared;
  }
  EXPECT_GT(compared, 100u);
  EXPECT_FALSE(m_provider->functionBounds(0xFFFFFFF0u).ok());
  EXPECT_EQ(m_provider->functionBounds(0xFFFFFFF0u).error().code, ErrorCode::NotFound);
}

TEST_F(DiaWindows, LinesForFunction) {
  const auto it = m_byName.find("hot_a");
  ASSERT_NE(it, m_byName.end());
  auto lines = m_provider->linesForFunction(it->second.beginRva);
  ASSERT_TRUE(lines.ok()) << lines.error().toString();
  ASSERT_FALSE(lines.value().empty());
  uint64_t prev = 0;
  bool sawScenarios = false;
  for (const LineRange& l : lines.value()) {
    EXPECT_GE(l.rva, prev);
    EXPECT_GE(l.rva, it->second.beginRva);
    EXPECT_LT(l.rva, it->second.endRva);
    prev = l.rva;
    if (lower(std::string(m_strings.get(l.file))).find("scenarios.cpp") != std::string::npos)
      sawScenarios = true;
  }
  EXPECT_TRUE(sawScenarios);
  EXPECT_EQ(m_provider->linesForFunction(it->second.beginRva + 1).error().code, ErrorCode::NotFound);
}

TEST_F(DiaWindows, MismatchedAndMissingPdbsMapToErrors) {
  {
    model::Module bad = m_tb.module;
    bad.id.bytes[0] ^= 0xFF;
    DiaSymbolProvider p(m_strings, m_opener);
    auto r = p.load(bad, m_tb.pdb);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound) << r.error().toString();
    std::printf("[dia] wrong GUID -> %s\n", r.error().toString().c_str());
  }
  {
    model::Module bad = m_tb.module;
    bad.id.age += 5;
    DiaSymbolProvider p(m_strings, m_opener);
    auto r = p.load(bad, m_tb.pdb);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound) << r.error().toString();
    std::printf("[dia] wrong age -> %s\n", r.error().toString().c_str());
  }
  {
    DiaSymbolProvider p(m_strings, m_opener);
    auto r = p.load(m_tb.module, fs::temp_directory_path() / "tachyon-dia-does-not-exist.pdb");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
  }
  {
    const fs::path junk = fs::temp_directory_path() / "tachyon-dia-junk.pdb";
    {
      std::ofstream out(junk, std::ios::binary);
      out << "this is not a PDB file, just some text padding it out to a few hundred bytes. "
          << std::string(512, 'x');
    }
    DiaSymbolProvider p(m_strings, m_opener);
    auto r = p.load(m_tb.module, junk);
    std::error_code ec;
    fs::remove(junk, ec);
    ASSERT_FALSE(r.ok());
    std::printf("[dia] junk file -> %s\n", r.error().toString().c_str());
    EXPECT_TRUE(r.error().code == ErrorCode::CorruptData || r.error().code == ErrorCode::NotFound)
        << r.error().toString();
  }
}

TEST_F(DiaWindows, ConcurrentResolveMatchesSequential) {
  std::vector<uint64_t> rvas;
  for (const auto& range : m_tb.ranges) {
    for (uint64_t rva = range.beginRva; rva < range.endRva; rva += 7) rvas.push_back(rva);
  }
  ASSERT_FALSE(rvas.empty());
  std::vector<uint64_t> expected(rvas.size());
  for (size_t i = 0; i < rvas.size(); ++i) {
    const ResolvedAddress a = m_provider->resolve(rvas[i]);
    expected[i] = (static_cast<uint64_t>(a.function.get()) << 32) ^ a.line ^ (a.inlines.size() << 24);
  }
  std::atomic<size_t> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (size_t k = 0; k < rvas.size(); ++k) {
        const size_t i = (k + static_cast<size_t>(t) * 997) % rvas.size();
        const ResolvedAddress a = m_provider->resolve(rvas[i]);
        const uint64_t got =
            (static_cast<uint64_t>(a.function.get()) << 32) ^ a.line ^ (a.inlines.size() << 24);
        if (got != expected[i]) ++mismatches;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(mismatches.load(), 0u);
}

TEST_F(DiaWindows, SymbolServiceResolvesThroughDia) {
  const auto it = m_byName.find("hot_a");
  ASSERT_NE(it, m_byName.end());
  model::TraceModel m;
  m.modules.push_back(m_tb.module);
  m.frames.push_back({ModuleIdx(0), it->second.beginRva + 2});
  const fs::path pdb = m_tb.pdb;
  SymbolService svc({}, {});
  registerDiaProvider(svc, m_opener);
  svc.setLocate([pdb](const Module&) -> Result<fs::path> { return pdb; });
  svc.resolveAllAsync(m);
  svc.waitAll();
  ASSERT_EQ(m.modules[0].state, SymbolState::Resolved) << svc.moduleError(ModuleIdx(0));
  const ResolvedAddress& a = svc.lookup(FrameIdx(0));
  ASSERT_TRUE(a.resolved);
  EXPECT_EQ(svc.symbolStrings().get(a.function), "hot_a");
}

TEST(DiaUndecorateWindows, UndecoratesMsvcNames) {
  EXPECT_EQ(undecorateMsvc("?bar@Foo@ns@@QEAAXXZ"), "ns::Foo::bar");
  EXPECT_EQ(undecorateMsvc("plain_c_name"), "plain_c_name");
}

}  // namespace
}  // namespace tachyon::symbols
