// DWARF provider perf budget (WP-72): resolve 1 M distinct RVAs spread over the testbed (full DWARF) and libc
// (usually only a dynamic symbol table; DWARF when libc6-dbg is installed). "Cold" includes loading the
// module and the lazy per-compilation-unit indexing; "warm" is a second pass over the same addresses. Numbers
// are printed.
#include "model/model.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "symbols/dwarf/elf_reader.h"
#include "symbols/symbol_strings.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

#ifndef TACHYON_TESTBED_EXE
#error "TACHYON_TESTBED_EXE must be defined by the build"
#endif

namespace tachyon::symbols {
namespace {

namespace fs = std::filesystem;

// Budgets. The spec target is 20 s for 1 M addresses warm. The local best (4-core container, RelWithDebInfo)
// is far below that; the regression guard asserts a generous multiple of the recorded local best.
constexpr double kSpecBudgetSeconds = 20.0;
constexpr double kRegressionBudgetWarmSeconds = 2.0;  // local best ~0.25 s, so ~8x headroom for slow runners
constexpr size_t kTotalAddresses = 1'000'000;

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct Subject {
  fs::path file;
  uint64_t bias = 0;
  std::vector<uint64_t> rvas;
  size_t resolved = 0;
};

// Every executable address of the file as RVAs, thinned to `quota` evenly spaced distinct values.
void collectRvas(Subject& s, size_t quota) {
  auto elf = ElfFile::open(s.file);
  ASSERT_TRUE(elf.ok()) << elf.error().toString();
  s.bias = elf.value().loadBias();
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  uint64_t total = 0;
  for (const ElfSection& sec : elf.value().sections()) {
    if ((sec.flags & kElfShfExecinstr) == 0 || sec.type == kElfShtNobits || sec.size == 0 ||
        sec.addr < s.bias)
      continue;
    spans.emplace_back(sec.addr - s.bias, sec.size);
    total += sec.size;
  }
  // Evenly spaced picks (Bresenham) so that exactly min(quota, total) distinct RVAs result.
  const uint64_t want = std::min<uint64_t>(total, std::max<size_t>(1, quota));
  uint64_t acc = 0;
  for (const auto& [start, size] : spans) {
    for (uint64_t off = 0; off < size; ++off) {
      acc += want;
      if (acc >= total) {
        acc -= total;
        s.rvas.push_back(start + off);
      }
    }
  }
}

double resolveAll(DwarfSymbolProvider& p, Subject& s) {
  const auto t0 = std::chrono::steady_clock::now();
  size_t resolved = 0;
  for (const uint64_t rva : s.rvas) resolved += p.resolve(rva).resolved ? 1 : 0;
  s.resolved = resolved;
  return secondsSince(t0);
}

TEST(DwarfPerf, ResolveOneMillionRvasAcrossTestbedAndLibc) {
  std::vector<Subject> subjects;
  subjects.push_back(Subject{fs::path(TACHYON_TESTBED_EXE), 0, {}, 0});
  for (const char* libc :
       {"/lib/x86_64-linux-gnu/libc.so.6", "/usr/lib/x86_64-linux-gnu/libc.so.6", "/lib64/libc.so.6"}) {
    std::error_code ec;
    if (fs::exists(libc, ec)) {
      subjects.push_back(Subject{fs::path(libc), 0, {}, 0});
      break;
    }
  }
  // Fill the quota: the testbed contributes every executable byte (up to half), libc thins its text to the
  // rest.
  size_t have = 0;
  for (Subject& s : subjects) {
    const bool last = &s == &subjects.back();
    collectRvas(s, last ? kTotalAddresses - have : kTotalAddresses / 2);
    have += s.rvas.size();
  }
  ASSERT_GT(have, 100'000u) << "too few executable addresses to measure anything";

  SymbolStringTable strings;
  double coldTotal = 0, warmTotal = 0;
  size_t resolvedTotal = 0;
  std::vector<std::unique_ptr<DwarfSymbolProvider>> providers;
  for (Subject& s : subjects) {
    auto provider = std::make_unique<DwarfSymbolProvider>(strings, makeLibdwOpener());
    model::Module module;
    module.path = s.file.string();
    const auto t0 = std::chrono::steady_clock::now();
    const Result<void> loaded = provider->load(module, s.file);
    ASSERT_TRUE(loaded.ok()) << s.file << ": " << loaded.error().toString();
    const double loadSeconds = secondsSince(t0);
    const double cold = loadSeconds + resolveAll(*provider, s);
    const double warm = resolveAll(*provider, s);
    std::printf(
        "[dwarf perf] %-40s %s  %8zu rvas  load %.3f s  cold %.3f s  warm %.3f s (%.0f ns/rva)  resolved "
        "%zu\n",
        s.file.filename().string().c_str(), provider->hasDwarf() ? "dwarf " : "symtab", s.rvas.size(),
        loadSeconds, cold, warm, warm * 1e9 / static_cast<double>(std::max<size_t>(1, s.rvas.size())),
        s.resolved);
    coldTotal += cold;
    warmTotal += warm;
    resolvedTotal += s.resolved;
    providers.push_back(std::move(provider));
  }
  std::printf(
      "[dwarf perf] TOTAL %zu distinct rvas: cold %.3f s, warm %.3f s, %zu resolved (spec budget %.0f s "
      "warm)\n",
      have, coldTotal, warmTotal, resolvedTotal, kSpecBudgetSeconds);

  EXPECT_GT(resolvedTotal, have / 20) << "implausibly few addresses resolved";
  // Scale the budgets to the number of addresses actually measured (a small testbed without libc has fewer).
  const double scale = static_cast<double>(have) / static_cast<double>(kTotalAddresses);
  EXPECT_LT(warmTotal, kSpecBudgetSeconds * std::max(1.0, scale));
  EXPECT_LT(warmTotal, kRegressionBudgetWarmSeconds * std::max(1.0, scale));
}

}  // namespace
}  // namespace tachyon::symbols
