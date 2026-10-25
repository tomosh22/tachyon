// Platform-neutral DwarfSymbolProvider logic over a fake IDwarfSession (WP-72): RVA mapping, entry-pc
// function identity for split functions, inline position fallbacks, name caching, symbol-table fallback and
// load errors. The carrier ELF files are the fixture binaries (their real symbol tables feed the fallback);
// the DWARF part is supplied by the fake in DWARF address space.
#include "dwarf_test_util.h"
#include "symbols/symbol_service.h"

#include <unistd.h>

#include <atomic>
#include <functional>
#include <map>
#include <thread>

namespace tachyon::symbols {
namespace {

using namespace dwarftest;

struct FakeFunction {
  uint64_t id = 0;
  uint64_t entry = 0;
  std::vector<DwarfAddrRange> ranges;
  std::string name;
  std::string source;  // qualified source name, served by sourceName(id)
  struct Inline {
    DwarfAddrRange range;
    std::vector<DwarfInlineSite> chain;  // innermost first
  };
  std::vector<Inline> inlines;
};

struct FakeLine {
  DwarfAddrRange range;
  std::string file;
  uint32_t line;
};

class FakeSession final : public IDwarfSession {
 public:
  std::vector<FakeFunction> functions;
  std::vector<FakeLine> lines;
  mutable int nameCalls = 0;
  int findFunctionCalls = 0;

  std::optional<DwarfFunction> findFunction(uint64_t addr) override {
    ++findFunctionCalls;
    for (const FakeFunction& f : functions) {
      for (const DwarfAddrRange& r : f.ranges) {
        if (addr >= r.lo && addr < r.hi) return DwarfFunction{f.id, f.entry, r, f.id};
      }
    }
    return std::nullopt;
  }
  std::string_view functionName(uint64_t id) override {
    ++nameCalls;
    for (const FakeFunction& f : functions) {
      if (f.id == id) return f.name;
    }
    return {};
  }
  std::string_view sourceName(uint64_t key) override {
    for (const FakeFunction& f : functions) {
      if (f.id == key) return f.source;
    }
    return {};
  }
  std::vector<DwarfAddrRange> functionRanges(uint64_t id) override {
    for (const FakeFunction& f : functions) {
      if (f.id == id) return f.ranges;
    }
    return {};
  }
  std::vector<DwarfInlineSite> findInlines(uint64_t id, uint64_t addr) override {
    for (const FakeFunction& f : functions) {
      if (f.id != id) continue;
      for (const auto& in : f.inlines) {
        if (addr >= in.range.lo && addr < in.range.hi) return in.chain;
      }
    }
    return {};
  }
  std::optional<DwarfLineRow> findLine(uint64_t addr) override {
    for (const FakeLine& l : lines) {
      if (addr >= l.range.lo && addr < l.range.hi)
        return DwarfLineRow{l.range.lo, l.range.hi, l.file, l.line};
    }
    return std::nullopt;
  }
  Result<std::vector<DwarfLineRow>> findLines(uint64_t lo, uint64_t hi) override {
    std::vector<DwarfLineRow> out;
    for (const FakeLine& l : lines) {
      const uint64_t a = std::max(l.range.lo, lo), b = std::min(l.range.hi, hi);
      if (a < b) out.push_back(DwarfLineRow{a, b, l.file, l.line});
    }
    return out;
  }
};

class FakeOpener final : public IDwarfOpener {
 public:
  explicit FakeOpener(FakeSession* session) : m_session(session) {}
  Result<std::unique_ptr<IDwarfSession>> open(const ElfFile&) override {
    ++opens;
    if (fail) return makeError(ErrorCode::CorruptData, "fake: damaged debug info");
    // The session is shared with the test through a forwarding wrapper.
    struct Forward final : IDwarfSession {
      explicit Forward(FakeSession* s) : s(s) {}
      std::optional<DwarfFunction> findFunction(uint64_t a) override { return s->findFunction(a); }
      std::string_view functionName(uint64_t id) override { return s->functionName(id); }
      std::string_view sourceName(uint64_t key) override { return s->sourceName(key); }
      std::vector<DwarfAddrRange> functionRanges(uint64_t id) override { return s->functionRanges(id); }
      std::vector<DwarfInlineSite> findInlines(uint64_t id, uint64_t a) override {
        return s->findInlines(id, a);
      }
      std::optional<DwarfLineRow> findLine(uint64_t a) override { return s->findLine(a); }
      Result<std::vector<DwarfLineRow>> findLines(uint64_t lo, uint64_t hi) override {
        return s->findLines(lo, hi);
      }
      FakeSession* s;
    };
    return std::unique_ptr<IDwarfSession>(new Forward(m_session));
  }
  int opens = 0;
  bool fail = false;

