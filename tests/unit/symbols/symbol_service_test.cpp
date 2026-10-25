#include "symbols/symbol_service.h"
#include "symbols/function_key.h"
#include "symbols_test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

namespace tachyon::symbols {
namespace {

using testutil::loadModel;
using testutil::Reference;
using testutil::ReferenceSymbols;

struct Resolved {
  model::TraceModel model;
  std::unique_ptr<SymbolService> service;
};

Resolved resolveFixture(const std::string& name, unsigned workers = 0) {
  Resolved r;
  r.model = loadModel(name);
  r.service = std::make_unique<SymbolService>(std::vector<std::string>{}, std::filesystem::path{});
  r.service->setWorkerThreads(workers);
  auto added = r.service->addSymbolFileOverride(testutil::fixtureFile(name, ".symbols.json"));
  EXPECT_TRUE(added.ok()) << (added.ok() ? "" : added.error().toString());
  r.service->resolveAllAsync(r.model);
  r.service->waitAll();
  return r;
}

// Every frame of the fixture against a brute-force scan of symbols.json, and function names against
// expected.json. Returns the number of frames that resolved.
void checkAllFrames(const std::string& name, size_t& resolvedOut) {
  Resolved r = resolveFixture(name);
  const ReferenceSymbols ref(testutil::fixtureFile(name, ".symbols.json"));
  const SymbolService& svc = *r.service;
  size_t resolved = 0, unmapped = 0, gaps = 0;
  for (uint32_t f = 0; f < r.model.frames.size(); ++f) {
    const model::Frame& frame = r.model.frames[f];
    const ResolvedAddress& got = svc.lookup(FrameIdx(f));
    if (!frame.module.valid()) {
      EXPECT_FALSE(got.resolved);
      ++unmapped;
      continue;
    }
    const Reference want = ref.lookup(frame.module.get(), frame.addr_or_rva);
    ASSERT_EQ(got.resolved, want.resolved) << "frame " << f;
    if (!want.resolved) {
      ++gaps;
      continue;
    }
    ++resolved;
    EXPECT_EQ(svc.symbolStrings().get(got.function), want.function) << "frame " << f;
    EXPECT_EQ(got.functionStartRva, want.startRva) << "frame " << f;
    EXPECT_EQ(svc.symbolStrings().get(got.file), want.file) << "frame " << f;
    EXPECT_EQ(got.line, want.line) << "frame " << f;
    ASSERT_EQ(got.inlines.size(), want.inlineFunctions.size()) << "frame " << f;
    for (size_t i = 0; i < got.inlines.size(); ++i) {
      EXPECT_EQ(svc.symbolStrings().get(got.inlines[i].function), want.inlineFunctions[i]);
    }
  }

  // Cross-check against expected.json: every sampled function must be reachable from some frame, with the
  // same "<module>!<function>" name.
  const nlohmann::json expected = testutil::readJson(testutil::fixtureFile(name, ".expected.json"));
  std::set<std::pair<uint32_t, uint64_t>> seen;
  for (uint32_t f = 0; f < r.model.frames.size(); ++f) {
    const FunctionKey k = functionKeyFor(svc, FrameIdx(f));
    seen.insert({k.module.get(), k.value});
  }
  size_t checkedNames = 0;
  for (const auto& fn : expected["functions"]) {
    const uint32_t module = fn["module"].get<uint32_t>();
    const uint64_t start = fn["start_rva"].get<uint64_t>();
    EXPECT_TRUE(seen.count({module, start})) << fn["name"];
    if (fn["kind"] != "function") continue;
    // Find a frame in that function and compare names.
    for (uint32_t f = 0; f < r.model.frames.size(); ++f) {
      const ResolvedAddress& got = svc.lookup(FrameIdx(f));
      if (r.model.frames[f].module.get() == module && got.resolved && got.functionStartRva == start) {
        const std::string modName = expected["modules"][module]["name"].get<std::string>();
        EXPECT_EQ(modName + "!" + std::string(svc.symbolStrings().get(got.function)),
                  fn["name"].get<std::string>());
        ++checkedNames;
        break;
      }
    }
  }
  EXPECT_GT(checkedNames, 0u);
  std::printf("[symbols] %s: %zu frames, %zu resolved, %zu gaps, %zu unmapped, %zu names cross-checked\n",
              name.c_str(), r.model.frames.size(), resolved, gaps, unmapped, checkedNames);
  resolvedOut = resolved;
}

size_t checkAllFrames(const std::string& name) {
  size_t n = 0;
  checkAllFrames(name, n);
  return n;
}

TEST(SymbolServiceTest, ResolvesAllFramesOfTiny) {
  EXPECT_GT(checkAllFrames("tiny"), 0u);
}

TEST(SymbolServiceTest, ResolvesAllFramesOfMedium) {
  EXPECT_GT(checkAllFrames("medium"), 100u);
}

TEST(SymbolServiceTest, ResolvesRecursionAndScopesFixtures) {
  EXPECT_GT(checkAllFrames("recursion"), 0u);
  EXPECT_GT(checkAllFrames("scopes"), 0u);
}

TEST(SymbolServiceTest, UnmappedFramesAreUnresolved) {
  Resolved r = resolveFixture("tiny");
  size_t unmapped = 0;
  for (uint32_t f = 0; f < r.model.frames.size(); ++f) {
    if (r.model.frames[f].module.valid()) continue;
    ++unmapped;
    EXPECT_FALSE(r.service->lookup(FrameIdx(f)).resolved);
  }
  EXPECT_GT(unmapped, 0u) << "tiny is expected to contain an unmapped frame";
}

TEST(SymbolServiceTest, ResolvesFramesWithInlineChains) {
  // medium has generated modules with inlines.
  Resolved r = resolveFixture("medium");
  size_t withInlines = 0;
  for (uint32_t f = 0; f < r.model.frames.size(); ++f) {
    const ResolvedAddress& a = r.service->lookup(FrameIdx(f));
    if (a.inlines.empty()) continue;
    ++withInlines;
    for (const InlineFrame& in : a.inlines) {
      EXPECT_FALSE(r.service->symbolStrings().get(in.function).empty());
      EXPECT_FALSE(r.service->symbolStrings().get(in.file).empty());
    }
  }
  std::printf("[symbols] medium: %zu frames inside an inline range\n", withInlines);
}

TEST(SymbolServiceTest, UnknownModuleIsNotResolved) {
  model::TraceModel m = loadModel("tiny");
  ASSERT_FALSE(m.modules.empty());
  SymbolService svc({}, {});  // no overrides: the locate stub returns NotFound
  std::vector<std::pair<uint32_t, SymbolState>> events;
  std::mutex mu;
  svc.resolveAllAsync(m, [&](ModuleIdx idx, SymbolState s) {
    std::lock_guard<std::mutex> lock(mu);
    events.emplace_back(idx.get(), s);
  });
  svc.waitAll();
  for (uint32_t f = 0; f < m.frames.size(); ++f) EXPECT_FALSE(svc.lookup(FrameIdx(f)).resolved);
  for (uint32_t mi = 0; mi < m.modules.size(); ++mi) {
    if (svc.moduleState(ModuleIdx(mi)) == SymbolState::Pending) continue;  // unreferenced
    EXPECT_EQ(svc.moduleState(ModuleIdx(mi)), SymbolState::NotFound);
    EXPECT_EQ(m.modules[mi].state, SymbolState::NotFound);
    EXPECT_FALSE(svc.moduleError(ModuleIdx(mi)).empty());
  }
  const SymbolProgress p = svc.progress();
  EXPECT_EQ(p.done, p.total);
  EXPECT_GT(p.total, 0u);
}

TEST(SymbolServiceTest, ModuleWithoutMatchingEntryIsNotFoundAndOthersStillResolve) {
  // Override from tiny used on medium: ids differ, paths differ for some modules.
  model::TraceModel m = loadModel("medium");
  SymbolService svc({}, {});
  ASSERT_TRUE(svc.addSymbolFileOverride(testutil::fixtureFile("tiny", ".symbols.json")).ok());
  svc.resolveAllAsync(m);
  svc.waitAll();
  for (uint32_t i = 0; i < m.modules.size(); ++i) {
    // Ids are seed-derived and differ between scenarios; when both sides carry ids a mismatch rejects the
    // module even if the paths agree.
    const SymbolState s = svc.moduleState(ModuleIdx(i));
    EXPECT_TRUE(s == SymbolState::NotFound || s == SymbolState::Pending) << i;
  }
}

TEST(SymbolServiceTest, ProviderFailureIsReported) {
  model::TraceModel m = loadModel("tiny");
  SymbolService svc({}, {});
  svc.setLocate(
      [](const Module&) -> Result<std::filesystem::path> { return std::filesystem::path("x.unknown"); });
  svc.resolveAllAsync(m);
  svc.waitAll();
  EXPECT_EQ(svc.moduleState(ModuleIdx(0)), SymbolState::Failed);
  EXPECT_EQ(m.modules[0].state, SymbolState::Failed);

  SymbolService svc2({}, {});
  svc2.setLocate([](const Module&) -> Result<std::filesystem::path> {
    return testutil::fixtureFile("does_not_exist", ".symbols.json");
  });
  model::TraceModel m2 = loadModel("tiny");
  svc2.resolveAllAsync(m2);
  svc2.waitAll();
  EXPECT_EQ(svc2.moduleState(ModuleIdx(0)), SymbolState::NotFound);
}

TEST(SymbolServiceTest, ReportsStateTransitionsAndProgress) {
  model::TraceModel m = loadModel("medium");
  SymbolService svc({}, {});
  svc.setWorkerThreads(2);
  ASSERT_TRUE(svc.addSymbolFileOverride(testutil::fixtureFile("medium", ".symbols.json")).ok());
  std::mutex mu;
  std::vector<std::vector<SymbolState>> seen(m.modules.size());
  svc.resolveAllAsync(m, [&](ModuleIdx idx, SymbolState s) {
    std::lock_guard<std::mutex> lock(mu);
    seen[idx.get()].push_back(s);
  });
  svc.waitAll();
  const SymbolProgress p = svc.progress();
  EXPECT_EQ(p.done, p.total);
  EXPECT_EQ(p.total, m.modules.size());
  for (size_t i = 0; i < m.modules.size(); ++i) {
    ASSERT_EQ(seen[i].size(), 2u) << i;
    EXPECT_EQ(seen[i][0], SymbolState::Loading);
    EXPECT_EQ(seen[i][1], SymbolState::Resolved);
    EXPECT_EQ(m.modules[i].state, SymbolState::Resolved);
    EXPECT_NE(svc.provider(ModuleIdx(static_cast<uint32_t>(i))), nullptr);
  }
}

TEST(SymbolServiceTest, LookupBeforeResolveIsUnresolvedAndOutOfRangeIsSafe) {
  SymbolService svc({}, {});
  EXPECT_FALSE(svc.lookup(FrameIdx(0)).resolved);
  EXPECT_FALSE(svc.lookup(FrameIdx::invalid()).resolved);
  Resolved r = resolveFixture("tiny");
  EXPECT_FALSE(r.service->lookup(FrameIdx(0xFFFFFF)).resolved);
  EXPECT_FALSE(r.service->lookup(FrameIdx::invalid()).resolved);
}

TEST(SymbolServiceTest, CanResolveAgainOnANewModel) {
  Resolved r = resolveFixture("tiny");
  model::TraceModel again = loadModel("tiny");
  r.service->resolveAllAsync(again);
  r.service->waitAll();
  size_t resolved = 0;
  for (uint32_t f = 0; f < again.frames.size(); ++f) resolved += r.service->lookup(FrameIdx(f)).resolved;
  EXPECT_GT(resolved, 0u);
}

// Wraps a provider and slows load() so lookups overlap with loading; also interns extra strings from the
// worker threads while readers hammer symbolStrings().
class SlowProvider final : public ISymbolProvider {
 public:
  SlowProvider(std::unique_ptr<ISymbolProvider> inner, SymbolStringTable& strings, int delayMs)
      : m_inner(std::move(inner)), m_strings(strings), m_delayMs(delayMs) {}
  Result<void> load(const Module& m, const std::filesystem::path& f) override {
    for (int i = 0; i < 50; ++i) (void)m_strings.intern("noise-" + std::to_string(i) + m.path);
    std::this_thread::sleep_for(std::chrono::milliseconds(m_delayMs));
    return m_inner->load(m, f);
  }
  ResolvedAddress resolve(uint64_t rva) override { return m_inner->resolve(rva); }
  Result<std::vector<LineRange>> linesForFunction(uint64_t s) override {
    return m_inner->linesForFunction(s);
  }
  Result<FunctionBounds> functionBounds(uint64_t rva) override { return m_inner->functionBounds(rva); }

