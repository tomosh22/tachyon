#include "symbols/demangle.h"
#include "symbols/json_symbol_provider.h"
#include "symbols/range_table.h"
#include "symbols/symbol_strings.h"
#include "symbols_test_util.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <thread>

namespace tachyon::symbols {
namespace {

std::filesystem::path writeTemp(const std::string& name, const std::string& content) {
  const auto p = std::filesystem::temp_directory_path() / ("tachyon_symbols_" + name);
  std::ofstream(p, std::ios::binary) << content;
  return p;
}

// Hand-built module: two functions with a gap in between, an inline range and a line gap.
constexpr const char* kHandBuilt = R"({
 "schema": "tachyon-symbols-1", "scenario": "hand",
 "modules": [ { "module_idx": 0, "name": "m.dll", "path": "bin/m.dll", "base": 4096, "size": 65536,
   "id_kind": "gnu-build-id", "id": "00112233445566778899aabbccddeeff00112233", "age": 0,
   "functions": [
     { "name": "b_second", "start_rva": 512, "size": 64, "file": "b.cpp", "line": 50,
       "lines": [ {"rva": 512, "size": 64, "line": 50} ], "inlines": [] },
     { "name": "a_first", "start_rva": 256, "size": 128, "file": "a.cpp", "line": 10,
       "lines": [ {"rva": 256, "size": 32, "line": 10}, {"rva": 320, "size": 64, "line": 14} ],
       "inlines": [ {"rva": 336, "size": 16, "chain": [
          {"function": "inner", "file": "i.h", "line": 3}, {"function": "outer", "file": "o.h", "line": 9} ]} ] }
   ] } ] })";

Module handModule() {
  Module m;
  m.path = "/usr/lib/M.DLL";  // filename match is case-insensitive
  return m;
}

TEST(JsonSymbolProviderTest, ResolvesFunctionsLinesAndGaps) {
  SymbolStringTable strings;
  JsonSymbolProvider p(strings);
  ASSERT_TRUE(p.load(handModule(), writeTemp("hand.symbols.json", kHandBuilt)).ok());

  ResolvedAddress a = p.resolve(256);
  ASSERT_TRUE(a.resolved);
  EXPECT_EQ(strings.get(a.function), "a_first");
  EXPECT_EQ(a.functionStartRva, 256u);
  EXPECT_EQ(strings.get(a.file), "a.cpp");
  EXPECT_EQ(a.line, 10u);
  EXPECT_TRUE(a.inlines.empty());

  a = p.resolve(383);  // last byte of a_first
  ASSERT_TRUE(a.resolved);
  EXPECT_EQ(a.line, 14u);

  EXPECT_FALSE(p.resolve(384).resolved);  // gap between the functions
  EXPECT_FALSE(p.resolve(511).resolved);
  EXPECT_TRUE(p.resolve(512).resolved);
  EXPECT_EQ(strings.get(p.resolve(575).function), "b_second");
  EXPECT_FALSE(p.resolve(576).resolved);
  EXPECT_FALSE(p.resolve(0).resolved);
  EXPECT_FALSE(p.resolve(255).resolved);
  EXPECT_FALSE(p.resolve(~0ull).resolved);
}

TEST(JsonSymbolProviderTest, InlineChainsAreInnermostFirst) {
  SymbolStringTable strings;
  JsonSymbolProvider p(strings);
  ASSERT_TRUE(p.load(handModule(), writeTemp("hand2.symbols.json", kHandBuilt)).ok());

  ResolvedAddress a = p.resolve(340);
  ASSERT_TRUE(a.resolved);
  EXPECT_EQ(strings.get(a.function), "a_first");  // the physical function
  ASSERT_EQ(a.inlines.size(), 2u);
  EXPECT_EQ(strings.get(a.inlines[0].function), "inner");
  EXPECT_EQ(strings.get(a.inlines[0].file), "i.h");
  EXPECT_EQ(a.inlines[0].line, 3u);
  EXPECT_EQ(strings.get(a.inlines[1].function), "outer");
  EXPECT_EQ(a.inlines[1].line, 9u);

  EXPECT_EQ(p.resolve(336).inlines.size(), 2u);  // first byte of the inline range
  EXPECT_EQ(p.resolve(351).inlines.size(), 2u);  // last byte
  EXPECT_TRUE(p.resolve(352).inlines.empty());   // one past the end
  EXPECT_TRUE(p.resolve(335).inlines.empty());
}

