// WP-65b: the pieces of the viewer's Live mode that do not need an agent: dropping the idle threads of a
// snapshot, and LiveSymbols (one symbol service per set of tables, adopted across epochs, never restarting a
// run in progress).
#include "viewer/live/live_session.h"
#include "symbols/json_symbol_provider.h"
#include "viewer/live/live_symbols.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

model::Thread makeThread(uint32_t tid, bool target = false) {
  model::Thread t;
  t.tid = tid;
  t.pid = 7;
  t.name = "t" + std::to_string(tid);
  t.is_target = target;
  return t;
}

}  // namespace

TEST(LivePrune, DropsTheThreadsWithNothingInTheWindowAndRemapsWhatRefersToThem) {
  model::TraceModel m;
  // 0: idle (dropped)   1: samples   2: idle target (kept)   3: only a context switch (dropped)   4: idle
  // (dropped) 5: only a frame mark   6: only a scope event
  for (uint32_t tid : {100u, 101u, 102u, 103u, 104u, 105u, 106u})
    m.threads.push_back(makeThread(tid, tid == 102));
  m.samplesByThread.resize(7);
  m.samplesByThread[1].ts = {10, 20};
  m.samplesByThread[1].callstack = {CallstackIdx(1), CallstackIdx(1)};
  m.samplesByThread[1].cpu = {0, 0};
  m.samplesByThread[1].flags = {0, 0};
  for (size_t i = 0; i < 7; ++i) m.tidToThread.emplace(m.threads[i].tid, ThreadIdx(static_cast<uint32_t>(i)));
  trace::ContextSwitchRec cs{};
  cs.old_tid = 103;
  cs.new_tid = 999;  // a thread that is not in the table
  m.contextSwitches.push_back(cs);
  trace::ScopeEventRec scope{};
  scope.tid = 106;
  m.scopeEvents.push_back(scope);
  m.frameMarks.push_back({30, ThreadIdx(5), StringIdx(0), 1});
  m.markers.push_back({31, ThreadIdx(5), StringIdx(0), StringIdx(0), 0});
  m.markers.push_back({32, ThreadIdx(), StringIdx(0), StringIdx(0), 0});  // global
  m.statesByThread.resize(7);
  m.scopesByThread.resize(7);

  EXPECT_EQ(pruneIdleThreads(m), 3u);
  ASSERT_EQ(m.threads.size(), 4u);
  const uint32_t expected[] = {101, 102, 105, 106};
  for (size_t i = 0; i < 4; ++i) EXPECT_EQ(m.threads[i].tid, expected[i]) << i;
  ASSERT_EQ(m.samplesByThread.size(), 4u);
  EXPECT_EQ(m.samplesByThread[0].ts.size(), 2u);
  EXPECT_EQ(m.statesByThread.size(), 4u);
  EXPECT_EQ(m.scopesByThread.size(), 4u);
  EXPECT_EQ(m.tidToThread.size(), 4u);
  EXPECT_EQ(m.tidToThread.at(105), ThreadIdx(2));
  EXPECT_EQ(m.tidToThread.count(100), 0u);
  EXPECT_EQ(m.frameMarks[0].thread, ThreadIdx(2));
  EXPECT_EQ(m.markers[0].thread, ThreadIdx(2));
  EXPECT_FALSE(m.markers[1].thread.valid());

  // Nothing idle: nothing changes.
  EXPECT_EQ(pruneIdleThreads(m), 0u);
  EXPECT_EQ(m.threads.size(), 4u);
}

namespace {

// A symbol configuration whose provider factory counts loads (the fixture's JSON symbols).
struct CountingConfig {
  std::shared_ptr<std::atomic<int>> loads = std::make_shared<std::atomic<int>>(0);
  int loadDelayMs = 0;
  LiveSymbolsConfig make() const {
    LiveSymbolsConfig config;
    config.frameEpochIntervalMs = 0;  // every change starts an epoch at once (the batching has its own test)
    auto counter = loads;
    const int delay = loadDelayMs;
    config.configure = [counter, delay](symbols::SymbolService& service) {
      EXPECT_TRUE(service.addSymbolFileOverride(fixture("frames.symbols.json")).ok());
      symbols::SymbolService* svc = &service;
      service.setProviderFactory(
          [svc, counter, delay](const model::Module&,
                                const fs::path& file) -> std::unique_ptr<symbols::ISymbolProvider> {
            ++*counter;
            if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            if (file.extension() == ".json")
              return std::make_unique<symbols::JsonSymbolProvider>(svc->stringSink());
            return nullptr;
          });
    };
    return config;
  }
};

bool waitDone(const symbols::SymbolService& service) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    const symbols::SymbolProgress p = service.progress();
    if (p.total > 0 && p.done == p.total) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

model::TraceModel loadFrames() {
  auto loaded = model::TraceModel::load(fixture("frames.tachyon"));
  EXPECT_TRUE(loaded.ok());
  return std::move(*loaded);
}

}  // namespace

