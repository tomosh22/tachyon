// Perf budgets (WP-62), asserted at 2x:
//   analyzeTrace on `medium` (load + states + symbols + report)  < 2 s
//   buildReport alone on `medium` (model already loaded)         < 1 s
#include "report/analyze.h"

#include "analysis/thread_states.h"
#include "report/report_builder.h"
#include "report/report_json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace tachyon::report {
namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

std::filesystem::path medium() {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon";
}

TEST(ReportPerf, FullReportOnMedium) {
  if (!std::filesystem::exists(medium())) GTEST_SKIP() << "medium fixture not generated";
  constexpr double kBudgetS = 2.0;
  double best = 1e30;
  size_t samples = 0;
  for (int i = 0; i < 3; ++i) {
    const auto t0 = Clock::now();
    auto r = analyzeTrace(medium(), {});
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    samples = r->trace.sampleCount;
    best = std::min(best, s);
  }
  std::printf("[perf] tachyon-report analyze on medium (%zu samples): best of 3 = %.3f s (budget %.1f s)\n",
              samples, best, kBudgetS);
  EXPECT_LT(best, 2 * kBudgetS);
}

TEST(ReportPerf, BuildReportOnLoadedMedium) {
  if (!std::filesystem::exists(medium())) GTEST_SKIP() << "medium fixture not generated";
  constexpr double kBudgetS = 1.0;
  auto loaded = model::TraceModel::load(medium());
  ASSERT_TRUE(loaded.ok());
  model::TraceModel m = std::move(*loaded);
  ASSERT_TRUE(analysis::buildThreadStates(m).ok());
  symbols::SymbolService svc(std::vector<std::string>{}, std::filesystem::path{});
  ASSERT_TRUE(svc.addSymbolFileOverride(medium().parent_path() / "medium.symbols.json").ok());
  svc.resolveAllAsync(m);
  svc.waitAll();
  double best = 1e30;
  std::string json;
  for (int i = 0; i < 3; ++i) {
    const auto t0 = Clock::now();
    auto r = buildReport(m, svc, {});
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    json = dumpJson(reportToJson(*r));
    best = std::min(best, s);
  }
  std::printf("[perf] buildReport on loaded medium: best of 3 = %.3f s (budget %.1f s), %zu JSON bytes\n",
              best, kBudgetS, json.size());
  EXPECT_LT(best, 2 * kBudgetS);
}

}  // namespace
}  // namespace tachyon::report