TEST(JsonSymbolProviderTest, FixtureInlineChainFromTiny) {
  // tiny: Engine::update at rva 4608 has an inline range [4752, 4784) with chain Vec3::length,
  // Entity::distance.
  model::TraceModel m = testutil::loadModel("tiny");
  ASSERT_FALSE(m.modules.empty());
  SymbolStringTable strings;
  JsonSymbolProvider p(strings);
  ASSERT_TRUE(p.load(m.modules[0], testutil::fixtureFile("tiny", ".symbols.json")).ok());
  const ResolvedAddress a = p.resolve(4760);
  ASSERT_TRUE(a.resolved);
  EXPECT_EQ(strings.get(a.function), "Engine::update");
  EXPECT_EQ(a.functionStartRva, 4608u);
  EXPECT_EQ(strings.get(a.file), "src/engine.cpp");
  EXPECT_EQ(a.line, 104u);
  ASSERT_EQ(a.inlines.size(), 2u);
  EXPECT_EQ(strings.get(a.inlines[0].function), "Vec3::length");
  EXPECT_EQ(strings.get(a.inlines[1].function), "Entity::distance");
}

TEST(JsonSymbolProviderTest, LinesAndBounds) {
  SymbolStringTable strings;
  JsonSymbolProvider p(strings);
  ASSERT_TRUE(p.load(handModule(), writeTemp("hand3.symbols.json", kHandBuilt)).ok());

  auto lines = p.linesForFunction(256);
  ASSERT_TRUE(lines.ok());
  ASSERT_EQ(lines->size(), 2u);
  EXPECT_EQ((*lines)[0].rva, 256u);
  EXPECT_EQ((*lines)[0].size, 32u);
  EXPECT_EQ((*lines)[1].line, 14u);
  EXPECT_EQ(strings.get((*lines)[1].file), "a.cpp");
  EXPECT_EQ(p.linesForFunction(257).error().code, ErrorCode::NotFound);  // not an exact start

  auto b = p.functionBounds(300);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b->startRva, 256u);
  EXPECT_EQ(b->size, 128u);
  EXPECT_EQ(strings.get(b->name), "a_first");
  EXPECT_EQ(p.functionBounds(400).error().code, ErrorCode::NotFound);
}

TEST(JsonSymbolProviderTest, ModuleMatchingAndErrors) {
  SymbolStringTable strings;
  const auto file = writeTemp("hand4.symbols.json", kHandBuilt);
  {
    JsonSymbolProvider p(strings);
    Module other;
    other.path = "other.dll";
    EXPECT_EQ(p.load(other, file).error().code, ErrorCode::NotFound);
  }
  {
    // Matching id wins even with a different path; a different id rejects even with the same path.
    JsonSymbolProvider p(strings);
    Module byId;
    byId.path = "renamed.dll";
    byId.id.kind = trace::kModuleIdGnuBuildId;
    byId.id.len = 20;
    const uint8_t bytes[20] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                               0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33};
    std::copy(bytes, bytes + 20, byId.id.bytes.begin());
    EXPECT_TRUE(p.load(byId, file).ok());
    JsonSymbolProvider q(strings);
    byId.path = "m.dll";
    byId.id.bytes[3] ^= 1;
    EXPECT_EQ(q.load(byId, file).error().code, ErrorCode::NotFound);
  }
  {
    JsonSymbolProvider p(strings);
    EXPECT_EQ(p.load(handModule(), "/nonexistent/x.symbols.json").error().code, ErrorCode::NotFound);
    EXPECT_EQ(p.load(handModule(), writeTemp("bad1.symbols.json", "not json")).error().code,
              ErrorCode::CorruptData);
    EXPECT_EQ(p.load(handModule(), writeTemp("bad2.symbols.json", R"({"schema":"other","modules":[]})"))
                  .error()
                  .code,
              ErrorCode::CorruptData);
    EXPECT_EQ(
        p.load(
             handModule(),
             writeTemp(
                 "bad3.symbols.json",
                 R"({"schema":"tachyon-symbols-1","modules":[{"name":"m.dll","functions":[{"name":"x"}]}]})"))
            .error()
            .code,
        ErrorCode::CorruptData);
  }
}

