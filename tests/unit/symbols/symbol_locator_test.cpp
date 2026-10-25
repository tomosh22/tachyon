#include "symbols/symbol_locator.h"

#include "mock_http_server.h"
#include "symbols/symbol_keys.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace tachyon::symbols {
namespace {

namespace fs = std::filesystem;
using testutil::MockHttpServer;

constexpr const char* kPdbKey = "1B72224D37B8179228200ED8994498B22";

model::Module pdbModule(const std::string& image = "C:\\bin\\game.exe", uint32_t age = 2) {
  model::Module m;
  m.path = image;
  m.id.kind = trace::kModuleIdPdb;
  m.id.len = 16;
  const uint8_t guid[16] = {0x4D, 0x22, 0x72, 0x1B, 0xB8, 0x37, 0x92, 0x17,
                            0x28, 0x20, 0x0E, 0xD8, 0x99, 0x44, 0x98, 0xB2};
  for (size_t i = 0; i < 16; ++i) m.id.bytes[i] = guid[i];
  m.id.age = age;
  return m;
}

model::Module buildIdModule(uint8_t seed = 0x10) {
  model::Module m;
  m.path = "/opt/game/libgame.so";
  m.id.kind = trace::kModuleIdGnuBuildId;
  m.id.len = 20;
  for (size_t i = 0; i < 20; ++i) m.id.bytes[i] = static_cast<uint8_t>(seed + i);
  return m;
}

void writeFile(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

size_t countFiles(const fs::path& root, const std::string& ext = {}) {
  size_t n = 0;
  std::error_code ec;
  if (!fs::exists(root, ec)) return 0;
  for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
    if (e.is_regular_file() && (ext.empty() || e.path().extension() == ext)) ++n;
  }
  return n;
}

class LocatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
#ifndef _WIN32
    setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    setenv("no_proxy", "127.0.0.1,localhost", 1);
#endif
    static std::atomic<int> counter{0};
    m_root = fs::temp_directory_path() /
             ("tachyon-locator-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
              "-" + std::to_string(counter++) + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(m_root);
    fs::create_directories(m_root);
    m_serverRoot = m_root / "server";
    m_cache = m_root / "cache";
    fs::create_directories(m_serverRoot);
  }
  void TearDown() override {
    m_server.reset();
    std::error_code ec;
    fs::remove_all(m_root, ec);
  }

  MockHttpServer& server() {
    if (!m_server) {
      m_server = std::make_unique<MockHttpServer>(m_serverRoot);
      EXPECT_TRUE(m_server->ok());
    }
    return *m_server;
  }

  SymbolLocatorConfig httpConfig() {
    SymbolLocatorConfig c;
    c.servers = {server().url()};
    c.cacheDir = m_cache;
    c.useSystemDebugDirs = false;
    c.httpOptions.connectTimeoutMs = 2000;
    c.httpOptions.totalTimeoutMs = 5000;
    c.httpOptions.maxRetries = 3;
    c.httpOptions.initialBackoffMs = 10;
    c.httpOptions.maxBackoffMs = 40;
    return c;
  }

  fs::path m_root, m_serverRoot, m_cache;
  std::unique_ptr<MockHttpServer> m_server;
};

// --- local search order -----------------------------------------------------------------------------------

TEST_F(LocatorTest, ExplicitDirectoryBeatsEverythingElse) {
  const fs::path explicitDir = m_root / "explicit";
  writeFile(explicitDir / "game.pdb", "explicit");
  writeFile(m_root / "bin" / "game.pdb", "next-to-module");
  SymbolLocatorConfig c;
  c.searchPaths = {explicitDir};
  c.cacheDir = m_cache;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule((m_root / "bin" / "game.exe").string()));
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), explicitDir / "game.pdb");
}

TEST_F(LocatorTest, ExplicitFileIsUsedWhenNameMatches) {
  writeFile(m_root / "elsewhere" / "GAME.PDB", "x");
  writeFile(m_root / "elsewhere" / "other.pdb", "y");
  SymbolLocatorConfig c;
  c.searchPaths = {m_root / "elsewhere" / "other.pdb", m_root / "elsewhere" / "GAME.PDB"};
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value().filename(), "GAME.PDB");
}

