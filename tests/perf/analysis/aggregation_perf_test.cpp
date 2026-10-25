// Perf budgets (WP-32): full-range bottomUpFunctions on `medium` < 300 ms and on `large` < 3 s (asserted at
// 2x); a memoized repeat must be near-instant. `large` is skipped when its fixture has not been generated
// (target tachyon_fixtures_large). Measured numbers are printed.
#include "analysis/query.h"

#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace tachyon::analysis {
namespace {

double seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct Prepared {
  model::TraceModel m;
  symbols::SymbolService svc{std::vector<std::string>{}, std::filesystem::path{}};
};

bool prepare(Prepared& p, const std::string& name) {
  const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
  if (!std::filesystem::exists(dir / (name + ".tachyon"))) return false;
  auto r = model::TraceModel::load(dir / (name + ".tachyon"));
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  if (!r.ok()) return false;
  p.m = std::move(*r);
  EXPECT_TRUE(buildThreadStates(p.m).ok());
  EXPECT_TRUE(p.svc.addSymbolFileOverride(dir / (name + ".symbols.json")).ok());
  p.svc.resolveAllAsync(p.m);
  p.svc.waitAll();
  return true;
}

void runBudget(const std::string& name, double budgetSeconds, int reps) {
  Prepared p;
  if (!prepare(p, name)) GTEST_SKIP() << name << " fixture not generated";
  double best = 1e30;
  size_t rows = 0, samples = 0;
  for (const auto& s : p.m.samplesByThread) samples += s.ts.size();
  for (int i = 0; i < reps; ++i) {
    QueryContext ctx(p.m, p.svc);  // empty memo each time
    const auto t0 = std::chrono::steady_clock::now();
    auto r = bottomUpFunctions(ctx, {});
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    rows = r->size();
    std::printf("[analysis perf] %s bottomUpFunctions #%d: %.4f s\n", name.c_str(), i, s);
    best = std::min(best, s);

    const auto t1 = std::chrono::steady_clock::now();
    auto again = bottomUpFunctions(ctx, {});
    const double cached = seconds(t1);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->size(), rows);
    EXPECT_LT(cached, std::max(budgetSeconds * 0.1, 0.01)) << "memoized repeat should be near-instant";
    if (i == 0) std::printf("[analysis perf] %s memoized repeat: %.6f s\n", name.c_str(), cached);
  }
  std::printf(
      "[analysis perf] %s best: %.4f s (target %.2f s, assert %.2f s, %u hw threads); %zu samples, %zu "
      "rows\n",
      name.c_str(), best, budgetSeconds, budgetSeconds * 2, std::thread::hardware_concurrency(), samples,
      rows);
  EXPECT_LT(best, budgetSeconds * 2);
}

TEST(AggregationPerf, MediumBottomUp) {
  runBudget("medium", 0.3, 5);
}

TEST(AggregationPerf, LargeBottomUp) {
  runBudget("large", 3.0, 3);
}

TEST(AggregationPerf, MediumTopDownAndButterfly) {
  Prepared p;
  if (!prepare(p, "medium")) GTEST_SKIP();
  QueryContext ctx(p.m, p.svc);
  auto t0 = std::chrono::steady_clock::now();
  auto tree = topDownTree(ctx, {});
  std::printf("[analysis perf] medium topDownTree: %.4f s\n", seconds(t0));
  ASSERT_TRUE(tree.ok());
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  ASSERT_FALSE(rows->empty());
  t0 = std::chrono::steady_clock::now();
  auto b = butterfly(ctx, {}, (*rows)[0].key);
  std::printf("[analysis perf] medium butterfly: %.4f s\n", seconds(t0));
  ASSERT_TRUE(b.ok());
}

}  // namespace
}  // namespace tachyon::analysis
