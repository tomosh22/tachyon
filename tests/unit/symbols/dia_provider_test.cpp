// Platform-neutral part of the DIA provider (WP-22): provider logic over a fake session, error mapping,
// PDB identity conversion and the SymbolService factory. The msdia140-backed session is tested on Windows
// (tests/windows/symbols/dia_provider_test.cpp).
#include "symbols/dia/dia_provider.h"
#include "core/platform.h"
#include "symbols/dia/dia_errors.h"
#include "symbols/dia/msvc_undecorate.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace tachyon::symbols {
namespace {

struct FakeFunction {
  DiaRawFunction fn;
  std::vector<DiaRawLine> lines;
  struct InlineSite {
    uint64_t rva;
    uint64_t size;
    std::vector<DiaRawInline> chain;
  };
  std::vector<InlineSite> inlines;
};

// Shared with the opener so tests can observe call counts after the session moved into the provider.
struct FakeState {
  std::vector<FakeFunction> functions;
  std::atomic<int> findFunctionCalls{0};
  std::atomic<int> concurrent{0};
  std::atomic<int> maxConcurrent{0};
};

class FakeSession final : public IDiaSymbolSession {
 public:
  explicit FakeSession(std::shared_ptr<FakeState> s) : m_s(std::move(s)) {}

  std::optional<DiaRawFunction> findFunction(uint64_t rva) override {
    Guard g(*m_s);
    ++m_s->findFunctionCalls;
    const FakeFunction* best = nullptr;
    for (const FakeFunction& f : m_s->functions) {
      if (f.fn.startRva <= rva && (best == nullptr || f.fn.startRva > best->fn.startRva)) best = &f;
    }
    if (best == nullptr) return std::nullopt;
    if (!best->fn.isPublicSymbol && rva - best->fn.startRva >= best->fn.size) return std::nullopt;
    return best->fn;
  }
  std::optional<DiaRawLine> findLine(uint64_t rva) override {
    Guard g(*m_s);
    for (const FakeFunction& f : m_s->functions) {
      for (const DiaRawLine& l : f.lines) {
        if (rva >= l.rva && rva - l.rva < l.size) return l;
      }
    }
    return std::nullopt;
  }
  std::vector<DiaRawInline> findInlines(const DiaRawFunction& function, uint64_t rva) override {
    Guard g(*m_s);
    for (const FakeFunction& f : m_s->functions) {
      if (f.fn.startRva != function.startRva) continue;
      for (const auto& site : f.inlines) {
        if (rva >= site.rva && rva - site.rva < site.size) return site.chain;
      }
    }
    return {};
  }
  Result<std::vector<DiaRawLine>> findLines(uint64_t rva, uint64_t size) override {
    Guard g(*m_s);
    for (const FakeFunction& f : m_s->functions) {
      if (f.fn.startRva == rva && f.fn.size == size) return f.lines;
    }
    return std::vector<DiaRawLine>{};
  }

 private:
  // Detects overlapping calls (the provider must serialize them).
  struct Guard {
    explicit Guard(FakeState& s) : st(s) {
      const int now = ++st.concurrent;
      int prev = st.maxConcurrent.load();
      while (now > prev && !st.maxConcurrent.compare_exchange_weak(prev, now)) {
      }
      std::this_thread::yield();
    }
    ~Guard() { --st.concurrent; }
    FakeState& st;
  };
  std::shared_ptr<FakeState> m_s;
};

class FakeOpener final : public IDiaSessionOpener {
 public:
  explicit FakeOpener(std::shared_ptr<FakeState> s) : m_s(std::move(s)) {}
  Result<std::unique_ptr<IDiaSymbolSession>> open(const model::Module& module,
                                                  const std::filesystem::path& file) override {
    lastModule = module;
    lastFile = file;
    if (failWith) return *failWith;
    return std::unique_ptr<IDiaSymbolSession>(std::make_unique<FakeSession>(m_s));
  }
  std::optional<Error> failWith;
  model::Module lastModule;
  std::filesystem::path lastFile;

