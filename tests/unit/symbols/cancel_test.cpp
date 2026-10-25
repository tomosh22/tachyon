// WP-21b: cancellable symbol downloads, cache unpinning and the const-model resolveAllAsync.
#include "mock_http_server.h"
#include "symbols/symbol_service.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace tachyon::symbols {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using testutil::MockHttpServer;

constexpr const char* kPdbKey = "1B72224D37B8179228200ED8994498B22";

model::Module pdbModule(const std::string& image) {
  model::Module m;
  m.path = image;
  m.id.kind = trace::kModuleIdPdb;
  m.id.len = 16;
  const uint8_t guid[16] = {0x4D, 0x22, 0x72, 0x1B, 0xB8, 0x37, 0x92, 0x17,
                            0x28, 0x20, 0x0E, 0xD8, 0x99, 0x44, 0x98, 0xB2};
  for (size_t i = 0; i < 16; ++i) m.id.bytes[i] = guid[i];
  m.id.age = 2;
  return m;
}

std::string pdbRel(const std::string& name) {
  return name + ".pdb/" + kPdbKey + "/" + name + ".pdb";
}

class StubProvider final : public ISymbolProvider {
 public:
  Result<void> load(const Module&, const std::filesystem::path&) override { return {}; }
  ResolvedAddress resolve(uint64_t) override { return {}; }
  Result<std::vector<LineRange>> linesForFunction(uint64_t) override {
    return makeError(ErrorCode::NotFound, "none");
  }
  Result<FunctionBounds> functionBounds(uint64_t) override { return makeError(ErrorCode::NotFound, "none"); }
};

std::unique_ptr<ISymbolProvider> makeStub(const Module&, const fs::path&) {
  return std::make_unique<StubProvider>();
}