TEST_F(LocatorTest, ExplicitSymstoreLayoutDirectory) {
  writeFile(m_root / "store" / "game.pdb" / kPdbKey / "game.pdb", "store");
  SymbolLocatorConfig c;
  c.searchPaths = {m_root / "store"};
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "store");
}

TEST_F(LocatorTest, NextToTheModule) {
  writeFile(m_root / "bin" / "game.pdb", "next");
  SymbolLocator loc;
  auto r = loc.locate(pdbModule((m_root / "bin" / "game.exe").string()));
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_root / "bin" / "game.pdb");
}

TEST_F(LocatorTest, ModuleWithoutIdentityOrFileIsNotFound) {
  SymbolLocator loc;
  model::Module m;
  m.path = "C:\\nowhere\\x.dll";
  auto r = loc.locate(m);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST_F(LocatorTest, LocalCacheFlatLayout) {
  writeFile(m_cache / "game.pdb" / kPdbKey / "game.pdb", "cached");
  SymbolLocator loc({}, m_cache);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "cached");
}

TEST_F(LocatorTest, LocalCacheTwoTierLayout) {
  writeFile(m_cache / "index2.txt", "");
  writeFile(m_cache / "ga" / "game.pdb" / kPdbKey / "game.pdb", "tier2");
  SymbolLocator loc({}, m_cache);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "tier2");
}

TEST_F(LocatorTest, FilePtrInCachePathAndMessage) {
  const fs::path target = m_root / "real" / "game.pdb";
  writeFile(target, "redirected");
  writeFile(m_cache / "game.pdb" / kPdbKey / "file.ptr", "PATH:" + target.string() + "\r\n");
  SymbolLocator loc({}, m_cache);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), target);

  fs::remove_all(m_cache);
  writeFile(m_cache / "game.pdb" / kPdbKey / "file.ptr", "MSG:file was deleted by retention policy");
  SymbolLocator loc2({}, m_cache);
  auto r2 = loc2.locate(pdbModule());
  ASSERT_FALSE(r2.ok());
  EXPECT_EQ(r2.error().code, ErrorCode::NotFound);
  EXPECT_NE(r2.error().message.find("retention policy"), std::string::npos) << r2.error().message;
}

TEST_F(LocatorTest, LocalSymbolStoreServerEntry) {
  writeFile(m_root / "share" / "game.pdb" / kPdbKey / "game.pdb", "share");
  SymbolLocator loc({(m_root / "share").string()}, m_cache);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "share");
}

TEST_F(LocatorTest, BuildIdLocalDebugTree) {
  const model::Module m = buildIdModule();
  const std::string hex = buildIdHex(m.id);
  writeFile(m_root / "dbg" / ".build-id" / hex.substr(0, 2) / (hex.substr(2) + ".debug"), "elf-debug");
  SymbolLocatorConfig c;
  c.searchPaths = {m_root / "dbg"};
  c.useSystemDebugDirs = false;
  SymbolLocator loc(c);
  auto r = loc.locate(m);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "elf-debug");
}

TEST_F(LocatorTest, BuildIdFallsBackToTheBinaryItself) {
  model::Module m = buildIdModule();
  m.path = (m_root / "libgame.so").string();
  writeFile(m_root / "libgame.so", "elf");
  SymbolLocatorConfig c;
  c.useSystemDebugDirs = false;
  SymbolLocator loc(c);
  auto r = loc.locate(m);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_root / "libgame.so");
}

// --- HTTP -------------------------------------------------------------------------------------------------

