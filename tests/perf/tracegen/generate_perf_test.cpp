// Perf budget (WP-04): generating the `medium` fixture (16 threads x 60 s at 8 kHz, ~7.5 M samples) with the
// brute-force verification must stay well inside the ~20 s fixture-generation allowance of the CI runners.
// Measured on a dev box: ~0.8 s to generate + ~0.3 s to verify. Budget 4 s (asserted at 2x = 8 s) for each
// phase.
#include "generator.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

namespace tachyon::tracegen {
namespace {

constexpr double kBudgetSeconds = 4.0;  // target; assert at 2x

TEST(TracegenPerf, MediumFixtureGenerationAndVerification) {
  auto scenario =
      loadScenario((std::filesystem::path(TACHYON_FIXTURE_DIR) / "tracegen" / "medium.json").string());
  ASSERT_TRUE(scenario.ok()) << scenario.error().toString();
  const auto dir = std::filesystem::temp_directory_path() / "tachyon_tracegen_perf";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  auto report = generateFixture(*scenario, dir, GenerateOptions{});
  ASSERT_TRUE(report.ok()) << report.error().toString();
  std::printf("tracegen medium: %llu samples, %.1f MiB, generate %.2f s (%.1f M samples/s), verify %.2f s\n",
              static_cast<unsigned long long>(report->samples),
              static_cast<double>(report->tachyonBytes) / (1024.0 * 1024.0), report->generateSeconds,
              static_cast<double>(report->samples) / report->generateSeconds / 1e6, report->verifySeconds);
  EXPECT_LT(report->generateSeconds, 2 * kBudgetSeconds);
  EXPECT_LT(report->verifySeconds, 2 * kBudgetSeconds);
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
}  // namespace tachyon::tracegen