double msSince(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

class CancelTest : public ::testing::Test {
 protected:
  void SetUp() override {
#ifndef _WIN32
    setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    setenv("no_proxy", "127.0.0.1,localhost", 1);
#endif
    static std::atomic<int> counter{0};
    m_root = fs::temp_directory_path() / ("tachyon-cancel-test-" + std::to_string(counter++) + "-" +
                                          std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(m_root);
    m_serverRoot = m_root / "server";
    m_cache = m_root / "cache";
    fs::create_directories(m_serverRoot);
    m_server = std::make_unique<MockHttpServer>(m_serverRoot);
    ASSERT_TRUE(m_server->ok());
  }
  void TearDown() override {
    m_server.reset();
    std::error_code ec;
    fs::remove_all(m_root, ec);
  }

  void writeServerFile(const std::string& rel, const std::string& content) {
    fs::create_directories((m_serverRoot / rel).parent_path());
    std::ofstream(m_serverRoot / rel, std::ios::binary) << content;
  }

  SymbolLocatorConfig config() const {
    SymbolLocatorConfig c;
    c.servers = {m_server->url()};
    c.cacheDir = m_cache;
    c.useSystemDebugDirs = false;
    c.httpOptions.connectTimeoutMs = 2000;
    c.httpOptions.totalTimeoutMs = 120000;
    c.httpOptions.maxRetries = 3;
    c.httpOptions.initialBackoffMs = 10;
    c.httpOptions.maxBackoffMs = 40;
    return c;
  }

  // A model with `n` PDB modules m0..m(n-1), one frame each.
  static model::TraceModel makeModel(int n) {
    model::TraceModel m;
    for (int i = 0; i < n; ++i) {
      m.modules.push_back(pdbModule("C:\\bin\\m" + std::to_string(i) + ".dll"));
      m.frames.push_back({ModuleIdx(static_cast<uint32_t>(i)), 0x1000});
    }
    return m;
  }

  bool waitForRequests(int count) const {
    for (int i = 0; i < 500; ++i) {
      if (m_server->totalRequests() >= count) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  void stallAll(int modules) {
    for (int i = 0; i < modules; ++i) {
      const std::string rel = pdbRel("m" + std::to_string(i));
      writeServerFile(rel, "data");
      m_server->delay("/" + rel, 60000);  // stalls "indefinitely" for this test
    }
  }

  fs::path m_root, m_serverRoot, m_cache;
  std::unique_ptr<MockHttpServer> m_server;
};

TEST_F(CancelTest, DestructorReturnsPromptlyWithStalledTransfersInFlight) {
  constexpr int kModules = 4;
  stallAll(kModules);
  model::TraceModel model = makeModel(kModules);
  auto svc = std::make_unique<SymbolService>(std::vector<std::string>{}, fs::path{});
  svc->configureLocator(config());
  svc->setWorkerThreads(kModules);
  svc->resolveAllAsync(model);
  ASSERT_TRUE(waitForRequests(kModules)) << "all four transfers should be in flight";

  const auto t0 = Clock::now();
  svc.reset();
  const double ms = msSince(t0);
  EXPECT_LT(ms, 500.0) << "destructor took " << ms << " ms";
}

TEST_F(CancelTest, ExplicitCancelReturnsPromptlyAndLeavesNoFiles) {
  constexpr int kModules = 4;
  stallAll(kModules);
  model::TraceModel model = makeModel(kModules);
  SymbolService svc({}, {});
  svc.configureLocator(config());
  svc.setWorkerThreads(2);  // two modules are never started
  svc.resolveAllAsync(model);
  ASSERT_TRUE(waitForRequests(2));

  const auto t0 = Clock::now();
  svc.cancel();
  const double ms = msSince(t0);
  EXPECT_LT(ms, 500.0) << "cancel took " << ms << " ms";
  svc.waitAll();
  for (uint32_t i = 0; i < kModules; ++i) {
    EXPECT_EQ(svc.moduleState(ModuleIdx(i)), SymbolState::Pending) << "module " << i;
  }
  size_t files = 0;
  std::error_code ec;
  if (fs::exists(m_cache, ec)) {
    for (const auto& e : fs::recursive_directory_iterator(m_cache, ec)) files += e.is_regular_file() ? 1 : 0;
  }
  EXPECT_EQ(files, 0u) << "no part or final files remain";
}

TEST_F(CancelTest, LocatorHonoursTheCancelFlagDuringATransfer) {
  stallAll(1);
  SymbolLocator loc(config());
  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancel = true;
  });
  const auto t0 = Clock::now();
  auto r = loc.locate(pdbModule("C:\\bin\\m0.dll"), &cancel);
  const double ms = msSince(t0);
  canceller.join();
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled) << r.error().toString();
  EXPECT_LT(ms, 650.0);
}

TEST_F(CancelTest, BackoffSleepWakesOnCancel) {
  const std::string rel = pdbRel("m0");
  writeServerFile(rel, "data");
  m_server->failNext("/" + rel, 100, 500);
  SymbolLocatorConfig c = config();
  c.httpOptions.initialBackoffMs = 30000;
  c.httpOptions.maxBackoffMs = 30000;
  auto client = makeDefaultHttpClient(c.httpOptions);
  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancel = true;
  });
  const auto t0 = Clock::now();
  auto r = client->get(m_server->url() + "/" + rel, m_root / "out.pdb", &cancel, nullptr);
  const double ms = msSince(t0);
  canceller.join();
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled) << r.error().toString();
  EXPECT_LT(ms, 700.0);
  EXPECT_FALSE(fs::exists(m_root / "out.pdb.part"));
}