 private:
  std::unique_ptr<ISymbolProvider> m_inner;
  SymbolStringTable& m_strings;
  int m_delayMs;
};

TEST(SymbolServiceTest, EightThreadsLookUpWhileModulesLoad) {
  model::TraceModel m = loadModel("medium");
  SymbolService svc({}, {});
  svc.setWorkerThreads(2);
  ASSERT_TRUE(svc.addSymbolFileOverride(testutil::fixtureFile("medium", ".symbols.json")).ok());
  svc.setProviderFactory(
      [&svc](const Module&, const std::filesystem::path&) -> std::unique_ptr<ISymbolProvider> {
        return std::make_unique<SlowProvider>(std::make_unique<JsonSymbolProvider>(svc.stringSink()),
                                              svc.stringSink(), 40);
      });

  constexpr int kReaders = 8;
  std::atomic<bool> go{false}, stop{false};
  std::atomic<size_t> unresolvedSeen{0}, resolvedSeen{0}, violations{0};
  const uint32_t frames = static_cast<uint32_t>(m.frames.size());
  ASSERT_GT(frames, 0u);

  std::vector<std::thread> readers;
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&, t] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      std::vector<const ResolvedAddress*> first(frames, nullptr);
      for (;;) {
        const bool last = stop.load(std::memory_order_acquire);  // one full pass after stop is requested
        for (uint32_t i = 0; i < frames; ++i) {
          const uint32_t f = (i + static_cast<uint32_t>(t) * 97u) % frames;
          const ResolvedAddress& a = svc.lookup(FrameIdx(f));
          if (&a == &unresolvedAddress()) {
            ++unresolvedSeen;
            continue;
          }
          ++resolvedSeen;
          // A published result is complete and never changes.
          if (a.resolved && svc.symbolStrings().get(a.function).empty()) ++violations;
          if (first[f] == nullptr)
            first[f] = &a;
          else if (first[f] != &a)
            ++violations;
        }
        (void)svc.symbolStrings().size();
        (void)svc.moduleState(ModuleIdx(0));
        (void)svc.progress();
        if (last) break;
      }
    });
  }

  svc.resolveAllAsync(m);
  go.store(true, std::memory_order_release);
  svc.waitAll();
  stop.store(true, std::memory_order_release);
  for (std::thread& th : readers) th.join();

  EXPECT_EQ(violations.load(), 0u);
  EXPECT_GT(unresolvedSeen.load(), 0u) << "readers should have observed the not-yet-loaded state";
  EXPECT_GT(resolvedSeen.load(), 0u);

  // Final answers match the brute-force reference.
  const ReferenceSymbols ref(testutil::fixtureFile("medium", ".symbols.json"));
  for (uint32_t f = 0; f < frames; ++f) {
    if (!m.frames[f].module.valid()) continue;
    const Reference want = ref.lookup(m.frames[f].module.get(), m.frames[f].addr_or_rva);
    const ResolvedAddress& got = svc.lookup(FrameIdx(f));
    ASSERT_EQ(got.resolved, want.resolved);
    if (want.resolved) EXPECT_EQ(svc.symbolStrings().get(got.function), want.function);
  }
}

}  // namespace
}  // namespace tachyon::symbols