 private:
  std::shared_ptr<FakeState> m_s;
};

std::shared_ptr<FakeState> makeState() {
  auto s = std::make_shared<FakeState>();
  FakeFunction a;
  a.fn = {0x1000, 0x80, "hot_a", false};
  a.lines = {{0x1000, 0x10, "C:\\src\\a.cpp", 10},
             {0x1010, 0x30, "C:\\src\\a.cpp", 11},
             {0x1040, 0x40, "C:\\src\\b.h", 5}};
  a.inlines = {{0x1040, 0x40, {{"inl_inner", "C:\\src\\b.h", 5}, {"inl_outer", "C:\\src\\a.cpp", 12}}}};
  FakeFunction b;
  b.fn = {0x2000, 0x20, "?bar@Foo@ns@@QEAAXXZ", false};  // decorated: goes through the name function
  b.lines = {{0x2000, 0x20, "C:\\src\\a.cpp", 40}};
  FakeFunction pub;
  pub.fn = {0x3000, 0, "pub_only", true};
  s->functions = {a, b, pub};
  return s;
}

model::Module pdbModule() {
  model::Module m;
  m.path = "C:\\bin\\app.exe";
  m.id.kind = trace::kModuleIdPdb;
  m.id.len = 16;
  for (uint8_t i = 0; i < 16; ++i) m.id.bytes[i] = static_cast<uint8_t>(0xA0 + i);
  m.id.age = 7;
  return m;
}

struct Fixture {
  SymbolStringTable strings;
  std::shared_ptr<FakeState> state = makeState();
  std::shared_ptr<FakeOpener> opener = std::make_shared<FakeOpener>(state);
  DiaSymbolProvider provider{strings, opener, [](std::string_view n) {
                               return n == "?bar@Foo@ns@@QEAAXXZ" ? std::string("ns::Foo::bar")
                                                                  : std::string(n);
                             }};
};

TEST(DiaProvider, LoadPassesModuleAndFileToTheOpener) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x/app.pdb").ok());
  EXPECT_EQ(f.opener->lastFile, std::filesystem::path("x/app.pdb"));
  EXPECT_EQ(f.opener->lastModule.id, pdbModule().id);
}

TEST(DiaProvider, LoadErrorsPropagateWithTheirCode) {
  Fixture f;
  f.opener->failWith = mapDiaLoadError(dia_hr::kEPdbInvalidSig, "x.pdb");
  auto r = f.provider.load(pdbModule(), "x.pdb");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
  EXPECT_EQ(f.provider.resolve(0x1000).resolved, false);  // not loaded
  EXPECT_FALSE(f.provider.functionBounds(0x1000).ok());
  EXPECT_FALSE(f.provider.linesForFunction(0x1000).ok());
}

TEST(DiaProvider, NoOpenerIsUnsupported) {
  SymbolStringTable strings;
  DiaSymbolProvider p(strings, nullptr);
  auto r = p.load(pdbModule(), "x.pdb");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}

TEST(DiaProvider, ResolvesFunctionFileLineAndInlines) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());

  ResolvedAddress r = f.provider.resolve(0x1012);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(f.strings.get(r.function), "hot_a");
  EXPECT_EQ(r.functionStartRva, 0x1000u);
  EXPECT_EQ(f.strings.get(r.file), "C:\\src\\a.cpp");
  EXPECT_EQ(r.line, 11u);
  EXPECT_TRUE(r.inlines.empty());

  r = f.provider.resolve(0x1050);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(f.strings.get(r.function), "hot_a");  // physical function, not the inlinee
  EXPECT_EQ(r.line, 5u);
  ASSERT_EQ(r.inlines.size(), 2u);
  EXPECT_EQ(f.strings.get(r.inlines[0].function), "inl_inner");  // innermost first
  EXPECT_EQ(f.strings.get(r.inlines[1].function), "inl_outer");
  EXPECT_EQ(r.inlines[1].line, 12u);
  EXPECT_EQ(f.strings.get(r.inlines[1].file), "C:\\src\\a.cpp");
}

TEST(DiaProvider, DecoratedNamesGoThroughTheNameFunction) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());
  const ResolvedAddress r = f.provider.resolve(0x2004);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(f.strings.get(r.function), "ns::Foo::bar");
}

TEST(DiaProvider, PublicSymbolsResolveWithoutLinesAndAreNotRangeCached) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());
  const ResolvedAddress r = f.provider.resolve(0x3010);
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(f.strings.get(r.function), "pub_only");
  EXPECT_EQ(r.functionStartRva, 0x3000u);
  EXPECT_FALSE(r.file.valid());
  EXPECT_EQ(r.line, 0u);
  const int before = f.state->findFunctionCalls;
  f.provider.resolve(0x3010);
  EXPECT_EQ(f.state->findFunctionCalls, before + 1);  // extent unknown: asked again
}

TEST(DiaProvider, GapsAndOutOfRangeAreUnresolved) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());
  EXPECT_FALSE(f.provider.resolve(0x500).resolved);
  EXPECT_FALSE(f.provider.resolve(0x1080).resolved);  // one past hot_a, before the next function
  EXPECT_FALSE(f.provider.functionBounds(0x1080).ok());
  EXPECT_EQ(f.provider.functionBounds(0x1080).error().code, ErrorCode::NotFound);
}

TEST(DiaProvider, FunctionExtentsAreCached) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());
  for (uint64_t rva = 0x1000; rva < 0x1080; ++rva) ASSERT_TRUE(f.provider.resolve(rva).resolved);
  EXPECT_EQ(f.provider.functionLookups(), 1u);
  EXPECT_EQ(f.state->findFunctionCalls, 1);
  ASSERT_TRUE(f.provider.resolve(0x2000).resolved);
  EXPECT_EQ(f.provider.functionLookups(), 2u);
}