TEST_F(CancelTest, CancellableLocateReceivesTheServiceFlag) {
  model::TraceModel model = makeModel(1);
  SymbolService svc({}, {});
  std::atomic<bool> entered{false};
  svc.setLocate([&](const Module&, const std::atomic<bool>* cancel) -> Result<fs::path> {
    entered = true;
    while (!cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return makeError(ErrorCode::Cancelled, "cancelled");
  });
  svc.resolveAllAsync(model);
  for (int i = 0; i < 500 && !entered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(entered);
  const auto t0 = Clock::now();
  svc.cancel();
  EXPECT_LT(msSince(t0), 500.0);
}

TEST_F(CancelTest, LegacyLocateFunctionsStillCompile) {
  model::TraceModel model = makeModel(1);
  SymbolService svc({}, {});
  svc.setLocate([](const Module&) -> Result<fs::path> { return makeError(ErrorCode::NotFound, "none"); });
  svc.setLocate(makeDefaultLocate(SymbolLocatorConfig{}));
  svc.setLocate(makeDefaultCancellableLocate(SymbolLocatorConfig{}));
  svc.resolveAllAsync(model);
  svc.waitAll();
  EXPECT_EQ(model.modules[0].state, SymbolState::NotFound);
}

TEST_F(CancelTest, CacheFilesAreUnpinnedWhenTheRunEnds) {
  writeServerFile(pdbRel("m0"), std::string(1000, 'x'));
  model::TraceModel model = makeModel(1);
  SymbolService svc({}, {});
  SymbolLocatorConfig c = config();
  c.cacheMaxBytes = 1;  // anything unpinned is over the limit
  svc.configureLocator(c);
  svc.setProviderFactory(makeStub);
  svc.resolveAllAsync(model);
  svc.waitAll();
  ASSERT_EQ(svc.moduleState(ModuleIdx(0)), SymbolState::Resolved);
  const fs::path cached = m_cache / pdbRel("m0");
  ASSERT_TRUE(fs::exists(cached));

  EXPECT_EQ(svc.locator().evictCache(), 0u) << "pinned while its provider lives";
  EXPECT_TRUE(fs::exists(cached));
  svc.cancel();  // ends the run: providers are released, files unpinned
  EXPECT_EQ(svc.locator().evictCache(), 1000u);
  EXPECT_FALSE(fs::exists(cached));
}

TEST_F(CancelTest, CacheFileIsUnpinnedWhenTheProviderFailsToLoad) {
  writeServerFile(pdbRel("m0"), std::string(1000, 'x'));
  model::TraceModel model = makeModel(1);
  SymbolService svc({}, {});
  SymbolLocatorConfig c = config();
  c.cacheMaxBytes = 1;
  svc.configureLocator(c);
  svc.setProviderFactory(
      [](const Module&, const fs::path&) -> std::unique_ptr<ISymbolProvider> { return nullptr; });
  svc.resolveAllAsync(model);
  svc.waitAll();
  ASSERT_EQ(svc.moduleState(ModuleIdx(0)), SymbolState::Failed);
  EXPECT_EQ(svc.locator().evictCache(), 1000u) << "not pinned any more";
}

TEST_F(CancelTest, PinsAreBalancedAcrossRuns) {
  writeServerFile(pdbRel("m0"), std::string(1000, 'x'));
  model::TraceModel model = makeModel(1);
  SymbolService svc({}, {});
  SymbolLocatorConfig c = config();
  c.cacheMaxBytes = 1;
  svc.configureLocator(c);
  svc.setProviderFactory(makeStub);
  for (int run = 0; run < 2; ++run) {
    svc.resolveAllAsync(model);
    svc.waitAll();
    ASSERT_EQ(svc.moduleState(ModuleIdx(0)), SymbolState::Resolved);
    EXPECT_EQ(svc.locator().evictCache(), 0u) << "run " << run;
  }
  svc.cancel();
  svc.cancel();  // idempotent
  EXPECT_EQ(svc.locator().evictCache(), 1000u) << "one unpin per pin: nothing left pinned";
}

TEST_F(CancelTest, ResolveAllAsyncAcceptsAConstModel) {
  model::TraceModel model = makeModel(2);
  const model::TraceModel& view = model;
  SymbolService svc({}, {});
  svc.setLocate([](const Module&) -> Result<fs::path> { return fs::path("x.stub"); });
  svc.setProviderFactory(makeStub);
  svc.resolveAllAsync(view);
  svc.waitAll();
  EXPECT_EQ(svc.moduleState(ModuleIdx(0)), SymbolState::Resolved);
  EXPECT_EQ(model.modules[0].state, SymbolState::Pending) << "the const overload never writes the model";
  svc.waitAll(model);
  EXPECT_EQ(model.modules[0].state, SymbolState::Resolved);
  EXPECT_EQ(model.modules[1].state, SymbolState::Resolved);
}

}  // namespace
}  // namespace tachyon::symbols
