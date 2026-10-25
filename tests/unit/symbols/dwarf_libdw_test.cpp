// Real libdw session over the fixture binaries (WP-72): functions, inline chains, lines, RVA mapping for PIE
// and non-PIE executables, hot/cold splits and the symbol-table fallback.
#include "dwarf_test_util.h"

#include <algorithm>
#include <set>

namespace tachyon::symbols {
namespace {

using namespace dwarftest;

constexpr const char* kWidgetWork = "_ZN2fx6Widget4workEi";

class DwarfLibdw : public testing::TestWithParam<const char*> {};

INSTANTIATE_TEST_SUITE_P(Fixtures, DwarfLibdw, testing::Values("dwarf_fixture_pie", "dwarf_fixture_nopie"));

TEST_P(DwarfLibdw, LoadsAndUsesDwarf) {
  Loaded l(fixture(GetParam()));
  ASSERT_TRUE(l.status.ok()) << l.status.error().toString();
  EXPECT_TRUE(l.provider->hasDwarf());
}

TEST_P(DwarfLibdw, LoadBiasMatchesTheBinaryKind) {
  Loaded l(fixture(GetParam()));
  ASSERT_TRUE(l.status.ok());
  if (std::string(GetParam()) == "dwarf_fixture_pie") {
    EXPECT_EQ(l.provider->loadBias(), 0u);
  } else {
    EXPECT_NE(l.provider->loadBias(), 0u);
    EXPECT_EQ(l.provider->loadBias() % 4096, 0u);
  }
}

TEST_P(DwarfLibdw, ResolvesFunctionFileAndLine) {
  const auto file = fixture(GetParam());
  Loaded l(file);
  ASSERT_TRUE(l.status.ok());
  const auto rva = symbolRva(file, "hot_c");
  ASSERT_TRUE(rva.has_value());

  const ResolvedAddress r = l.provider->resolve(*rva);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(l.name(r.function), "hot_c");
  EXPECT_EQ(r.functionStartRva, *rva);
  EXPECT_EQ(baseName(l.name(r.file)), "fixture_main.cpp");
  EXPECT_EQ(r.line, markerLine("fixture_main.cpp", "hot_c"));
  EXPECT_TRUE(r.inlines.empty());

  // Somewhere inside the loop body the line must be the loop's line.
  auto lines = l.provider->linesForFunction(*rva);
  ASSERT_TRUE(lines.ok());
  const uint32_t loopLine = markerLine("fixture_main.cpp", "hot_c_loop");
  const auto it = std::find_if(lines.value().begin(), lines.value().end(),
                               [&](const LineRange& lr) { return lr.line == loopLine; });
  ASSERT_NE(it, lines.value().end());
  const ResolvedAddress inLoop = l.provider->resolve(it->rva + it->size / 2);
  EXPECT_EQ(inLoop.line, loopLine);
  EXPECT_EQ(l.name(inLoop.function), "hot_c");
}

TEST_P(DwarfLibdw, DemangledQualifiedNames) {
  const auto file = fixture(GetParam());
  Loaded l(file);
  ASSERT_TRUE(l.status.ok());
  const auto rva = symbolRva(file, kWidgetWork);
  ASSERT_TRUE(rva.has_value());
  const ResolvedAddress r = l.provider->resolve(*rva);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(l.name(r.function), "fx::Widget::work(int)");
  EXPECT_EQ(r.line, markerLine("fixture_main.cpp", "widget_work"));
}

TEST_P(DwarfLibdw, InlineChainInnermostFirst) {
  const auto file = fixture(GetParam());
  Loaded l(file);
  ASSERT_TRUE(l.status.ok());
  const auto start = symbolRva(file, "with_inlines");
  ASSERT_TRUE(start.has_value());
  auto lines = l.provider->linesForFunction(*start);
  ASSERT_TRUE(lines.ok());

  // The line-table row that executes the body of innerHelper (in the header) inside with_inlines.
  const uint32_t innerBody = markerLine("fixture_inl.h", "inner_body");
  const uint32_t outerCalls = markerLine("fixture_inl.h", "outer_calls_inner");
  const uint32_t callLine = markerLine("fixture_main.cpp", "with_inlines_call");
  ASSERT_NE(innerBody, 0u);
  bool sawChain = false;
  for (const LineRange& lr : lines.value()) {
    if (lr.line != innerBody) continue;
    const ResolvedAddress r = l.provider->resolve(lr.rva);
    ASSERT_TRUE(r.resolved);
    EXPECT_EQ(l.name(r.function), "with_inlines");
    EXPECT_EQ(r.line, innerBody);
    if (r.inlines.size() == 2) {
      sawChain = true;
      EXPECT_EQ(l.name(r.inlines[0].function), "innerHelper(int)");
      EXPECT_EQ(l.name(r.inlines[1].function), "outerHelper(int)");
      EXPECT_EQ(r.inlines[0].line, innerBody);
      EXPECT_EQ(baseName(l.name(r.inlines[0].file)), "fixture_inl.h");
      // Outer frame: the call site of innerHelper inside outerHelper.
      EXPECT_EQ(r.inlines[1].line, outerCalls);
      EXPECT_EQ(baseName(l.name(r.inlines[1].file)), "fixture_inl.h");
      EXPECT_NE(r.inlines[0].line, 0u);
      EXPECT_NE(r.inlines[1].line, 0u);
    }
  }
  EXPECT_TRUE(sawChain) << "no address of with_inlines resolved to the outerHelper -> innerHelper chain";
  (void)callLine;
}

// Every line-table range of the fixture's functions: our function / inline chain / lines against a reference
// symbolizer.
// `gnu`: binutils addr2line, whose inline frames are unreliable (see symbolize()), so only the physical
// function and the line-table position are compared; otherwise the whole chain against llvm-symbolizer.
void expectAgreesWithSymbolizer(const std::filesystem::path& file, bool gnu) {
  Loaded l(file);
  ASSERT_TRUE(l.status.ok());
  const uint64_t bias = l.provider->loadBias();

  size_t compared = 0;
  for (const char* sym : {"hot_c", "with_inlines", "split_fn", "_ZN2fx6Widget4workEi", "main"}) {
    const auto start = symbolRva(file, sym);
    ASSERT_TRUE(start.has_value()) << sym;
    auto lines = l.provider->linesForFunction(*start);
    ASSERT_TRUE(lines.ok()) << sym;
    ASSERT_FALSE(lines.value().empty()) << sym;
    for (const LineRange& lr : lines.value()) {
      const ResolvedAddress r = l.provider->resolve(lr.rva);
      ASSERT_TRUE(r.resolved);
      std::vector<A2lFrame> ref = symbolize(file, lr.rva + bias, gnu);
      if (ref.empty()) continue;
      // The references name the cold part of a split function by its symbol ("split_fn.cold"); we report the
      // function (documented behaviour: all pieces share one identity).
      for (A2lFrame& f : ref) {
        if (f.function.size() > 5 && f.function.compare(f.function.size() - 5, 5, ".cold") == 0)
          f.function.resize(f.function.size() - 5);
      }
      // The reference lists the inline frames first and the physical function last.
      EXPECT_EQ(l.name(r.function), ref.back().function) << std::hex << lr.rva;
      if (!gnu) {
        ASSERT_EQ(ref.size(), r.inlines.size() + 1) << sym << " rva " << std::hex << lr.rva;
        for (size_t k = 0; k < r.inlines.size(); ++k) {
          EXPECT_EQ(l.name(r.inlines[k].function), ref[k].function) << std::hex << lr.rva << " frame " << k;
          // A reference line of 0 is compiler-generated code; our frames inherit the next outer position
          // then.
          if (ref[k].line != 0)
            EXPECT_EQ(r.inlines[k].line, ref[k].line) << std::hex << lr.rva << " frame " << k;
          EXPECT_NE(r.inlines[k].line, 0u);
          EXPECT_EQ(baseName(l.name(r.inlines[k].file)), baseName(ref[k].file));
        }
      }
      if (r.line != 0) {
        EXPECT_EQ(r.line, ref.front().line) << std::hex << lr.rva;
        EXPECT_EQ(baseName(l.name(r.file)), baseName(ref.front().file));
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 20u);
}

TEST_P(DwarfLibdw, EveryLineRangeAgreesWithLlvmSymbolizer) {
  if (!haveTool("llvm-symbolizer")) GTEST_SKIP() << "llvm-symbolizer not available";
  expectAgreesWithSymbolizer(fixture(GetParam()), false);
}

TEST(DwarfLibdwSplit, EveryLineRangeAgreesWithLlvmSymbolizer) {
  const auto file = fixture("dwarf_fixture_split");
  if (!std::filesystem::exists(file)) GTEST_SKIP() << "g++ not available: no gcc-built fixture";
  if (!haveTool("llvm-symbolizer")) GTEST_SKIP() << "llvm-symbolizer not available";
  expectAgreesWithSymbolizer(file, false);
}

TEST(DwarfLibdwSplit, EveryLineRangeAgreesWithAddr2line) {
  const auto file = fixture("dwarf_fixture_split");
  if (!std::filesystem::exists(file)) GTEST_SKIP() << "g++ not available: no gcc-built fixture";
  if (!haveTool("addr2line")) GTEST_SKIP() << "addr2line not available";
  expectAgreesWithSymbolizer(file, true);
}

TEST_P(DwarfLibdw, FunctionBoundsAndLinesAreConsistent) {
  const auto file = fixture(GetParam());
  Loaded l(file);
  ASSERT_TRUE(l.status.ok());
  const auto start = symbolRva(file, "hot_c");
  const uint64_t size = symbolSize(file, "hot_c");
  ASSERT_TRUE(start.has_value());
  auto b = l.provider->functionBounds(*start + 3);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value().startRva, *start);
  EXPECT_EQ(b.value().size, size);
  EXPECT_EQ(l.name(b.value().name), "hot_c");
  EXPECT_FALSE(l.provider->functionBounds(*start + size + 0x100000).ok());

  auto lines = l.provider->linesForFunction(*start);
  ASSERT_TRUE(lines.ok());
  uint64_t prevEnd = 0;
  for (const LineRange& lr : lines.value()) {
    EXPECT_GE(lr.rva, *start);
    EXPECT_LE(lr.rva + lr.size, *start + size);
    EXPECT_GE(lr.rva, prevEnd);
    prevEnd = lr.rva + lr.size;
  }
  EXPECT_EQ(l.provider->linesForFunction(*start + 1).error().code, ErrorCode::NotFound);
}

TEST_P(DwarfLibdw, ReturnAddressRuleJustWorks) {
  // The service resolves rva - 1 for non-leaf frames: the last byte of a function still belongs to it, and
  // the first byte of the next function does not.
  const auto file = fixture(GetParam());
  Loaded l(file);
  ASSERT_TRUE(l.status.ok());
  const auto start = symbolRva(file, "hot_c");
  const uint64_t size = symbolSize(file, "hot_c");
  const ResolvedAddress last = l.provider->resolve(*start + size - 1);
  ASSERT_TRUE(last.resolved);
  EXPECT_EQ(l.name(last.function), "hot_c");
  EXPECT_EQ(last.functionStartRva, *start);
  const ResolvedAddress after = l.provider->resolve(*start + size);
  EXPECT_FALSE(after.resolved && after.functionStartRva == *start);
}

TEST(DwarfLibdwPieVsNoPie, SameSourceResolvesToTheSameFunctions) {
  Loaded pie(fixture("dwarf_fixture_pie"));
  Loaded nopie(fixture("dwarf_fixture_nopie"));
  ASSERT_TRUE(pie.status.ok());
  ASSERT_TRUE(nopie.status.ok());
  for (const char* sym : {"hot_c", "with_inlines", "_ZN2fx6Widget4workEi"}) {
    const ResolvedAddress a = pie.provider->resolve(*symbolRva(fixture("dwarf_fixture_pie"), sym));
    const ResolvedAddress b = nopie.provider->resolve(*symbolRva(fixture("dwarf_fixture_nopie"), sym));
    ASSERT_TRUE(a.resolved && b.resolved) << sym;
    EXPECT_EQ(pie.name(a.function), nopie.name(b.function));
    EXPECT_EQ(a.line, b.line);
  }
}

TEST(DwarfLibdwSplit, HotColdPiecesMapToOneFunction) {
  const auto file = fixture("dwarf_fixture_split");
  if (!std::filesystem::exists(file)) GTEST_SKIP() << "g++ not available: no split fixture";
  Loaded l(file);
  ASSERT_TRUE(l.status.ok()) << l.status.error().toString();
  const auto hot = symbolRva(file, "split_fn");
  const auto cold = symbolRva(file, "split_fn.cold");
  ASSERT_TRUE(hot.has_value());
  if (!cold.has_value()) GTEST_SKIP() << "the compiler did not split split_fn";
  ASSERT_NE(*hot, *cold);

  const ResolvedAddress h = l.provider->resolve(*hot);
  const ResolvedAddress c = l.provider->resolve(*cold);
  ASSERT_TRUE(h.resolved);
  ASSERT_TRUE(c.resolved);
  EXPECT_EQ(l.name(h.function), "split_fn");
  EXPECT_EQ(l.name(c.function), "split_fn");
  EXPECT_EQ(h.functionStartRva, *hot);  // the entry is the hot part
  EXPECT_EQ(c.functionStartRva, *hot);  // the cold part maps to the same function start
  EXPECT_EQ(h.function, c.function);
  EXPECT_NE(c.line, 0u);

  // Bounds: start = entry in both cases; size = the hot piece.
  auto bh = l.provider->functionBounds(*hot);
  auto bc = l.provider->functionBounds(*cold);
  ASSERT_TRUE(bh.ok() && bc.ok());
  EXPECT_EQ(bh.value().startRva, *hot);
  EXPECT_EQ(bc.value().startRva, *hot);
  EXPECT_EQ(bh.value().size, symbolSize(file, "split_fn"));

  // Lines cover both pieces.
  auto lines = l.provider->linesForFunction(*hot);
  ASSERT_TRUE(lines.ok());
  bool inHot = false, inCold = false;
  for (const LineRange& lr : lines.value()) {
    if (lr.rva >= *hot && lr.rva < *hot + symbolSize(file, "split_fn")) inHot = true;
    if (lr.rva >= *cold && lr.rva < *cold + symbolSize(file, "split_fn.cold")) inCold = true;
  }
  EXPECT_TRUE(inHot);
  EXPECT_TRUE(inCold);
  const uint32_t coldCall = markerLine("fixture_main.cpp", "split_cold_call");
  EXPECT_TRUE(std::any_of(lines.value().begin(), lines.value().end(), [&](const LineRange& lr) {
    return lr.line == coldCall && lr.rva >= *cold && lr.rva < *cold + symbolSize(file, "split_fn.cold");
  }));
  // The cold start itself is not a function start.
  EXPECT_EQ(l.provider->linesForFunction(*cold).error().code, ErrorCode::NotFound);
}

TEST(DwarfLibdwSymtab, BinaryWithoutDebugInfoFallsBackToTheSymbolTable) {
  const auto file = fixture("dwarf_fixture_nodebug");
  Loaded l(file);
  ASSERT_TRUE(l.status.ok()) << l.status.error().toString();
  EXPECT_FALSE(l.provider->hasDwarf());
  const auto rva = symbolRva(file, "hot_c");
  const uint64_t size = symbolSize(file, "hot_c");
  ASSERT_TRUE(rva.has_value());

  const ResolvedAddress r = l.provider->resolve(*rva + 4);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(l.name(r.function), "hot_c");
  EXPECT_EQ(r.functionStartRva, *rva);
  EXPECT_EQ(r.line, 0u);
  EXPECT_TRUE(r.file == StringIdx() || l.name(r.file).empty());
  EXPECT_TRUE(r.inlines.empty());

  const ResolvedAddress mangled = l.provider->resolve(*symbolRva(file, kWidgetWork));
  ASSERT_TRUE(mangled.resolved);
  EXPECT_EQ(l.name(mangled.function), "fx::Widget::work(int)");

  auto b = l.provider->functionBounds(*rva);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value().size, size);
  auto lines = l.provider->linesForFunction(*rva);
  ASSERT_TRUE(lines.ok());
  EXPECT_TRUE(lines.value().empty());
  EXPECT_FALSE(l.provider->resolve(*rva + size + 0x200000).resolved);
}

#ifdef TACHYON_TESTBED_EXE
// The profiler testbed (built with -g): its known-answer functions and a large sample of its code against
// llvm-symbolizer.
TEST(DwarfTestbed, HotFunctionsResolveToTheirSourceLines) {
  const std::filesystem::path exe(TACHYON_TESTBED_EXE);
  const std::filesystem::path source = std::filesystem::path(TACHYON_TESTBED_SOURCE_DIR) / "scenarios.cpp";
  Loaded l(exe);
  ASSERT_TRUE(l.status.ok()) << l.status.error().toString();
  ASSERT_TRUE(l.provider->hasDwarf());
  for (const char* hot : {"hot_a", "hot_b", "hot_c"}) {
    const auto rva = symbolRva(exe, hot);
    ASSERT_TRUE(rva.has_value()) << hot;
    const ResolvedAddress r = l.provider->resolve(*rva);
    ASSERT_TRUE(r.resolved) << hot;
    EXPECT_EQ(l.name(r.function), hot);
    EXPECT_EQ(r.functionStartRva, *rva);
    EXPECT_EQ(baseName(l.name(r.file)), "scenarios.cpp");
    EXPECT_EQ(r.line, lineContaining(source, std::string("uint64_t ") + hot + "(uint64_t iterations")) << hot;
    // The burn<> template is inlined into every hot function: its loop rows carry an inline frame, and no row
    // of the function resolves to anything but the function itself.
    auto lines = l.provider->linesForFunction(*rva);
    ASSERT_TRUE(lines.ok());
    bool sawBurn = false;
    for (const LineRange& lr : lines.value()) {
      const ResolvedAddress a = l.provider->resolve(lr.rva);
      EXPECT_EQ(l.name(a.function), hot);
      EXPECT_EQ(a.functionStartRva, *rva);
      for (const InlineFrame& f : a.inlines) {
        if (l.name(f.function).find("burn<") != std::string::npos) sawBurn = true;
      }
    }
    EXPECT_TRUE(sawBurn) << hot;
  }
}

TEST(DwarfTestbed, InlineScenarioProducesInnermostFirstChains) {
  const std::filesystem::path exe(TACHYON_TESTBED_EXE);
  Loaded l(exe);
  ASSERT_TRUE(l.status.ok());
  const auto rva = symbolRva(exe, "inline_hot_loop");
  ASSERT_TRUE(rva.has_value());
  auto lines = l.provider->linesForFunction(*rva);
  ASSERT_TRUE(lines.ok());
  std::set<std::string> outerFrames;
  for (const LineRange& lr : lines.value()) {
    const ResolvedAddress a = l.provider->resolve(lr.rva);
    EXPECT_EQ(l.name(a.function), "inline_hot_loop");
    for (size_t k = 0; k < a.inlines.size(); ++k) EXPECT_NE(a.inlines[k].line, 0u);
    if (a.inlines.size() >= 2 && l.name(a.inlines[0].function).find("burn<") != std::string::npos) {
      outerFrames.insert(l.name(a.inlines.back().function));
    }
  }
  // Each of inline_a/b/c wraps its own burn<Salt> instantiation: burn<> is the innermost frame, inline_x
  // outside.
  for (const char* wanted : {"inline_a", "inline_b", "inline_c"}) {
    EXPECT_TRUE(std::any_of(outerFrames.begin(), outerFrames.end(),
                            [&](const std::string& n) { return n.find(wanted) != std::string::npos; }))
        << wanted << " never seen as the outer frame of a burn<> chain";
  }
}

TEST(DwarfTestbed, SampledAddressesAgreeWithLlvmSymbolizer) {
  if (!haveTool("llvm-symbolizer")) GTEST_SKIP() << "llvm-symbolizer not available";
  const std::filesystem::path exe(TACHYON_TESTBED_EXE);
  Loaded l(exe);
  ASSERT_TRUE(l.status.ok());
  auto elf = ElfFile::open(exe);
  ASSERT_TRUE(elf.ok());
  std::vector<uint64_t> rvas;
  for (const ElfSection& s : elf.value().sections()) {
    if ((s.flags & kElfShfExecinstr) == 0 || s.name != ".text") continue;
    for (uint64_t a = s.addr; a < s.addr + s.size; a += 13) rvas.push_back(a - elf.value().loadBias());
  }
  ASSERT_GT(rvas.size(), 5000u);
  const auto ref = symbolizeBatch(exe, rvas, true);
  ASSERT_EQ(ref.size(), rvas.size());

  size_t compared = 0, funcMismatch = 0, lineMismatch = 0, inlineMismatch = 0;
  // The reference prints the short DW_AT_name of each function (--functions=short): independent of any
  // demangler. Our display name (demangled linkage name, or qualified source name) must contain it. libstdc++
  // 13 cannot demangle C++20 requires-clauses and then leaves the raw "_Z..." name, which is counted, not
  // compared.
  size_t undemangled = 0;
  auto sameFunction = [&](const std::string& ours, const std::string& shortName) {
    if (ours.rfind("_Z", 0) == 0) {
      ++undemangled;
      return true;
    }
    // Template arguments are spelled differently (const char * / char const*): compare the bare name.
    std::string bare = shortName.substr(0, shortName.find('<'));
    if (bare.rfind("operator ", 0) == 0)
      bare = bare.substr(9);  // conversion operators: "operator shared_ptr"
    return ours.find(bare) != std::string::npos;
  };
  for (size_t i = 0; i < rvas.size(); ++i) {
    const auto& frames = ref[i];
    if (frames.empty() || frames.back().function == "??") continue;
    const ResolvedAddress r = l.provider->resolve(rvas[i]);
    if (!r.resolved) {
      ++funcMismatch;
      continue;
    }
    ++compared;
    if (!sameFunction(l.name(r.function), frames.back().function)) {
      if (funcMismatch++ < 8) {
        ADD_FAILURE() << "rva " << std::hex << rvas[i] << ": ours '" << l.name(r.function) << "' theirs '"
                      << frames.back().function << "'";
      }
    }
    if (frames.size() != r.inlines.size() + 1) {
      if (inlineMismatch++ < 8) {
        ADD_FAILURE() << "rva " << std::hex << rvas[i] << ": " << std::dec << r.inlines.size()
                      << " inline frames, reference " << frames.size() - 1 << "; ours[0]='"
                      << (r.inlines.empty() ? "" : l.name(r.inlines[0].function)) << "' theirs[0]='"
                      << frames[0].function << "'";
      }
      continue;
    }
    for (size_t k = 0; k < r.inlines.size(); ++k) {
      if (!sameFunction(l.name(r.inlines[k].function), frames[k].function)) {
        if (inlineMismatch++ < 8) {
          ADD_FAILURE() << "rva " << std::hex << rvas[i] << " frame " << k << ": ours '"
                        << l.name(r.inlines[k].function) << "' theirs '" << frames[k].function << "'";
        }
      }
    }
    if (frames.front().line != 0 && r.line != frames.front().line) ++lineMismatch;
  }
  std::printf(
      "[dwarf testbed] compared %zu addresses: %zu function, %zu inline-chain, %zu line mismatches; %zu "
      "names the system demangler cannot handle\n",
      compared, funcMismatch, inlineMismatch, lineMismatch, undemangled);
  EXPECT_GT(compared, 5000u);
  EXPECT_EQ(funcMismatch, 0u);
  EXPECT_EQ(lineMismatch, 0u);
  EXPECT_EQ(inlineMismatch, 0u);
}
#endif  // TACHYON_TESTBED_EXE

}  // namespace
}  // namespace tachyon::symbols