TEST_F(LocatorTest, HttpHitDownloadsIntoCacheThenServesFromCache) {
  const std::string rel = std::string("game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / rel, "pdb-bytes");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_cache / "game.pdb" / kPdbKey / "game.pdb");
  EXPECT_EQ(readFile(r.value()), "pdb-bytes");
  EXPECT_EQ(server().requestCount("/" + rel), 1);
  EXPECT_EQ(countFiles(m_cache, ".part"), 0u);

  auto again = loc.locate(pdbModule());
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(server().totalRequests(), 1) << "second locate must hit the cache";
}

TEST_F(LocatorTest, HttpTwoTierCacheLayoutWhenIndex2Exists) {
  writeFile(m_cache / "index2.txt", "");
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "game.pdb", "tier");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_cache / "ga" / "game.pdb" / kPdbKey / "game.pdb");
}

TEST_F(LocatorTest, HttpMissIsNotFoundWithoutRetries) {
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
  EXPECT_EQ(server().requestCount(std::string("/game.pdb/") + kPdbKey + "/game.pdb"), 1) << "404 is final";
  EXPECT_EQ(countFiles(m_cache), 0u);
}

TEST_F(LocatorTest, HttpFilePtrRedirect) {
  const fs::path target = m_root / "elsewhere" / "redirected.pdb";
  writeFile(target, "ptr-target");
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "file.ptr", "PATH:" + target.string());
  SymbolLocatorConfig c = httpConfig();
  c.searchPaths = {m_root / "elsewhere"};
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), target);
}

TEST_F(LocatorTest, HttpFilePtrMessageIsReported) {
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "file.ptr", "MSG:pulled by legal");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().message.find("pulled by legal"), std::string::npos) << r.error().message;
}

TEST_F(LocatorTest, CompressedPdbIsReportedNotDecompressed) {
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "game.pd_", "MSCF....");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
  EXPECT_NE(r.error().message.find("compressed CAB not supported"), std::string::npos) << r.error().message;
}

TEST_F(LocatorTest, ServersAreTriedInOrder) {
  fs::create_directories(m_root / "second");
  writeFile(m_root / "second" / "game.pdb" / kPdbKey / "game.pdb", "from-second");
  MockHttpServer second(m_root / "second");
  SymbolLocatorConfig c = httpConfig();
  c.servers = {server().url(), second.url()};
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "from-second");
  EXPECT_GE(server().totalRequests(), 1);
  EXPECT_EQ(second.requestCount(std::string("/game.pdb/") + kPdbKey + "/game.pdb"), 1);
}

TEST_F(LocatorTest, RetriesServerErrorsThenSucceeds) {
  const std::string path = std::string("/game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / path.substr(1), "eventually");
  server().failNext(path, 2, 500);
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "eventually");
  EXPECT_EQ(server().requestCount(path), 3);
}