TEST(DiaProvider, FunctionBoundsAndLines) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());
  auto b = f.provider.functionBounds(0x1033);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value().startRva, 0x1000u);
  EXPECT_EQ(b.value().size, 0x80u);
  EXPECT_EQ(f.strings.get(b.value().name), "hot_a");

  auto lines = f.provider.linesForFunction(0x1000);
  ASSERT_TRUE(lines.ok());
  ASSERT_EQ(lines.value().size(), 3u);
  EXPECT_EQ(lines.value()[0].rva, 0x1000u);
  EXPECT_EQ(lines.value()[1].line, 11u);
  EXPECT_EQ(f.strings.get(lines.value()[2].file), "C:\\src\\b.h");

  EXPECT_EQ(f.provider.linesForFunction(0x1004).error().code, ErrorCode::NotFound);  // not a start
  EXPECT_EQ(f.provider.linesForFunction(0x9000).error().code, ErrorCode::NotFound);
  auto pubLines = f.provider.linesForFunction(0x3000);
  ASSERT_TRUE(pubLines.ok());
  EXPECT_TRUE(pubLines.value().empty());
}

TEST(DiaProvider, LinesAreSortedByRva) {
  Fixture f;
  std::reverse(f.state->functions[0].lines.begin(), f.state->functions[0].lines.end());
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());
  auto lines = f.provider.linesForFunction(0x1000);
  ASSERT_TRUE(lines.ok());
  for (size_t i = 1; i < lines.value().size(); ++i) {
    EXPECT_LT(lines.value()[i - 1].rva, lines.value()[i].rva);
  }
}

TEST(DiaProvider, ConcurrentResolveIsSerializedAndConsistent) {
  Fixture f;
  ASSERT_TRUE(f.provider.load(pdbModule(), "x.pdb").ok());
  std::vector<std::thread> threads;
  std::atomic<int> bad{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 2000; ++i) {
        const uint64_t rva = 0x1000 + static_cast<uint64_t>(i % 0x80);
        const ResolvedAddress r = f.provider.resolve(rva);
        if (!r.resolved || f.strings.get(r.function) != "hot_a") ++bad;
        if (!f.provider.functionBounds(rva).ok()) ++bad;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(bad, 0);
  EXPECT_EQ(f.state->maxConcurrent, 1);
}

TEST(DiaErrors, MapsMissingAndMismatchingPdbsToNotFound) {
  using namespace dia_hr;
  for (const uint32_t hr : {kEPdbNotFound, kFileNotFound, kPathNotFound, kEPdbInvalidSig, kEPdbInvalidAge,
                            kEPdbInvalidExeTimestamp, kEPdbNoDebugInfo}) {
    EXPECT_EQ(mapDiaLoadError(hr, "a.pdb").code, ErrorCode::NotFound) << std::hex << hr;
  }
  EXPECT_NE(mapDiaLoadError(kEPdbInvalidSig, "a.pdb").message.find("GUID"), std::string::npos);
  EXPECT_NE(mapDiaLoadError(kEPdbInvalidAge, "a.pdb").message.find("age"), std::string::npos);
  const Error e = mapDiaLoadError(kEPdbNotFound, "dir/a.pdb");
  EXPECT_NE(e.message.find("a.pdb"), std::string::npos);
  EXPECT_NE(e.message.find("0x806D0005"), std::string::npos);
}

TEST(DiaErrors, MapsOtherFailures) {
  using namespace dia_hr;
  EXPECT_EQ(mapDiaLoadError(kEPdbFormat, "a.pdb").code, ErrorCode::CorruptData);
  EXPECT_EQ(mapDiaLoadError(kEPdbCorrupt, "a.pdb").code, ErrorCode::CorruptData);
  EXPECT_EQ(mapDiaLoadError(kEPdbV1Pdb, "a.pdb").code, ErrorCode::CorruptData);
  EXPECT_EQ(mapDiaLoadError(kEPdbAccessDenied, "a.pdb").code, ErrorCode::IoError);
  EXPECT_EQ(mapDiaLoadError(kEAccessDenied, "a.pdb").code, ErrorCode::IoError);
  EXPECT_EQ(mapDiaLoadError(kEPdbOutOfMemory, "a.pdb").code, ErrorCode::OutOfMemory);
  EXPECT_EQ(mapDiaLoadError(kEPdbUsage, "a.pdb").code, ErrorCode::InvalidArgument);
  EXPECT_EQ(mapDiaLoadError(kClassNotRegistered, "a.pdb").code, ErrorCode::Unsupported);
  EXPECT_EQ(mapDiaLoadError(0x80004005u, "a.pdb").code, ErrorCode::Internal);
}

TEST(DiaErrors, IdentityConversion) {
  const auto id = diaIdentityFor(pdbModule().id);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->age, 7u);
  EXPECT_EQ(id->guid[0], 0xA0);
  EXPECT_EQ(id->guid[15], 0xAF);

  model::ModuleId none;
  EXPECT_FALSE(diaIdentityFor(none).has_value());
  model::ModuleId gnu = pdbModule().id;
  gnu.kind = trace::kModuleIdGnuBuildId;
  EXPECT_FALSE(diaIdentityFor(gnu).has_value());
  model::ModuleId shortId = pdbModule().id;
  shortId.len = 8;
  EXPECT_FALSE(diaIdentityFor(shortId).has_value());
}

