#include "etlconvert/pe_info.h"
#include "symbols/symbol_keys.h"
#include "symbols/symbol_locator.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <fstream>

namespace tachyon::symbols {
namespace {

namespace fs = std::filesystem;

constexpr const char* kMicrosoftServer = "https://msdl.microsoft.com/download/symbols";

// Key of the running system's ntdll.dll via the PE parser, then a real download of its PDB.
TEST(MicrosoftSymbolServer, FetchesNtdllPdb) {
  wchar_t systemDir[MAX_PATH] = {};
  ASSERT_GT(GetSystemDirectoryW(systemDir, MAX_PATH), 0u);
  const fs::path ntdll = fs::path(systemDir) / "ntdll.dll";
  auto pe = etlconvert::parsePeFile(ntdll);
  ASSERT_TRUE(pe.ok()) << pe.error().toString();
  ASSERT_TRUE(pe.value().hasCodeView);

  model::Module m;
  m.path = ntdll.string();
  m.id.kind = trace::kModuleIdPdb;
  m.id.len = 16;
  std::memcpy(m.id.bytes.data(), pe.value().guid, 16);
  m.id.age = pe.value().age;
  const auto key = symbolKeyFor(m);
  ASSERT_TRUE(key.has_value());

  const fs::path cache = fs::temp_directory_path() / "tachyon-msdl-test-cache";
  std::error_code ec;
  fs::remove_all(cache, ec);
  SymbolLocatorConfig config;
  config.servers = {kMicrosoftServer};
  config.cacheDir = cache;
  config.httpOptions.connectTimeoutMs = 8000;
  config.httpOptions.totalTimeoutMs = 120000;
  config.httpOptions.maxRetries = 1;
  SymbolLocator locator(config);
  auto found = locator.locate(m);
  if (!found.ok()) {
    // A reachable server answers 404 for an unknown key; any other failure means no usable network.
    auto probe = makeDefaultHttpClient(config.httpOptions)
                     ->get(std::string(kMicrosoftServer) + "/" + key->relativePath(), cache / "probe.pdb",
                           nullptr, nullptr);
    fs::remove_all(cache, ec);
    if (probe.ok() || probe.error().code == ErrorCode::NotFound) {
      FAIL() << "symbol server reachable but locate failed: " << found.error().toString();
    }
    GTEST_SKIP() << "Microsoft symbol server unreachable: " << probe.error().toString();
  }

  std::ifstream in(found.value(), std::ios::binary);
  char magic[32] = {};
  in.read(magic, sizeof(magic));
  EXPECT_EQ(std::memcmp(magic,
                        "Microsoft C/C++ MSF 7.00\r\n\x1a"
                        "DS",
                        29),
            0)
      << "not an MSF 7.00 PDB";
  fs::remove_all(cache, ec);
}

}  // namespace
}  // namespace tachyon::symbols