TEST_F(LocatorTest, GivesUpAfterMaxRetriesAndLeavesNoPartFile) {
  const std::string path = std::string("/game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / path.substr(1), "never");
  server().failNext(path, 100, 503);
  SymbolLocatorConfig c = httpConfig();
  c.httpOptions.maxRetries = 2;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(server().requestCount(path), 3);
  EXPECT_EQ(countFiles(m_cache), 0u);
}

TEST_F(LocatorTest, TruncatedDownloadLeavesNoPartOrFinalFile) {
  const std::string path = std::string("/game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / path.substr(1), std::string(4096, 'x'));
  server().truncate(path);
  SymbolLocatorConfig c = httpConfig();
  c.httpOptions.maxRetries = 1;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(countFiles(m_cache), 0u) << "no .part and no partial final file may remain";
}

TEST_F(LocatorTest, SlowServerTimesOut) {
  const std::string path = std::string("/game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / path.substr(1), "slow");
  server().delay(path, 1500);
  SymbolLocatorConfig c = httpConfig();
  c.httpOptions.totalTimeoutMs = 250;
  c.httpOptions.maxRetries = 1;
  // Client-level: the error is Timeout (the locator turns it into a NotFound with the reason).
  auto client = makeDefaultHttpClient(c.httpOptions);
  auto direct = client->get(server().url() + path, m_root / "out.pdb", nullptr, nullptr);
  ASSERT_FALSE(direct.ok());
  EXPECT_EQ(direct.error().code, ErrorCode::Timeout) << direct.error().toString();
  EXPECT_FALSE(fs::exists(m_root / "out.pdb"));
  EXPECT_FALSE(fs::exists(m_root / "out.pdb.part"));

  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
  EXPECT_EQ(countFiles(m_cache), 0u);
}

TEST_F(LocatorTest, HttpClientDownloadsAtomicallyAndReportsStatuses) {
  writeFile(m_serverRoot / "a.bin", "hello");
  auto client = makeDefaultHttpClient(httpConfig().httpOptions);
  auto ok = client->get(server().url() + "/a.bin", m_root / "d" / "a.bin", nullptr, nullptr);
  ASSERT_TRUE(ok.ok()) << ok.error().toString();
  EXPECT_EQ(ok.value().code, 200);
  EXPECT_EQ(ok.value().bytes, 5u);
  EXPECT_EQ(readFile(m_root / "d" / "a.bin"), "hello");
  auto miss = client->get(server().url() + "/nope", m_root / "d" / "nope", nullptr, nullptr);
  ASSERT_FALSE(miss.ok());
  EXPECT_EQ(miss.error().code, ErrorCode::NotFound);
  EXPECT_EQ(server().requestCount("/nope"), 1);
}

TEST_F(LocatorTest, ErrorsNeverContainCredentials) {
  const std::string path = std::string("/game.pdb/") + kPdbKey + "/game.pdb";
  server().failNext(path, 100, 500);
  SymbolLocatorConfig c = httpConfig();
  c.servers = {"http://alice:hunter2@" + server().url().substr(7)};
  c.httpOptions.maxRetries = 0;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().message.find("hunter2"), std::string::npos) << r.error().message;
  EXPECT_EQ(r.error().message.find("alice"), std::string::npos) << r.error().message;
}

TEST_F(LocatorTest, ConcurrentDownloadsAreBounded) {
  constexpr int kModules = 8;
  for (int i = 0; i < kModules; ++i) {
    const std::string name = "m" + std::to_string(i);
    model::Module m = pdbModule("C:\\bin\\" + name + ".dll");
    const std::string rel = name + ".pdb/" + kPdbKey + "/" + name + ".pdb";
    writeFile(m_serverRoot / rel, "data" + std::to_string(i));
    server().delay("/" + rel, 200);
  }
  SymbolLocatorConfig c = httpConfig();
  c.maxConcurrentDownloads = 2;
  SymbolLocator loc(c);
  std::vector<std::thread> threads;
  std::atomic<int> okCount{0};
  for (int i = 0; i < kModules; ++i) {
    threads.emplace_back([&, i] {
      auto r = loc.locate(pdbModule("C:\\bin\\m" + std::to_string(i) + ".dll"));
      if (r.ok() && readFile(r.value()) == "data" + std::to_string(i)) ++okCount;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(okCount.load(), kModules);
  EXPECT_EQ(server().maxConcurrent(), 2) << "never more than the limit, and the limit is reached";
}

TEST_F(LocatorTest, SameModuleRequestedConcurrentlyDownloadsOnce) {
  const std::string rel = std::string("game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / rel, "once");
  server().delay("/" + rel, 150);
  SymbolLocator loc(httpConfig());
  std::vector<std::thread> threads;
  std::atomic<int> okCount{0};
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back([&] {
      if (loc.locate(pdbModule()).ok()) ++okCount;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(okCount.load(), 6);
  EXPECT_EQ(server().requestCount("/" + rel), 1);
}

TEST_F(LocatorTest, DebuginfodLayout) {
  const model::Module m = buildIdModule(0x40);
  const std::string hex = buildIdHex(m.id);
  writeFile(m_serverRoot / "buildid" / hex / "debuginfo", "dwarf");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(m);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_cache / "buildid" / hex / "debuginfo");
  EXPECT_EQ(readFile(r.value()), "dwarf");
  EXPECT_EQ(server().requestCount("/buildid/" + hex + "/debuginfo"), 1);
}

TEST_F(LocatorTest, PeImageIsFetchedByBinaryKey) {
  model::Module m;
  m.path = "C:\\Windows\\foo.dll";
  m.id.kind = trace::kModuleIdPeTimestampSize;
  m.id.len = 8;
  const uint8_t raw[8] = {0x3D, 0x2C, 0x1B, 0x5A, 0x00, 0x00, 0x0A, 0x00};
  for (size_t i = 0; i < 8; ++i) m.id.bytes[i] = raw[i];
  writeFile(m_serverRoot / "foo.dll" / "5A1B2C3DA0000" / "foo.dll", "MZ");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(m);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_cache / "foo.dll" / "5A1B2C3DA0000" / "foo.dll");
}

// --- fake client ------------------------------------------------------------------------------------------

class FakeHttp final : public IHttpClient {
 public:
  Result<HttpStatus> get(const std::string& url, const fs::path& dest, const std::atomic<bool>*,
                         const HttpProgressFn&, uint64_t) override {
    urls.push_back(url);
    if (url.find("hit") == std::string::npos) return makeError(ErrorCode::NotFound, "404");
    fs::create_directories(dest.parent_path());
    std::ofstream(dest, std::ios::binary) << "fake";
    return HttpStatus{200, 4, 1};
  }
  std::vector<std::string> urls;
};

TEST_F(LocatorTest, UsesInjectedHttpClient) {
  auto fake = std::make_shared<FakeHttp>();
  SymbolLocatorConfig c;
  c.servers = {"https://symbols.example/hit"};
  c.cacheDir = m_cache;
  c.http = fake;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  ASSERT_EQ(fake->urls.size(), 1u);
  EXPECT_EQ(fake->urls[0], std::string("https://symbols.example/hit/game.pdb/") + kPdbKey + "/game.pdb");
}

TEST_F(LocatorTest, MakeDefaultLocateWorksAsLocateFn) {
  writeFile(m_root / "bin" / "game.pdb", "x");
  LocateFn fn = makeDefaultLocate(SymbolLocatorConfig{});
  auto r = fn(pdbModule((m_root / "bin" / "game.exe").string()));
  ASSERT_TRUE(r.ok()) << r.error().toString();
}

// --- hardening --------------------------------------------------------------------------------------------

TEST_F(LocatorTest, HttpTwoTierServer) {
  writeFile(m_serverRoot / "index2.txt", "");
  writeFile(m_serverRoot / "ga" / "game.pdb" / kPdbKey / "game.pdb", "two-tier-server");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "two-tier-server");
  EXPECT_EQ(server().requestCount("/index2.txt"), 1);

  // The index2.txt answer is cached per server: another module does not ask again.
  writeFile(m_serverRoot / "ot" / "other.pdb" / kPdbKey / "other.pdb", "second");
  auto r2 = loc.locate(pdbModule("C:\\bin\\other.dll"));
  ASSERT_TRUE(r2.ok()) << r2.error().toString();
  EXPECT_EQ(server().requestCount("/index2.txt"), 1);
}

TEST_F(LocatorTest, HttpFlatServerWithoutIndex2IsAskedOnceAndStaysMissing) {
  SymbolLocator loc(httpConfig());
  EXPECT_FALSE(loc.locate(pdbModule()).ok());
  EXPECT_FALSE(loc.locate(pdbModule("C:\\bin\\other.dll")).ok());
  EXPECT_EQ(server().requestCount("/index2.txt"), 1);
}

TEST_F(LocatorTest, RemoteFilePtrPathOutsideConfiguredRootsIsIgnored) {
  const fs::path secret = m_root / "secret" / "game.pdb";
  writeFile(secret, "must not be served");
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "file.ptr", "PATH:" + secret.string());
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
  EXPECT_NE(r.error().message.find("file.ptr PATH from remote server ignored"), std::string::npos)
      << r.error().message;
}

TEST_F(LocatorTest, RemoteFilePtrPathUnderASearchPathIsFollowed) {
  const fs::path target = m_root / "trusted" / "sub" / "redirected.bin";
  writeFile(target, "ok");
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "file.ptr", "PATH:" + target.string());
  SymbolLocatorConfig c = httpConfig();
  c.searchPaths = {m_root / "trusted"};
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), target);
}

TEST_F(LocatorTest, RemoteFilePtrDotDotEscapeIsIgnored) {
  writeFile(m_root / "secret" / "x.pdb", "secret");
  writeFile(m_root / "trusted" / "keep", "k");
  const fs::path escape = m_root / "trusted" / ".." / "secret" / "x.pdb";
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "file.ptr", "PATH:" + escape.string());
  SymbolLocatorConfig c = httpConfig();
  c.searchPaths = {m_root / "trusted"};
  SymbolLocator loc(c);
  EXPECT_FALSE(loc.locate(pdbModule()).ok());
}

