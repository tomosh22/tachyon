// WP-21b: WinHTTP transfers and SymbolService teardown must not wait for a stalled server.
#include "../../unit/symbols/mock_http_server.h"
#include "symbols/symbol_service.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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

class WinHttpCancelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    m_root = fs::temp_directory_path() / ("tachyon-winhttp-cancel-" + std::to_string(counter++) + "-" +
                                          std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(m_root);
    m_serverRoot = m_root / "server";
    fs::create_directories(m_serverRoot);
    m_server = std::make_unique<MockHttpServer>(m_serverRoot);
    ASSERT_TRUE(m_server->ok());
    for (int i = 0; i < kModules; ++i) {
      const std::string name = "m" + std::to_string(i);
      const std::string rel = name + ".pdb/" + kPdbKey + "/" + name + ".pdb";
      fs::create_directories((m_serverRoot / rel).parent_path());
      std::ofstream(m_serverRoot / rel, std::ios::binary) << "data";
      m_server->delay("/" + rel, 60000);
    }
  }
  void TearDown() override {
    m_server.reset();
    std::error_code ec;
    fs::remove_all(m_root, ec);
  }

  bool waitForRequests(int count) const {
    for (int i = 0; i < 500; ++i) {
      if (m_server->totalRequests() >= count) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  static constexpr int kModules = 4;
  fs::path m_root, m_serverRoot;
  std::unique_ptr<MockHttpServer> m_server;
};

TEST_F(WinHttpCancelTest, ClientGetAbortsWhileWaitingForTheResponse) {
  HttpOptions opts;
  opts.totalTimeoutMs = 120000;
  auto client = makeDefaultHttpClient(opts);
  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancel = true;
  });
  const std::string url = m_server->url() + "/m0.pdb/" + kPdbKey + "/m0.pdb";
  const auto t0 = Clock::now();
  auto r = client->get(url, m_root / "out.pdb", &cancel, nullptr);
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  canceller.join();
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled) << r.error().toString();
  EXPECT_LT(ms, 700.0);
  EXPECT_FALSE(fs::exists(m_root / "out.pdb.part"));
}

TEST_F(WinHttpCancelTest, ServiceDestructorReturnsPromptlyWithStalledTransfers) {
  model::TraceModel model;
  for (int i = 0; i < kModules; ++i) {
    model.modules.push_back(pdbModule("C:\\bin\\m" + std::to_string(i) + ".dll"));
    model.frames.push_back({ModuleIdx(static_cast<uint32_t>(i)), 0x1000});
  }
  auto svc = std::make_unique<SymbolService>(std::vector<std::string>{}, fs::path{});
  SymbolLocatorConfig c;
  c.servers = {m_server->url()};
  c.cacheDir = m_root / "cache";
  c.httpOptions.totalTimeoutMs = 120000;
  svc->configureLocator(c);
  svc->setWorkerThreads(kModules);
  svc->resolveAllAsync(model);
  ASSERT_TRUE(waitForRequests(kModules));

  const auto t0 = Clock::now();
  svc.reset();
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  EXPECT_LT(ms, 500.0) << "destructor took " << ms << " ms";
}

}  // namespace
}  // namespace tachyon::symbols
