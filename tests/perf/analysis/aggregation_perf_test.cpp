// Perf budgets (WP-32): full-range bottomUpFunctions on `medium` < 300 ms and on `large` < 3 s (asserted at
// 2x); a memoized repeat must be near-instant. `large` is skipped when its fixture has not been generated
// (target tachyon_fixtures_large). Measured numbers are printed.
#include "analysis/query.h"

#include "analysis/attribution.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
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

// WP-33b: a query with Filter::subtreePath must cost about the same as the same query with
// Filter::subtreeRoot (budget 1.2x, asserted at 2x like the other budgets). Matching is done per distinct
// leaf callstack node of the weight table, never per sample. "cold" = fresh context (the weight scan is
// included); "warm" = weights cached, only the per-node stack work differs, over several different subtrees.
struct SubtreePick {
  CallstackIdx root;  // a callstack node at depth `depth`
  std::vector<FunctionKey> path;
};

std::vector<SubtreePick> pickSubtrees(const QueryContext& ctx, size_t count, size_t depth) {
  const model::TraceModel& m = ctx.model();
  std::vector<SubtreePick> out;
  std::set<uint32_t> seen;
  for (const model::SampleColumns& cols : m.samplesByThread) {
    for (size_t i = 0; i < cols.ts.size() && out.size() < count; i += 97) {
      const CallstackIdx leaf = cols.callstack[i];
      if ((cols.flags[i] & 1u) != 0 || !leaf.valid() || leaf.get() == 0) continue;
      std::vector<uint32_t> chain;  // leaf first
      for (uint32_t n = leaf.get(); n != 0; n = m.nodes[n].parent.get()) chain.push_back(n);
      if (chain.size() < depth + 2) continue;
      const uint32_t root = chain[chain.size() - depth];
      if (!seen.insert(root).second) continue;
      SubtreePick p;
      p.root = CallstackIdx(root);
      for (size_t d = 0; d < depth; ++d) p.path.push_back(frameKeyAt(ctx, leaf, chain.size() - 1 - d, false));
      out.push_back(std::move(p));
    }
  }
  return out;
}

void runSubtreePathBudget(const std::string& name) {
  Prepared p;
  if (!prepare(p, name)) GTEST_SKIP() << name << " fixture not generated";
  size_t samples = 0;
  for (const auto& s : p.m.samplesByThread) samples += s.ts.size();
  constexpr int kReps = 5;
  for (const char* query : {"bottomUp", "topDown"}) {
    auto run = [&](QueryContext& ctx, const Filter& f) {
      const auto t0 = std::chrono::steady_clock::now();
      if (std::string(query) == "bottomUp") {
        EXPECT_TRUE(bottomUpFunctions(ctx, f).ok());
      } else {
        EXPECT_TRUE(topDownTree(ctx, f).ok());
      }
      return seconds(t0);
    };
    // Cold.
    double coldRoot = 1e30, coldPath = 1e30;
    {
      QueryContext probe(p.m, p.svc);
      const auto picks = pickSubtrees(probe, 1, 3);
      ASSERT_FALSE(picks.empty());
      Filter fr, fp;
      fr.subtreeRoot = picks[0].root;
      fp.subtreePath = picks[0].path;
      for (int i = 0; i < kReps; ++i) {
        QueryContext a(p.m, p.svc), b(p.m, p.svc);
        coldRoot = std::min(coldRoot, run(a, fr));
        coldPath = std::min(coldPath, run(b, fp));
      }
    }
    // Warm: weights cached by an unfiltered query, then distinct subtrees.
    double warmRoot = 1e30, warmPath = 1e30;
    for (int i = 0; i < kReps; ++i) {
      QueryContext a(p.m, p.svc), b(p.m, p.svc);
      const auto picks = pickSubtrees(a, 6, 3);
      ASSERT_FALSE(picks.empty());
      run(a, {});
      run(b, {});
      double tr = 0, tp = 0;
      for (const SubtreePick& pick : picks) {
        Filter fr, fp;
        fr.subtreeRoot = pick.root;
        fp.subtreePath = pick.path;
        tr += run(a, fr);
        tp += run(b, fp);
      }
      warmRoot = std::min(warmRoot, tr);
      warmPath = std::min(warmPath, tp);
    }
    std::printf(
        "[analysis perf] %s %s subtree (%zu samples): cold root %.4f s, path %.4f s (%.2fx); warm root %.4f "
        "s, path %.4f s (%.2fx)\n",
        name.c_str(), query, samples, coldRoot, coldPath, coldPath / coldRoot, warmRoot, warmPath,
        warmPath / warmRoot);
    EXPECT_LT(coldPath, coldRoot * 1.2 * 2) << "cold " << query;
    EXPECT_LT(warmPath, warmRoot * 1.2 * 2 + 0.005) << "warm " << query;
  }
}

TEST(AggregationPerf, MediumSubtreePathVsSubtreeRoot) {
  runSubtreePathBudget("medium");
}

TEST(AggregationPerf, LargeSubtreePathVsSubtreeRoot) {
  runSubtreePathBudget("large");
}

}  // namespace
}  // namespace tachyon::analysis