TEST_F(LocatorTest, OversizedFilePtrIsNotRead) {
  const fs::path target = m_root / "trusted" / "t.bin";
  writeFile(target, "ok");
  writeFile(m_serverRoot / "game.pdb" / kPdbKey / "file.ptr",
            "PATH:" + target.string() + std::string(20000, ' '));
  SymbolLocatorConfig c = httpConfig();
  c.searchPaths = {m_root / "trusted"};
  c.httpOptions.maxRetries = 0;
  SymbolLocator loc(c);
  EXPECT_FALSE(loc.locate(pdbModule()).ok());
}

TEST_F(LocatorTest, LocalCacheFilePtrIsStillFollowedAnywhere) {
  const fs::path target = m_root / "anywhere" / "game.pdb";
  writeFile(target, "x");
  writeFile(m_cache / "game.pdb" / kPdbKey / "file.ptr", "PATH:" + target.string());
  SymbolLocator loc({}, m_cache);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), target);
}

TEST_F(LocatorTest, DownloadSizeCapAbortsAndLeavesNothingBehind) {
  const std::string path = std::string("/game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / path.substr(1), std::string(5000, 'x'));
  SymbolLocatorConfig c = httpConfig();
  c.maxDownloadBytes = 1000;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().message.find("exceeds limit"), std::string::npos) << r.error().message;
  EXPECT_EQ(server().requestCount(path), 1) << "an oversized response is not retried";
  EXPECT_EQ(countFiles(m_cache), 0u);
}

