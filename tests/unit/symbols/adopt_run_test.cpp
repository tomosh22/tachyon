// WP-68: SymbolService::adoptRun. The service of the complete document takes over what the preview's
// service finished: modules with an identical record keep their provider and are not located or loaded again
// (resolution count 0), unfinished and changed modules are resolved normally, and the string table is shared
// so that every StringIdx agrees.
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace tachyon::symbols {
namespace {

struct Fn {
  const char* name;
  uint64_t start, size;
};
const Fn kFns[] = {{"Main", 0x1000, 0x100}, {"A", 0x1100, 0x100}, {"B", 0x1200, 0x100}};

class TableProvider final : public ISymbolProvider {
 public:
  explicit TableProvider(SymbolStringTable& strings) : m_strings(strings) {}
  Result<void> load(const Module& module, const std::filesystem::path&) override {
    if (module.path == "broken.so") return makeError(ErrorCode::CorruptData, "unreadable symbols");
    return {};
  }
  ResolvedAddress resolve(uint64_t rva) override {
    ResolvedAddress r;
    for (const Fn& f : kFns) {
      if (rva < f.start || rva >= f.start + f.size) continue;
      r.function = m_strings.intern(f.name);
      r.functionStartRva = f.start;
      r.resolved = true;
    }
    return r;
  }
  Result<std::vector<LineRange>> linesForFunction(uint64_t) override {
    return makeError(ErrorCode::NotFound, "none");
  }
  Result<FunctionBounds> functionBounds(uint64_t) override { return makeError(ErrorCode::NotFound, "none"); }

 private:
  SymbolStringTable& m_strings;
};

// Modules: 0 a.exe (resolves), 1 b.exe (no symbol file), 2 broken.so (the provider's load fails), 3 c.exe
// (resolves; the locate function of the "slow" fixture blocks on it).
model::TraceModel makeModel(size_t extraFrames) {
  model::TraceModel m;
  for (const char* path : {"a.exe", "b.exe", "broken.so", "c.exe"}) {
    model::Module mod;
    mod.path = path;
    mod.base = 0x400000 + 0x10000 * m.modules.size();
    mod.size = 0x1000;
    mod.id.kind = 2;
    mod.id.len = 4;
    mod.id.bytes[0] = static_cast<uint8_t>(m.modules.size() + 1);
    m.modules.push_back(mod);
  }
  m.frames = {{ModuleIdx(0), 0x1010}, {ModuleIdx(0), 0x1110}, {ModuleIdx(1), 0x1010},
              {ModuleIdx(2), 0x1010}, {ModuleIdx(3), 0x1210}};
  for (size_t i = 0; i < extraFrames; ++i)  // the complete model knows more frames of the same modules
    m.frames.push_back({ModuleIdx(static_cast<uint32_t>(i % 4)), 0x1000 + 0x10 * i});
  m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
  for (uint32_t f = 0; f < m.frames.size(); ++f)
    m.nodes.push_back(model::CallstackNode{CallstackIdx(f == 0 ? 0 : f), FrameIdx(f)});
  return m;
}

struct Counters {
  std::atomic<int> located{0};
  std::atomic<int> created{0};
  std::atomic<bool> holdC{false};  // locate blocks on c.exe until cancelled
};

// A service that counts what it locates and loads. `strings` null = its own table.
std::unique_ptr<SymbolService> makeService(Counters& counters,
                                           std::shared_ptr<SymbolStringTable> strings = nullptr) {
  auto svc = std::make_unique<SymbolService>(std::vector<std::string>{}, std::filesystem::path{},
                                             std::move(strings));
  svc->setLocate([&counters](const Module& mod, const std::atomic<bool>* cancel) -> Result<std::filesystem::path> {
    ++counters.located;
    if (mod.path == "b.exe") return makeError(ErrorCode::NotFound, "no symbols for b.exe");
    if (mod.path == "c.exe" && counters.holdC.load()) {
      while (!cancel->load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return makeError(ErrorCode::Cancelled, "cancelled");
    }
    return std::filesystem::path("t.sym");
  });
  SymbolStringTable& sink = svc->stringSink();
  svc->setProviderFactory([&sink, &counters](const Module&, const std::filesystem::path&) {
    ++counters.created;
    return std::unique_ptr<ISymbolProvider>(new TableProvider(sink));
  });
  return svc;
}

std::string name(const SymbolService& svc, FrameIdx f) {
  return std::string(svc.symbolStrings().get(svc.lookup(f).function));
}

TEST(AdoptRun, FinishedModulesAreNotLocatedOrLoadedAgain) {
  Counters first;
  auto preview = makeService(first);
  model::TraceModel previewModel = makeModel(0);
  preview->resolveAllAsync(previewModel);
  preview->waitAll();
  ASSERT_EQ(first.created.load(), 3);  // a, broken, c (b has no file)
  ASSERT_EQ(preview->modulesLoaded(), 3u);

  Counters second;
  auto full = makeService(second, preview->sharedStrings());
  EXPECT_EQ(full->adoptRun(*preview), 4u);  // every module reached a final state
  model::TraceModel fullModel = makeModel(40);
  std::atomic<int> callbacks{0};
  full->resolveAllAsync(fullModel, [&](ModuleIdx, SymbolState) { ++callbacks; });
  full->waitAll();

  // Resolution count on the swap: 0 for the unchanged modules.
  EXPECT_EQ(second.located.load(), 0);
  EXPECT_EQ(second.created.load(), 0);
  EXPECT_EQ(full->modulesLoaded(), 0u);
  EXPECT_EQ(full->modulesAdopted(), 4u);
  EXPECT_EQ(callbacks.load(), 8);  // Loading + final state per module, as for a normal run
  const SymbolProgress progress = full->progress();
  EXPECT_EQ(progress.done, progress.total);

  EXPECT_EQ(full->moduleState(ModuleIdx(0)), SymbolState::Resolved);
  EXPECT_EQ(full->moduleState(ModuleIdx(1)), SymbolState::NotFound);
  EXPECT_EQ(full->moduleState(ModuleIdx(2)), SymbolState::Failed);
  EXPECT_EQ(full->moduleState(ModuleIdx(3)), SymbolState::Resolved);
  EXPECT_EQ(full->moduleError(ModuleIdx(1)), preview->moduleError(ModuleIdx(1)));
  EXPECT_FALSE(full->moduleError(ModuleIdx(2)).empty());
  // The new model's frames (more of them) resolve through the adopted providers, with the same StringIdx.
  EXPECT_EQ(name(*full, FrameIdx(0)), "Main");
  EXPECT_EQ(name(*full, FrameIdx(1)), "A");
  EXPECT_EQ(name(*full, FrameIdx(4)), "B");
  EXPECT_EQ(full->lookup(FrameIdx(0)).function, preview->lookup(FrameIdx(0)).function);
  EXPECT_NE(full->provider(ModuleIdx(0)), nullptr);
  EXPECT_EQ(full->provider(ModuleIdx(0)), preview->provider(ModuleIdx(0)));  // shared
  size_t resolved = 0;
  for (uint32_t f = 0; f < fullModel.frames.size(); ++f) resolved += full->lookup(FrameIdx(f)).resolved;
  EXPECT_GT(resolved, 20u);
  // Memoized non-leaf variant is computed for the new model as well.
  EXPECT_TRUE(full->lookupCaller(FrameIdx(1)).resolved);
}

TEST(AdoptRun, TheAdoptedProvidersOutliveThePreviewService) {
  Counters first, second;
  std::unique_ptr<SymbolService> full;
  model::TraceModel fullModel = makeModel(8);
  {
    auto preview = makeService(first);
    model::TraceModel previewModel = makeModel(0);
    preview->resolveAllAsync(previewModel);
    preview->waitAll();
    full = makeService(second, preview->sharedStrings());
    EXPECT_GT(full->adoptRun(*preview), 0u);
  }  // the preview service (and the model it ran on) are gone
  full->resolveAllAsync(fullModel);
  full->waitAll();
  EXPECT_EQ(name(*full, FrameIdx(0)), "Main");
  EXPECT_EQ(full->modulesLoaded(), 0u);
  ASSERT_NE(full->provider(ModuleIdx(0)), nullptr);
  EXPECT_TRUE(full->provider(ModuleIdx(0))->resolve(0x1010).resolved);
}

TEST(AdoptRun, AChangedModuleRecordIsResolvedAgain) {
  Counters first, second;
  auto preview = makeService(first);
  model::TraceModel previewModel = makeModel(0);
  preview->resolveAllAsync(previewModel);
  preview->waitAll();

  auto full = makeService(second, preview->sharedStrings());
  full->adoptRun(*preview);
  model::TraceModel fullModel = makeModel(0);
  fullModel.modules[0].base += 0x1000;  // a.exe was loaded elsewhere: not the same module
  fullModel.modules[3].load_ts = 99;    // load timestamps are not part of the identity
  full->resolveAllAsync(fullModel);
  full->waitAll();
  EXPECT_EQ(full->modulesAdopted(), 3u);
  EXPECT_EQ(full->modulesLoaded(), 1u);
  EXPECT_EQ(second.created.load(), 1);
  EXPECT_EQ(second.located.load(), 1);
  EXPECT_EQ(name(*full, FrameIdx(0)), "Main");
  EXPECT_NE(full->provider(ModuleIdx(0)), preview->provider(ModuleIdx(0)));
}

TEST(AdoptRun, AnUnfinishedRunIsCancelledAndItsRemainingModulesAreResolvedNormally) {
  Counters first, second;
  first.holdC = true;  // the preview's run never finishes c.exe by itself
  auto preview = makeService(first);
  preview->setWorkerThreads(2);
  model::TraceModel previewModel = makeModel(0);
  preview->resolveAllAsync(previewModel);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (preview->progress().done < 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(preview->progress().done, 3u);
  EXPECT_NE(preview->moduleState(ModuleIdx(3)), SymbolState::Resolved);

  auto full = makeService(second, preview->sharedStrings());
  EXPECT_EQ(full->adoptRun(*preview), 3u);  // joins the preview's workers: c.exe is abandoned
  model::TraceModel fullModel = makeModel(4);
  full->resolveAllAsync(fullModel);
  full->waitAll();
  EXPECT_EQ(full->modulesAdopted(), 3u);
  EXPECT_EQ(full->modulesLoaded(), 1u);  // c.exe, located and loaded by the new run
  EXPECT_EQ(second.located.load(), 1);
  EXPECT_EQ(full->moduleState(ModuleIdx(3)), SymbolState::Resolved);
  EXPECT_EQ(name(*full, FrameIdx(4)), "B");
}

TEST(AdoptRun, ServicesWithDifferentStringTablesAdoptNothing) {
  Counters first, second;
  auto preview = makeService(first);
  model::TraceModel previewModel = makeModel(0);
  preview->resolveAllAsync(previewModel);
  preview->waitAll();
  auto full = makeService(second);  // its own table: the StringIdx values would not agree
  EXPECT_EQ(full->adoptRun(*preview), 0u);
  model::TraceModel fullModel = makeModel(0);
  full->resolveAllAsync(fullModel);
  full->waitAll();
  EXPECT_EQ(full->modulesAdopted(), 0u);
  EXPECT_EQ(full->modulesLoaded(), 3u);
}

TEST(AdoptRun, AdoptingBeforeAnyRunOrFromItselfDoesNothing) {
  Counters c;
  auto a = makeService(c);
  EXPECT_EQ(a->adoptRun(*a), 0u);
  auto b = makeService(c, a->sharedStrings());
  EXPECT_EQ(b->adoptRun(*a), 0u);  // `a` never ran
}

TEST(AdoptRun, QueriesOnThePreviewServiceStayValidWhileTheNewRunWorks) {
  Counters first, second;
  auto preview = makeService(first);
  model::TraceModel previewModel = makeModel(0);
  preview->resolveAllAsync(previewModel);
  preview->waitAll();
  auto full = makeService(second, preview->sharedStrings());
  full->adoptRun(*preview);
  model::TraceModel fullModel = makeModel(2000);
  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};
  std::thread reader([&] {  // an analysis snapshot of the preview keeps querying it
    while (!stop.load()) {
      if (name(*preview, FrameIdx(0)) == "Main") ++reads;
      if (preview->provider(ModuleIdx(0))) preview->provider(ModuleIdx(0))->resolve(0x1110);
    }
  });
  full->resolveAllAsync(fullModel);
  full->waitAll();
  stop = true;
  reader.join();
  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(name(*preview, FrameIdx(0)), "Main");
  EXPECT_EQ(name(*full, FrameIdx(0)), "Main");
}

}  // namespace
}  // namespace tachyon::symbols
