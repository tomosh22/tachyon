// DIA perf budget (WP-22, Windows only): resolve 1 M distinct RVAs spread across the testbed, ntdll.dll and
// kernelbase.dll in < 20 s with a warm symbol cache. PDBs of the system DLLs come from the Microsoft symbol
// server through SymbolLocator into a cache under the build directory (downloaded once, then warm); the test
// skips with a clear message when the server is unreachable. Numbers are printed and written to
// <build>/dia_perf_summary.txt.
#include "etlconvert/pe_info.h"
#include "symbols/dia/dia_provider.h"
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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace tachyon::symbols {
namespace {

namespace fs = std::filesystem;

constexpr const char* kMicrosoftServer = "https://msdl.microsoft.com/download/symbols";
constexpr size_t kTargetRvas = 1'000'000;
constexpr double kBudgetSeconds = 20.0;

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct PerfModule {
  std::string name;
  fs::path image;
  fs::path pdb;
  model::Module module;
  std::unique_ptr<DiaSymbolProvider> provider;
  std::vector<uint32_t> rvas;  // every byte offset of every .pdata function
  double loadSeconds = 0;
};

bool prepare(PerfModule& pm, SymbolLocator* locator, std::string& why) {
  auto pe = etlconvert::parsePeFile(pm.image);
  if (!pe.ok() || !pe.value().hasCodeView) {
    why = pm.name + ": no CodeView record";
    return false;
  }
  pm.module.path = pm.image.string();
  pm.module.id.kind = trace::kModuleIdPdb;
  pm.module.id.len = 16;
  std::memcpy(pm.module.id.bytes.data(), pe.value().guid, 16);
  pm.module.id.age = pe.value().age;
  if (locator != nullptr) {
    auto found = locator->locate(pm.module);
    if (!found.ok()) {
      why = pm.name + ": " + found.error().toString();
      return false;
    }
    pm.pdb = found.value();
  }
  auto ranges = etlconvert::readPeFunctionRangesFromFile(pm.image);
  if (!ranges.ok() || ranges.value().empty()) {
    why = pm.name + ": no .pdata";
    return false;
  }
  for (const auto& r : ranges.value()) {
    for (uint32_t rva = r.beginRva; rva < r.endRva; ++rva) pm.rvas.push_back(rva);
  }
  return true;
}

TEST(DiaPerf, ResolveOneMillionRvasWarmCache) {
  std::string err;
  auto opener = makeComDiaOpener(&err);
  ASSERT_NE(opener, nullptr) << err;

  const fs::path binaryDir(TACHYON_BINARY_DIR);
  const fs::path cache = binaryDir / "dia_perf_symcache";
  SymbolLocatorConfig config;
  config.servers = {kMicrosoftServer};
  config.cacheDir = cache;
  config.httpOptions.connectTimeoutMs = 8000;
  config.httpOptions.totalTimeoutMs = 300000;
  config.httpOptions.maxRetries = 1;
  SymbolLocator locator(config);

  wchar_t sysDir[MAX_PATH] = {};
  ASSERT_GT(GetSystemDirectoryW(sysDir, MAX_PATH), 0u);
  std::vector<PerfModule> modules(3);
  modules[0].name = "tachyon-testbed.exe";
  modules[0].image = fs::path(TACHYON_TESTBED_EXE);
  modules[1].name = "ntdll.dll";
  modules[1].image = fs::path(sysDir) / "ntdll.dll";
  modules[2].name = "kernelbase.dll";
  modules[2].image = fs::path(sysDir) / "kernelbase.dll";

  // Testbed PDB sits next to the exe; the system ones are fetched (downloads are not part of the timing).
  {
    auto pe = etlconvert::parsePeFile(modules[0].image);
    ASSERT_TRUE(pe.ok());
    modules[0].pdb = pe.value().pdbPath;
    if (!fs::exists(modules[0].pdb)) modules[0].pdb = fs::path(modules[0].image).replace_extension(".pdb");
    ASSERT_TRUE(fs::exists(modules[0].pdb));
  }
  const auto fetchStart = std::chrono::steady_clock::now();
  for (size_t i = 0; i < modules.size(); ++i) {
    std::string why;
    if (!prepare(modules[i], i == 0 ? nullptr : &locator, why)) {
      if (i == 0) FAIL() << why;
      GTEST_SKIP() << "symbols unavailable (offline? " << why << ")";
    }
  }
  const double fetchSeconds = secondsSince(fetchStart);

  SymbolStringTable strings;
  const auto loadStart = std::chrono::steady_clock::now();
  for (PerfModule& pm : modules) {
    const auto t0 = std::chrono::steady_clock::now();
    pm.provider = std::make_unique<DiaSymbolProvider>(strings, opener);
    auto r = pm.provider->load(pm.module, pm.pdb);
    ASSERT_TRUE(r.ok()) << pm.name << ": " << r.error().toString();
    pm.loadSeconds = secondsSince(t0);
  }
  const double loadSeconds = secondsSince(loadStart);

  // 1 M distinct (module, rva) pairs, drawn uniformly from all candidates with a fixed seed.
  struct Pick {
    uint32_t module;
    uint32_t rva;
  };
  std::vector<Pick> all;
  for (uint32_t m = 0; m < modules.size(); ++m) {
    for (const uint32_t rva : modules[m].rvas) all.push_back({m, rva});
  }
  std::mt19937_64 rng(0x7AC4);
  std::shuffle(all.begin(), all.end(), rng);
  if (all.size() > kTargetRvas) all.resize(kTargetRvas);
  ASSERT_GE(all.size(), 500'000u) << "too few candidate addresses";

  size_t resolved = 0;
  size_t inlined = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const Pick& p : all) {
    const ResolvedAddress a = modules[p.module].provider->resolve(p.rva);
    resolved += a.resolved ? 1 : 0;
    inlined += a.inlines.empty() ? 0 : 1;
  }
  const double seconds = secondsSince(t0);

  uint64_t lookups = 0;
  for (const PerfModule& pm : modules) lookups += pm.provider->functionLookups();

  char buf[1024];
  std::snprintf(
      buf, sizeof(buf),
      "[dia perf] resolved %zu distinct RVAs (%zu resolved, %zu with inline chains) across %zu modules in "
      "%.3f s (%.2f us/rva); budget %.0f s\n"
      "[dia perf] PDB fetch/locate %.2f s (not budgeted), PDB load %.2f s total (",
      all.size(), resolved, inlined, modules.size(), seconds, seconds * 1e6 / static_cast<double>(all.size()),
      kBudgetSeconds, fetchSeconds, loadSeconds);
  std::string summary = buf;
  for (const PerfModule& pm : modules) {
    std::snprintf(buf, sizeof(buf), "%s %.2f s; ", pm.name.c_str(), pm.loadSeconds);
    summary += buf;
  }
  std::snprintf(buf, sizeof(buf), ")\n[dia perf] session function lookups (function-cache misses): %llu\n",
                static_cast<unsigned long long>(lookups));
  summary += buf;
  std::printf("%s", summary.c_str());
  std::fflush(stdout);
  {
    std::ofstream out(binaryDir / "dia_perf_summary.txt", std::ios::trunc);
    out << summary;
  }

  EXPECT_GT(resolved, all.size() / 2);
  EXPECT_LT(seconds, kBudgetSeconds);
}

}  // namespace
}  // namespace tachyon::symbols