TEST(FunctionRangeTableTest, BinarySearchBoundaries) {
  SymbolStringTable strings;
  const StringIdx n = strings.intern("n");
  FunctionRangeTable t({{300, 50, n}, {100, 100, n}, {200, 0, n}, {1000, 8, n}});
  ASSERT_EQ(t.all().size(), 4u);
  EXPECT_EQ(t.all()[0].startRva, 100u);  // sorted
  EXPECT_EQ(t.find(100)->startRva, 100u);
  EXPECT_EQ(t.find(199)->startRva, 100u);
  EXPECT_EQ(t.find(200), nullptr);  // zero-size function and gap
  EXPECT_EQ(t.find(299), nullptr);
  EXPECT_EQ(t.find(349)->startRva, 300u);
  EXPECT_EQ(t.find(350), nullptr);
  EXPECT_EQ(t.find(1007)->startRva, 1000u);
  EXPECT_EQ(t.find(1008), nullptr);
  EXPECT_EQ(t.find(99), nullptr);
  EXPECT_EQ(t.findExact(300), 2u);
  EXPECT_EQ(t.findExact(301), FunctionRangeTable::kNotFound);
  EXPECT_EQ(FunctionRangeTable().find(5), nullptr);

  const LineRange lines[] = {{10, 5, n, 1}, {15, 5, n, 2}, {30, 4, n, 3}};
  EXPECT_EQ(findLineRange(lines, 10)->line, 1u);
  EXPECT_EQ(findLineRange(lines, 19)->line, 2u);
  EXPECT_EQ(findLineRange(lines, 20), nullptr);
  EXPECT_EQ(findLineRange(lines, 33)->line, 3u);
  EXPECT_EQ(findLineRange(lines, 9), nullptr);
}

TEST(SymbolStringTableTest, InternFindGet) {
  SymbolStringTable t;
  EXPECT_EQ(t.size(), 0u);
  EXPECT_EQ(t.get(StringIdx(0)), "");
  const StringIdx a = t.intern("alpha");
  const StringIdx b = t.intern("beta");
  EXPECT_NE(a, b);
  EXPECT_EQ(t.intern("alpha"), a);
  EXPECT_EQ(t.intern(""), StringIdx(0));
  EXPECT_EQ(t.find("beta"), b);
  EXPECT_FALSE(t.find("gamma").valid());
  EXPECT_EQ(t.get(a), "alpha");
  EXPECT_EQ(t.get(StringIdx(9999)), "");
  EXPECT_EQ(t.get(StringIdx::invalid()), "");

  // Views stay valid across growth (unlike trace::StringTable).
  const std::string_view view = t.get(a);
  for (int i = 0; i < 20000; ++i) (void)t.intern("filler-" + std::to_string(i));
  EXPECT_EQ(view, "alpha");
  EXPECT_EQ(t.get(a).data(), view.data());
  const std::string big(100000, 'x');
  EXPECT_EQ(t.get(t.intern(big)), big);
}

TEST(SymbolStringTableTest, ConcurrentInternAndRead) {
  SymbolStringTable t;
  std::atomic<bool> stop{false};
  std::atomic<size_t> bad{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 4; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < 5000; ++i) {
        const std::string s = "w" + std::to_string(w) + "-" + std::to_string(i);
        const StringIdx idx = t.intern(s);
        if (t.get(idx) != s) ++bad;
      }
    });
  }
  for (int r = 0; r < 4; ++r) {
    threads.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const size_t n = t.size();
        for (size_t i = 1; i <= n; i += 7) {
          if (t.get(StringIdx(static_cast<uint32_t>(i))).empty()) ++bad;
        }
      }
    });
  }
  for (int i = 0; i < 4; ++i) threads[static_cast<size_t>(i)].join();
  stop.store(true, std::memory_order_release);
  for (size_t i = 4; i < threads.size(); ++i) threads[i].join();
  EXPECT_EQ(bad.load(), 0u);
  EXPECT_EQ(t.size(), 20000u);
}

TEST(DemangleTest, DefaultsArePassThroughForPlainNames) {
#if !defined(_WIN32)
  EXPECT_EQ(demangleMsvc("?foo@@YAXXZ"), "?foo@@YAXXZ");  // undecorated through DbgHelp on Windows (WP-22)
#else
  EXPECT_EQ(demangleMsvc("?foo@@YAXXZ"), "foo");
#endif
  EXPECT_EQ(demangleItanium("Engine::update"), "Engine::update");
  EXPECT_EQ(demangle("main"), "main");
  EXPECT_EQ(demangleItanium("_Znot_valid"), "_Znot_valid");
#if __has_include(<cxxabi.h>)
  EXPECT_EQ(demangleItanium("_ZN3Foo3barEv"), "Foo::bar()");
  EXPECT_EQ(demangle("_ZN3Foo3barEv"), "Foo::bar()");
#endif
}

}  // namespace
}  // namespace tachyon::symbols