TEST_F(LocatorTest, DownloadSizeCapAppliesWithoutContentLengthToo) {
  // Direct client call: a body above maxBytes is rejected and removed.
  writeFile(m_serverRoot / "big.bin", std::string(8000, 'x'));
  auto client = makeDefaultHttpClient(httpConfig().httpOptions);
  auto r = client->get(server().url() + "/big.bin", m_root / "d" / "big.bin", nullptr, nullptr, 100);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
  EXPECT_FALSE(fs::exists(m_root / "d" / "big.bin"));
  EXPECT_FALSE(fs::exists(m_root / "d" / "big.bin.part"));
}

TEST_F(LocatorTest, RedirectToNonHttpProtocolIsRefused) {
  const fs::path secret = m_root / "secret.pdb";
  writeFile(secret, "secret");
  const std::string path = std::string("/game.pdb/") + kPdbKey + "/game.pdb";
  server().redirect(path, "file://" + (secret.generic_string().front() == '/' ? "" : std::string("/")) +
                              secret.generic_string());
  SymbolLocatorConfig c = httpConfig();
  c.httpOptions.maxRetries = 0;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(countFiles(m_cache), 0u);
}

TEST_F(LocatorTest, UrlPathComponentsArePercentEncoded) {
  EXPECT_EQ(urlEncodeComponent("my lib#1.pdb"), "my%20lib%231.pdb");
  EXPECT_EQ(urlEncodeComponent("a-b_c.d~e"), "a-b_c.d~e");
  EXPECT_EQ(urlEncodeComponent("100%/x?y"), "100%25%2Fx%3Fy");

  model::Module m = pdbModule("C:\\bin\\my lib#1.dll");
  const std::string rel = std::string("my lib#1.pdb/") + kPdbKey + "/my lib#1.pdb";
  writeFile(m_serverRoot / rel, "encoded");
  SymbolLocator loc(httpConfig());
  auto r = loc.locate(m);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(readFile(r.value()), "encoded");
  EXPECT_EQ(server().requestCount(std::string("/my%20lib%231.pdb/") + kPdbKey + "/my%20lib%231.pdb"), 1);
}