TEST(DiaUndecorate, NonMangledNamesPassThrough) {
  EXPECT_EQ(undecorateMsvc("hot_a"), "hot_a");
  EXPECT_EQ(undecorateMsvc(""), "");
  EXPECT_EQ(undecorateMsvc("_ZN3Foo3barEv"), "_ZN3Foo3barEv");
#if !TACHYON_PLATFORM_WINDOWS
  EXPECT_EQ(undecorateMsvc("?bar@Foo@@QEAAXXZ"), "?bar@Foo@@QEAAXXZ");
#endif
}

TEST(DiaFactory, RegistersPdbAndJsonProviders) {
  // The factory is exercised through the service end to end: a module whose "located" file is a .pdb gets a
  // DiaSymbolProvider over the fake opener; frames then resolve through it.
  auto state = makeState();
  auto opener = std::make_shared<FakeOpener>(state);

  model::TraceModel m;
  model::Module mod = pdbModule();
  mod.pid = 1;
  m.modules.push_back(mod);
  m.frames.push_back({ModuleIdx(0), 0x1012});
  m.frames.push_back({ModuleIdx(0), 0x2004});
  m.frames.push_back({ModuleIdx(0), 0x500});

  SymbolService svc({}, {});
  registerDiaProvider(svc, opener);
  svc.setLocate(
      [](const Module&) -> Result<std::filesystem::path> { return std::filesystem::path("fake/app.pdb"); });
  svc.setWorkerThreads(2);
  svc.resolveAllAsync(m);
  svc.waitAll();

  EXPECT_EQ(m.modules[0].state, SymbolState::Resolved) << svc.moduleError(ModuleIdx(0));
  const ResolvedAddress& a = svc.lookup(FrameIdx(0));
  ASSERT_TRUE(a.resolved);
  EXPECT_EQ(svc.symbolStrings().get(a.function), "hot_a");
  EXPECT_EQ(a.line, 11u);
  // The default name function undecorates only on Windows; elsewhere the decorated name is kept.
  EXPECT_TRUE(svc.lookup(FrameIdx(1)).resolved);
  EXPECT_FALSE(svc.lookup(FrameIdx(2)).resolved);
  ASSERT_NE(svc.provider(ModuleIdx(0)), nullptr);
  EXPECT_TRUE(svc.provider(ModuleIdx(0))->functionBounds(0x1000).ok());
}

TEST(DiaFactory, MissingPdbMapsToNotFoundState) {
  auto state = makeState();
  auto opener = std::make_shared<FakeOpener>(state);
  opener->failWith = mapDiaLoadError(dia_hr::kEPdbInvalidAge, "fake/app.pdb");

  model::TraceModel m;
  m.modules.push_back(pdbModule());
  m.frames.push_back({ModuleIdx(0), 0x1000});
  SymbolService svc({}, {});
  registerDiaProvider(svc, opener);
  svc.setLocate(
      [](const Module&) -> Result<std::filesystem::path> { return std::filesystem::path("fake/app.pdb"); });
  svc.resolveAllAsync(m);
  svc.waitAll();
  EXPECT_EQ(m.modules[0].state, SymbolState::NotFound);
  EXPECT_FALSE(svc.lookup(FrameIdx(0)).resolved);
}

TEST(DiaFactory, WithoutDiaOpenerPdbModulesFail) {
  // On non-Windows builds makeComDiaOpener() is null, so a located .pdb has no provider -> Failed.
  if (makeComDiaOpener() != nullptr) GTEST_SKIP() << "real DIA available";
  model::TraceModel m;
  m.modules.push_back(pdbModule());
  m.frames.push_back({ModuleIdx(0), 0x1000});
  SymbolService svc({}, {});
  registerDiaProvider(svc);
  svc.setLocate(
      [](const Module&) -> Result<std::filesystem::path> { return std::filesystem::path("fake/app.pdb"); });
  svc.resolveAllAsync(m);
  svc.waitAll();
  EXPECT_EQ(m.modules[0].state, SymbolState::Failed);
}

}  // namespace
}  // namespace tachyon::symbols
