// Perf budgets (WP-20): resolve every frame of `medium` (cold: parse symbols.json, build tables, resolve all
// frames; memoized: lookup() of every frame). Measured numbers are printed.
#include "model/model.h"
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace tachyon::symbols {
namespace {

// Budgets: local best was 16 ms cold (4 modules, 929 frames) and 2.3 ns/lookup memoized; the asserted limits
// are about 6x / 20x of that to absorb slow shared CI runners and Windows/MSVC (cold is dominated by JSON
// parsing, which scales with the symbols.json size, not with the frame count).
constexpr double kColdBudgetSeconds = 0.1;
constexpr double kMemoBudgetNsPerLookup = 50.0;

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

TEST(SymbolsPerf, ResolveAllFramesOfMedium) {
  const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
  auto loaded = model::TraceModel::load(dir / "medium.tachyon");
  ASSERT_TRUE(loaded.ok());
  model::TraceModel m = std::move(loaded).value();
  const size_t frames = m.frames.size();
  ASSERT_GT(frames, 0u);

  double bestCold = 1e30;
  for (int run = 0; run < 3; ++run) {
    SymbolService svc({}, {});
    ASSERT_TRUE(svc.addSymbolFileOverride(dir / "medium.symbols.json").ok());
    const auto t0 = std::chrono::steady_clock::now();
    svc.resolveAllAsync(m);
    svc.waitAll();
    const double s = secondsSince(t0);
    std::printf("[symbols perf] cold resolve #%d: %.4f s for %zu frames in %zu modules (%.0f ns/frame)\n",
                run, s, frames, m.modules.size(), s * 1e9 / static_cast<double>(frames));
    bestCold = std::min(bestCold, s);

    if (run == 2) {
      constexpr int kPasses = 50;
      size_t resolved = 0;
      const auto t1 = std::chrono::steady_clock::now();
      for (int pass = 0; pass < kPasses; ++pass) {
        for (uint32_t f = 0; f < frames; ++f) resolved += svc.lookup(FrameIdx(f)).resolved;
      }
      const double memo = secondsSince(t1);
      const double perLookup = memo * 1e9 / (static_cast<double>(frames) * kPasses);
      std::printf(
          "[symbols perf] memoized lookup: %.1f ns/lookup (%zu lookups, %zu resolved; budget %.0f ns)\n",
          perLookup, frames * static_cast<size_t>(kPasses), resolved, kMemoBudgetNsPerLookup);
      EXPECT_GT(resolved, 0u);
      EXPECT_LT(perLookup, kMemoBudgetNsPerLookup);
    }
  }
  std::printf("[symbols perf] cold best: %.4f s (budget %.2f s)\n", bestCold, kColdBudgetSeconds);
  EXPECT_LT(bestCold, kColdBudgetSeconds);
}

}  // namespace
}  // namespace tachyon::symbols