 private:
  FakeSession* m_session;
};

// A provider over a carrier ELF with `session` as its DWARF. The carrier is either a fixture binary (its real
// symbol table feeds the fallback) or a synthetic ELF without any symbols and a chosen load bias.
struct Env {
  FakeSession session;
  std::shared_ptr<FakeOpener> opener;
  SymbolStringTable strings;
  std::unique_ptr<DwarfSymbolProvider> provider;
  Result<void> status = Result<void>();
  uint64_t bias = 0;

  using Setup = std::function<void(FakeSession&, uint64_t bias)>;

  explicit Env(const char* carrier, const Setup& setup) { init(fixture(carrier), setup); }
  // Synthetic carrier: load bias `syntheticBias` (0 = PIE-like).
  explicit Env(uint64_t syntheticBias, const Setup& setup) {
    static int counter = 0;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("tachyon-dwarf-synth-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + ".elf");
    writeSyntheticElf(path, syntheticBias);
    init(path, setup);
    std::filesystem::remove(path);
  }
  std::string name(StringIdx i) const { return std::string(strings.get(i)); }

 private:
  void init(const std::filesystem::path& file, const Setup& setup) {
    auto elf = ElfFile::open(file);
    bias = elf.ok() ? elf.value().loadBias() : 0;
    setup(session, bias);
    opener = std::make_shared<FakeOpener>(&session);
    provider = std::make_unique<DwarfSymbolProvider>(strings, opener);
    status = provider->load(moduleFor(file), file);
  }
};

TEST(DwarfProviderLogic, MapsRvaToTheDwarfAddressSpaceWithTheLoadBias) {
  for (const uint64_t syntheticBias : {uint64_t{0}, uint64_t{0x400000}, uint64_t{0x200000}}) {
    const std::string carrier = "bias " + std::to_string(syntheticBias);
    Env e(syntheticBias, [](FakeSession& s, uint64_t bias) {
      FakeFunction f;
      f.id = 1;
      f.entry = bias + 0x10000;  // far from the real code: no symbol-table interference
      f.ranges = {{bias + 0x10000, bias + 0x10040}};
      f.name = "mapped";
      s.functions.push_back(f);
      s.lines.push_back({{bias + 0x10000, bias + 0x10040}, "/src/a.cpp", 17});
    });
    ASSERT_TRUE(e.status.ok()) << e.status.error().toString();
    EXPECT_EQ(e.provider->loadBias(), e.bias) << carrier;
    const ResolvedAddress r = e.provider->resolve(0x10020);
    ASSERT_TRUE(r.resolved) << carrier;
    EXPECT_EQ(e.name(r.function), "mapped");
    EXPECT_EQ(r.functionStartRva, 0x10000u);
    EXPECT_EQ(e.name(r.file), "/src/a.cpp");
    EXPECT_EQ(r.line, 17u);
    EXPECT_FALSE(e.provider->resolve(0x10040).resolved);
    EXPECT_FALSE(e.provider->resolve(0xffff).resolved);
    // An absolute address (rva + bias given by mistake) must not resolve for a biased binary.
    if (e.bias != 0) EXPECT_FALSE(e.provider->resolve(0x10020 + e.bias).resolved);
    EXPECT_FALSE(e.provider->resolve(~0ull).resolved);  // overflow-safe
  }
}

TEST(DwarfProviderLogic, SplitFunctionIdentityIsTheEntryPc) {
  Env e(0x400000, [](FakeSession& s, uint64_t bias) {
    FakeFunction f;
    f.id = 7;
    f.entry = bias + 0x20100;  // hot part, at the HIGHER address
    f.ranges = {{bias + 0x20000, bias + 0x20030},
                {bias + 0x20100, bias + 0x20180}};  // cold part first by address
    f.name = "split";
    s.functions.push_back(f);
    s.lines.push_back({{bias + 0x20000, bias + 0x20030}, "/src/s.cpp", 90});
    s.lines.push_back({{bias + 0x20100, bias + 0x20180}, "/src/s.cpp", 80});
  });
  ASSERT_TRUE(e.status.ok());
  const ResolvedAddress hot = e.provider->resolve(0x20110);
  const ResolvedAddress cold = e.provider->resolve(0x20010);
  ASSERT_TRUE(hot.resolved && cold.resolved);
  EXPECT_EQ(hot.functionStartRva, 0x20100u);
  EXPECT_EQ(cold.functionStartRva, 0x20100u);
  EXPECT_EQ(hot.function, cold.function);
  EXPECT_EQ(cold.line, 90u);

  auto bHot = e.provider->functionBounds(0x20110);
  auto bCold = e.provider->functionBounds(0x20010);
  ASSERT_TRUE(bHot.ok() && bCold.ok());
  EXPECT_EQ(bHot.value().startRva, 0x20100u);
  EXPECT_EQ(bHot.value().size, 0x80u);
  EXPECT_EQ(bCold.value().startRva, 0x20100u);
  EXPECT_EQ(bCold.value().size, 0x80u);  // the piece holding the entry

  auto lines = e.provider->linesForFunction(0x20100);
  ASSERT_TRUE(lines.ok());
  ASSERT_EQ(lines.value().size(), 2u);
  EXPECT_EQ(lines.value()[0].rva, 0x20000u);  // sorted by rva, covering both pieces
  EXPECT_EQ(lines.value()[0].line, 90u);
  EXPECT_EQ(lines.value()[1].rva, 0x20100u);
  EXPECT_EQ(lines.value()[1].size, 0x80u);
  EXPECT_EQ(e.provider->linesForFunction(0x20000).error().code, ErrorCode::NotFound);  // not the entry
}

TEST(DwarfProviderLogic, InlineChainsAreInnermostFirstWithCallSitePositions) {
  Env e(uint64_t{0}, [](FakeSession& s, uint64_t) {
    FakeFunction f;
    f.id = 1;
    f.entry = 0x30000;
    f.ranges = {{0x30000, 0x30100}};
    f.name = "outer_fn";
    // at 0x30040: inner (called from mid.h:20) inside mid (called from top.cpp:55) inside outer_fn
    f.inlines.push_back({{0x30040, 0x30050}, {{"inner", "/src/mid.h", 20}, {"mid", "/src/top.cpp", 55}}});
    // at 0x30060: positions missing everywhere
    f.inlines.push_back({{0x30060, 0x30070}, {{"in1", "", 0}, {"in2", "", 0}}});
    // at 0x30080: only the outer frame has a position
    f.inlines.push_back({{0x30080, 0x30090}, {{"x1", "", 0}, {"x2", "/src/x.cpp", 9}}});
    s.functions.push_back(f);
    s.lines.push_back({{0x30040, 0x30050}, "/src/in.h", 3});  // the fake returns the first matching row
    s.lines.push_back({{0x30000, 0x30100}, "/src/low.h", 5});
  });
  ASSERT_TRUE(e.status.ok());

  ResolvedAddress r = e.provider->resolve(0x30044);
  ASSERT_EQ(r.inlines.size(), 2u);
  EXPECT_EQ(e.name(r.function), "outer_fn");
  EXPECT_EQ(e.name(r.inlines[0].function), "inner");
  EXPECT_EQ(e.name(r.inlines[1].function), "mid");
  // frame 0: the line-table position; frame 1: the call site of frame 0.
  EXPECT_EQ(e.name(r.inlines[0].file), "/src/in.h");
  EXPECT_EQ(r.inlines[0].line, 3u);
  EXPECT_EQ(e.name(r.inlines[1].file), "/src/mid.h");
  EXPECT_EQ(r.inlines[1].line, 20u);
  EXPECT_EQ(r.line, 3u);

  // No positions at all: everything takes the line-table position of the address (/src/low.h:5).
  r = e.provider->resolve(0x30064);
  ASSERT_EQ(r.inlines.size(), 2u);
  for (const InlineFrame& f : r.inlines) {
    EXPECT_EQ(f.line, 5u);
    EXPECT_EQ(e.name(f.file), "/src/low.h");
  }

  // Frame 0 lacks a call-site-independent position but has the line table; frame 1 has the call site of frame
  // 0 missing -> the next outer one (none) -> the table.
  r = e.provider->resolve(0x30084);
  ASSERT_EQ(r.inlines.size(), 2u);
  EXPECT_EQ(r.inlines[0].line, 5u);
  EXPECT_EQ(r.inlines[1].line, 5u);

  r = e.provider->resolve(0x30010);
  EXPECT_TRUE(r.resolved);
  EXPECT_TRUE(r.inlines.empty());
}

TEST(DwarfProviderLogic, NamesAreDemangledAndCachedPerFunction) {
  Env e(uint64_t{0}, [](FakeSession& s, uint64_t) {
    FakeFunction f;
    f.id = 1;
    f.entry = 0x40000;
    f.ranges = {{0x40000, 0x40100}};
    f.name = "_ZN2ns3Foo3barEi";
    f.inlines.push_back({{0x40010, 0x40020}, {{"_ZL6helperv", "", 0}}});
    s.functions.push_back(f);
  });
  ASSERT_TRUE(e.status.ok());
  for (int i = 0; i < 50; ++i) {
    const ResolvedAddress r = e.provider->resolve(0x40000 + static_cast<uint64_t>(i));
    EXPECT_EQ(e.name(r.function), "ns::Foo::bar(int)");
  }
  EXPECT_EQ(e.session.nameCalls, 1);  // name resolved once per function
  const ResolvedAddress in = e.provider->resolve(0x40014);
  ASSERT_EQ(in.inlines.size(), 1u);
  EXPECT_EQ(e.name(in.inlines[0].function), "helper()");
  EXPECT_EQ(e.provider->functionBounds(0x40000).value().size, 0x100u);
}

TEST(DwarfProviderLogic, UndemanglableLinkageNamesFallBackToTheSourceName) {
  Env e(uint64_t{0}, [](FakeSession& s, uint64_t) {
    FakeFunction f;
    f.id = 1;
    f.entry = 0x60000;
    f.ranges = {{0x60000, 0x60100}};
    f.name = "_ZN2ns3fooIiEEvT_Qrq4wild";  // not a valid mangling: __cxa_demangle fails
    f.source = "ns::foo";
    s.functions.push_back(f);
    FakeFunction g;
    g.id = 2;
    g.entry = 0x61000;
    g.ranges = {{0x61000, 0x61100}};
    g.name = "_Zbogus";  // undemanglable, and no source name either: the raw text is kept
    s.functions.push_back(g);
  });
  ASSERT_TRUE(e.status.ok());
  EXPECT_EQ(e.name(e.provider->resolve(0x60010).function), "ns::foo");
  EXPECT_EQ(e.name(e.provider->resolve(0x61010).function), "_Zbogus");
}

TEST(DwarfProviderLogic, FunctionsWithoutADwarfNameUseTheSymbolTableThenAHexName) {
  const auto file = fixture("dwarf_fixture_pie");
  const uint64_t hot = *symbolRva(file, "hot_c");
  Env withSymbols("dwarf_fixture_pie", [&](FakeSession& s, uint64_t) {
    FakeFunction f;
    f.id = 1;
    f.entry = hot;
    f.ranges = {{hot, hot + 8}};
    s.functions.push_back(f);  // no name in the DWARF: the symbol table names it
  });
  ASSERT_TRUE(withSymbols.status.ok());
  EXPECT_EQ(withSymbols.name(withSymbols.provider->resolve(hot).function), "hot_c");

  Env noSymbols(uint64_t{0}, [&](FakeSession& s, uint64_t) {
    FakeFunction g;
    g.id = 2;
    g.entry = 0x50000;
    g.ranges = {{0x50000, 0x50010}};
    s.functions.push_back(g);  // no name and no symbol: a hex placeholder
  });
  ASSERT_TRUE(noSymbols.status.ok());
  EXPECT_EQ(noSymbols.name(noSymbols.provider->resolve(0x50004).function), "0x50000");
}

TEST(DwarfProviderLogic, CodeOutsideDwarfFallsBackToTheSymbolTableAndKeepsLineInfoWhenThereIsSome) {
  const auto file = fixture("dwarf_fixture_pie");
  const uint64_t hot = *symbolRva(file, "hot_c");
  Env e("dwarf_fixture_pie", [&](FakeSession& s, uint64_t) {
    s.lines.push_back({{hot, hot + 4}, "/src/asm.S", 12});  // assembler code: lines but no subprogram DIE
  });
  ASSERT_TRUE(e.status.ok());
  const ResolvedAddress r = e.provider->resolve(hot + 1);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(e.name(r.function), "hot_c");
  EXPECT_EQ(r.functionStartRva, hot);
  EXPECT_EQ(e.name(r.file), "/src/asm.S");
  EXPECT_EQ(r.line, 12u);
  EXPECT_TRUE(r.inlines.empty());
  EXPECT_FALSE(e.provider->resolve(hot + 0x100000).resolved);
}

TEST(DwarfProviderLogic, DwarfWinsOverTheSymbolTable) {
  const auto file = fixture("dwarf_fixture_pie");
  const uint64_t hot = *symbolRva(file, "hot_c");
  Env e("dwarf_fixture_pie", [&](FakeSession& s, uint64_t) {
    FakeFunction f;
    f.id = 1;
    f.entry = hot;
    f.ranges = {{hot, hot + 0x10}};
    f.name = "dwarf_says_so";
    s.functions.push_back(f);
  });
  EXPECT_EQ(e.name(e.provider->resolve(hot + 2).function), "dwarf_says_so");
}

TEST(DwarfProviderLogic, LoadErrorsAreMapped) {
  SymbolStringTable strings;
  {  // Missing file.
    DwarfSymbolProvider p(strings, makeLibdwOpener());
    EXPECT_EQ(p.load(model::Module{}, "/no/such/file").error().code, ErrorCode::NotFound);
  }
  {  // Not an ELF file.
    const auto text = std::filesystem::temp_directory_path() / "tachyon-dwarf-notelf.txt";
    {
      std::ofstream out(text);
      out << "hello";
    }
    DwarfSymbolProvider p(strings, makeLibdwOpener());
    EXPECT_EQ(p.load(model::Module{}, text).error().code, ErrorCode::CorruptData);
    std::filesystem::remove(text);
  }
  {  // The opener reports damaged DWARF.
    FakeSession s;
    auto opener = std::make_shared<FakeOpener>(&s);
    opener->fail = true;
    DwarfSymbolProvider p(strings, opener);
    const auto file = fixture("dwarf_fixture_pie");
    EXPECT_EQ(p.load(moduleFor(file), file).error().code, ErrorCode::CorruptData);
    EXPECT_EQ(opener->opens, 1);
  }
  {  // Resolving before load is harmless.
    DwarfSymbolProvider p(strings, makeLibdwOpener());
    EXPECT_FALSE(p.resolve(0x1000).resolved);
    EXPECT_FALSE(p.functionBounds(0x1000).ok());
    EXPECT_EQ(p.linesForFunction(0x1000).error().code, ErrorCode::InvalidArgument);
  }
}

TEST(DwarfProviderLogic, BuildIdMismatchIsNotFound) {
  const auto file = fixture("dwarf_fixture_pie");
  SymbolStringTable strings;
  {
    DwarfSymbolProvider p(strings, makeLibdwOpener());
    model::Module m = moduleFor(file);
    ASSERT_EQ(m.id.kind, 2);
    m.id.bytes[3] ^= 0x55;
    const auto r = p.load(m, file);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_NE(r.error().message.find("build-id"), std::string::npos);
  }
  {  // A module without identity (or a non build-id kind) is accepted as is.
    DwarfSymbolProvider p(strings, makeLibdwOpener());
    model::Module m;
    EXPECT_TRUE(p.load(m, file).ok());
    DwarfSymbolProvider q(strings, makeLibdwOpener());
    model::Module pdb;
    pdb.id.kind = 1;
    EXPECT_TRUE(q.load(pdb, file).ok());
  }
}

TEST(DwarfProviderLogic, WithoutAnOpenerSymbolsStillComeFromTheSymbolTable) {
  const auto file = fixture("dwarf_fixture_pie");
  SymbolStringTable strings;
  DwarfSymbolProvider p(strings, nullptr);
  ASSERT_TRUE(p.load(moduleFor(file), file).ok());
  EXPECT_FALSE(p.hasDwarf());
  const ResolvedAddress r = p.resolve(*symbolRva(file, "hot_c"));
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(strings.get(r.function), "hot_c");
}

TEST(DwarfProviderLogic, ConcurrentResolvesAreSafe) {
  const auto file = fixture("dwarf_fixture_pie");
  Loaded l(file);
  ASSERT_TRUE(l.status.ok());
  const uint64_t hot = *symbolRva(file, "hot_c");
  const uint64_t inl = *symbolRva(file, "with_inlines");
  std::vector<std::thread> threads;
  std::atomic<int> bad{0};
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 2000; ++i) {
        const uint64_t rva =
            ((i + t) & 1) != 0 ? hot + static_cast<uint64_t>(i % 8) : inl + static_cast<uint64_t>(i % 40);
        const ResolvedAddress r = l.provider->resolve(rva);
        if (!r.resolved) ++bad;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(bad.load(), 0);
}

// registerDwarfProvider through the SymbolService: real fixture, build-id located symbols.
TEST(DwarfServiceIntegration, ResolvesFramesThroughTheService) {
  const auto file = fixture("dwarf_fixture_pie");
  const uint64_t hot = *symbolRva(file, "hot_c");
  const uint64_t widget = *symbolRva(file, "_ZN2fx6Widget4workEi");

  model::TraceModel m;
  m.modules.push_back(moduleFor(file));
  m.frames = {{ModuleIdx(0), hot + 4}, {ModuleIdx(0), widget + 1}, {ModuleIdx(0), 0x7fffff}};
  m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
  m.nodes.push_back(model::CallstackNode{CallstackIdx(0), FrameIdx(0)});
  m.nodes.push_back(model::CallstackNode{CallstackIdx(1), FrameIdx(1)});
  m.nodes.push_back(model::CallstackNode{CallstackIdx(2), FrameIdx(2)});

  SymbolService svc({}, {});
  svc.setLocate([&](const Module&) -> Result<std::filesystem::path> { return file; });
  registerDwarfProvider(svc);
  svc.resolveAllAsync(m);
  svc.waitAll(m);
  ASSERT_EQ(m.modules[0].state, model::SymbolState::Resolved) << svc.moduleError(ModuleIdx(0));

  const ResolvedAddress& leaf = svc.lookup(FrameIdx(0));
  ASSERT_TRUE(leaf.resolved);
  EXPECT_EQ(svc.symbolStrings().get(leaf.function), "hot_c");
  EXPECT_EQ(leaf.functionStartRva, hot);
  EXPECT_NE(leaf.line, 0u);
  const ResolvedAddress& caller = svc.lookupCaller(FrameIdx(1));  // non-leaf: rva - 1
  ASSERT_TRUE(caller.resolved);
  EXPECT_EQ(svc.symbolStrings().get(caller.function), "fx::Widget::work(int)");
  EXPECT_FALSE(svc.lookup(FrameIdx(2)).resolved);
  ASSERT_NE(svc.provider(ModuleIdx(0)), nullptr);
  EXPECT_TRUE(svc.provider(ModuleIdx(0))->functionBounds(hot).ok());
}

TEST(DwarfServiceIntegration, WrongBuildIdIsNotFoundAndJsonStillWorks) {
  const auto file = fixture("dwarf_fixture_pie");
  model::TraceModel m;
  m.modules.push_back(moduleFor(file));
  m.modules[0].id.bytes[0] ^= 0xFF;
  m.frames = {{ModuleIdx(0), 0x1000}};
  m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
  m.nodes.push_back(model::CallstackNode{CallstackIdx(0), FrameIdx(0)});
  SymbolService svc({}, {});
  svc.setLocate([&](const Module&) -> Result<std::filesystem::path> { return file; });
  registerDwarfProvider(svc);
  svc.resolveAllAsync(m);
  svc.waitAll(m);
  EXPECT_EQ(m.modules[0].state, model::SymbolState::NotFound);

  // A file that is neither ELF nor JSON gets no provider (Failed).
  const auto junk = std::filesystem::temp_directory_path() / "tachyon-dwarf-junk.bin";
  {
    std::ofstream out(junk);
    out << "junk";
  }
  SymbolService svc2({}, {});
  svc2.setLocate([&](const Module&) -> Result<std::filesystem::path> { return junk; });
  registerDwarfProvider(svc2);
  m.modules[0].id.bytes[0] ^= 0xFF;
  svc2.resolveAllAsync(m);
  svc2.waitAll(m);
  EXPECT_EQ(m.modules[0].state, model::SymbolState::Failed);
  std::filesystem::remove(junk);
}

}  // namespace
}  // namespace tachyon::symbols