TEST(LiveSymbols, TheSameTablesKeepTheSameServiceAndGrownTablesAdoptTheFinishedRun) {
  CountingConfig counting;
  LiveSymbols live(counting.make());
  model::TraceModel a = loadFrames();
  auto first = live.serviceFor(a);
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(waitDone(*first));
  EXPECT_EQ(counting.loads->load(), 1);
  EXPECT_EQ(live.stats().epochs, 1u);
  FrameIdx resolvedFrame;
  for (size_t f = 0; f < a.frames.size() && !resolvedFrame.valid(); ++f)
    if (first->lookup(FrameIdx(static_cast<uint32_t>(f))).resolved)
      resolvedFrame = FrameIdx(static_cast<uint32_t>(f));
  ASSERT_TRUE(resolvedFrame.valid()) << "the fixture's symbols resolve some frame";

  // The same tables (a later snapshot of a quiet capture): the very same service, no work.
  model::TraceModel same = a;
  EXPECT_EQ(live.serviceFor(same), first);
  EXPECT_EQ(live.stats().epochs, 1u);

  // A new frame appeared: a new service that adopts the finished module (no second load) and answers for the
  // old frames as the old one did.
  model::TraceModel grown = a;
  grown.frames.push_back(model::Frame{ModuleIdx(0), a.frames.empty() ? 0x100 : a.frames[0].addr_or_rva + 1});
  auto second = live.serviceFor(grown);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(second, first);
  ASSERT_TRUE(waitDone(*second));
  EXPECT_EQ(counting.loads->load(), 1) << "the module was adopted, not loaded again";
  EXPECT_EQ(live.stats().epochs, 2u);
  EXPECT_GE(live.stats().modulesAdopted, 1u);
  EXPECT_TRUE(second->lookup(resolvedFrame).resolved);
  EXPECT_EQ(second->symbolStrings().get(second->lookup(resolvedFrame).function),
            first->symbolStrings().get(first->lookup(resolvedFrame).function));
  EXPECT_EQ(second->lookup(resolvedFrame).function, first->lookup(resolvedFrame).function)
      << "the string table is shared: the same function has the same StringIdx";
  EXPECT_LE(live.stats().epochsAlive, 3u);
}

TEST(LiveSymbols, FrameOnlyGrowthIsBatchedButANewModuleStartsAnEpochAtOnce) {
  CountingConfig counting;
  LiveSymbolsConfig config = counting.make();
  config.frameEpochIntervalMs = 400;
  LiveSymbols live(std::move(config));
  model::TraceModel a = loadFrames();
  auto first = live.serviceFor(a);
  ASSERT_TRUE(waitDone(*first));
  model::TraceModel grown = a;
  grown.frames.push_back(model::Frame{ModuleIdx(0), 0x321});
  EXPECT_EQ(live.serviceFor(grown), first) << "new frames alone wait for the batching interval";
  EXPECT_EQ(live.stats().epochs, 1u);
  // A new module does not wait.
  model::TraceModel moreModules = grown;
  moreModules.modules.push_back(a.modules.front());
  moreModules.modules.back().base += 0x100000;
  EXPECT_NE(live.serviceFor(moreModules), first);
  EXPECT_EQ(live.stats().epochs, 2u);
  // And once the interval has passed the frames get theirs.
  auto second = live.serviceFor(moreModules);
  ASSERT_TRUE(waitDone(*second));
  model::TraceModel evenMore = moreModules;
  evenMore.frames.push_back(model::Frame{ModuleIdx(0), 0x322});
  EXPECT_EQ(live.serviceFor(evenMore), second);
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  EXPECT_NE(live.serviceFor(evenMore), second);
  EXPECT_EQ(live.stats().epochs, 3u);
}

TEST(LiveSymbols, ARunInProgressIsNeverRestartedByNewerTables) {
  CountingConfig counting;
  counting.loadDelayMs = 400;
  LiveSymbols live(counting.make());
  model::TraceModel a = loadFrames();
  auto first = live.serviceFor(a);
  ASSERT_NE(first, nullptr);
  // The module is still loading (400 ms): newer tables do not start a new epoch (that would cancel the load
  // and begin it again, for ever when snapshots come faster than the load).
  model::TraceModel grown = a;
  grown.frames.push_back(model::Frame{ModuleIdx(0), 0x123});
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(live.serviceFor(grown), first);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(live.stats().epochs, 1u);
  ASSERT_TRUE(waitDone(*first));
  // Once the run is done the next snapshot gets its epoch.
  auto second = live.serviceFor(grown);
  EXPECT_NE(second, first);
  EXPECT_EQ(live.stats().epochs, 2u);
  ASSERT_TRUE(waitDone(*second));
  EXPECT_EQ(counting.loads->load(), 1);
}

TEST(LiveSymbols, ResetStartsFromScratch) {
  CountingConfig counting;
  LiveSymbols live(counting.make());
  model::TraceModel a = loadFrames();
  auto first = live.serviceFor(a);
  ASSERT_TRUE(waitDone(*first));
  live.reset(counting.make());
  auto second = live.serviceFor(a);
  EXPECT_NE(second, first);
  ASSERT_TRUE(waitDone(*second));
  EXPECT_EQ(counting.loads->load(), 2) << "nothing is adopted across a reset (new symbol path, Retry)";
}