// --- eviction ---------------------------------------------------------------------------------------------

TEST_F(LocatorTest, EvictionRemovesLeastRecentlyUsedFirstAndSparesPinnedFiles) {
  const auto now = fs::file_time_type::clock::now();
  const fs::path oldest = m_cache / "a.pdb" / "K1" / "a.pdb";
  const fs::path middle = m_cache / "b.pdb" / "K2" / "b.pdb";
  const fs::path newest = m_cache / "c.pdb" / "K3" / "c.pdb";
  writeFile(oldest, std::string(1000, 'a'));
  writeFile(middle, std::string(1000, 'b'));
  writeFile(newest, std::string(1000, 'c'));
  fs::last_write_time(oldest, now - std::chrono::hours(3));
  fs::last_write_time(middle, now - std::chrono::hours(2));
  fs::last_write_time(newest, now - std::chrono::hours(1));

  SymbolLocatorConfig c;
  c.cacheDir = m_cache;
  c.cacheMaxBytes = 2500;
  SymbolLocator loc(c);
  EXPECT_EQ(loc.evictCache(), 1000u);
  EXPECT_FALSE(fs::exists(oldest));
  EXPECT_FALSE(fs::exists(oldest.parent_path())) << "empty directories are pruned";
  EXPECT_TRUE(fs::exists(middle));
  EXPECT_TRUE(fs::exists(newest));

  // Pin the now-oldest file; shrinking the limit must evict the next candidate instead.
  SymbolLocatorConfig c2 = c;
  c2.cacheMaxBytes = 1500;
  SymbolLocator loc2(c2);
  loc2.pin(middle);
  EXPECT_EQ(loc2.evictCache(), 1000u);
  EXPECT_TRUE(fs::exists(middle)) << "pinned";
  EXPECT_FALSE(fs::exists(newest));
  loc2.unpin(middle);
  EXPECT_EQ(loc2.evictCache(), 0u) << "within the limit now";
}

TEST_F(LocatorTest, DownloadEvictsOldEntriesButNeverTheFileJustReturned) {
  const fs::path stale = m_cache / "stale.pdb" / "K" / "stale.pdb";
  writeFile(stale, std::string(5000, 's'));
  fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours(10));
  const std::string rel = std::string("game.pdb/") + kPdbKey + "/game.pdb";
  writeFile(m_serverRoot / rel, std::string(3000, 'g'));
  SymbolLocatorConfig c = httpConfig();
  c.cacheMaxBytes = 4000;
  SymbolLocator loc(c);
  auto r = loc.locate(pdbModule());
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_TRUE(fs::exists(r.value()));
  EXPECT_FALSE(fs::exists(stale));
}

TEST_F(LocatorTest, CacheHitRefreshesRecency) {
  const auto now = fs::file_time_type::clock::now();
  const fs::path a = m_cache / "game.pdb" / kPdbKey / "game.pdb";
  const fs::path b = m_cache / "other.pdb" / "K" / "other.pdb";
  writeFile(a, std::string(1000, 'a'));
  writeFile(b, std::string(1000, 'b'));
  fs::last_write_time(a, now - std::chrono::hours(5));
  fs::last_write_time(b, now - std::chrono::hours(1));
  SymbolLocatorConfig c;
  c.cacheDir = m_cache;
  c.cacheMaxBytes = 1500;
  SymbolLocator loc(c);
  ASSERT_TRUE(loc.locate(pdbModule()).ok());  // touches a
  loc.unpin(a);                               // not in use any more
  EXPECT_EQ(loc.evictCache(), 1000u);
  EXPECT_TRUE(fs::exists(a));
  EXPECT_FALSE(fs::exists(b));
}

}  // namespace
}  // namespace tachyon::symbols
