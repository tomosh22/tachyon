// Perf budgets (WP-63): export `medium` (7.5 M samples) to speedscope in < 5 s and to Chrome trace JSON in
// < 3 s, each asserted at 2x (10 s and 6 s). The output goes to a counting null stream, so the numbers
// measure the exporters, not the disk. Measured numbers are printed.
#include "analysis/query.h"
#include "analysis/thread_states.h"
#include "export/export.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <ostream>
#include <streambuf>

namespace tachyon::exporters {
namespace {

constexpr double kSpeedscopeBudgetSeconds = 5.0;  // target; assert at 2x
constexpr double kChromeBudgetSeconds = 3.0;

class CountingBuf final : public std::streambuf {
 public:
  uint64_t bytes = 0;

 protected:
  std::streamsize xsputn(const char*, std::streamsize n) override {
    bytes += static_cast<uint64_t>(n);
    return n;
  }
  int_type overflow(int_type c) override {
    ++bytes;
    return c;
  }
};

struct Medium {
  Medium() : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
    auto r = model::TraceModel::load(dir / "medium.tachyon");
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(analysis::buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(dir / "medium.symbols.json").ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
  }
  model::TraceModel m;
  symbols::SymbolService svc;
};

double seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

TEST(ExportPerf, MediumSpeedscope) {
  Medium f;
  analysis::QueryContext ctx(f.m, f.svc);
  const double budget = kSpeedscopeBudgetSeconds * 2;
  double best = 1e30;
  for (int i = 0; i < 2; ++i) {
    CountingBuf buf;
    std::ostream os(&buf);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = exportSpeedscope(ctx, {}, os);
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    std::printf(
        "[export perf] medium speedscope #%d: %.3f s, %llu samples, %llu frames, %.1f MiB (target %.1f s, "
        "assert "
        "%.1f s)\n",
        i, s, static_cast<unsigned long long>(r->samples), static_cast<unsigned long long>(r->frames),
        static_cast<double>(buf.bytes) / (1024.0 * 1024.0), kSpeedscopeBudgetSeconds, budget);
    best = std::min(best, s);
  }
  EXPECT_LT(best, budget);
}

TEST(ExportPerf, MediumChromeTrace) {
  Medium f;
  const double budget = kChromeBudgetSeconds * 2;
  double best = 1e30;
  for (int i = 0; i < 2; ++i) {
    CountingBuf buf;
    std::ostream os(&buf);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = exportChromeTrace(f.m, f.svc, {}, os);
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    std::printf(
        "[export perf] medium chrome #%d: %.3f s, %llu events, %.1f MiB (target %.1f s, assert %.1f s)\n", i,
        s, static_cast<unsigned long long>(r->events), static_cast<double>(buf.bytes) / (1024.0 * 1024.0),
        kChromeBudgetSeconds, budget);
    best = std::min(best, s);
  }
  EXPECT_LT(best, budget);
}

}  // namespace
}  // namespace tachyon::exporters
