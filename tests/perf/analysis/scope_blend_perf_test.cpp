// Perf budgets (WP-53). The blended bottomUpFunctions (scopes spliced into every sample, index warm) must
// stay within 3x of the unblended one on a synthetic model with 1 M scopes and 5 M samples (asserted at 2x of
// the budget, like the other perf tests); the cold first query (pairing + index build + query) has an
// absolute budget of 3 s (asserted at 6 s). A realistic mix is used: every scope template samples a handful
// of stacks, so the number of distinct (stack, scope stack) pairs stays in the tens of thousands. Measured
// numbers are printed.
#include "aggregation_test_util.h"
#include "analysis/fibers.h"
#include "analysis/query.h"
#include "analysis/scope_blend.h"
#include "analysis/thread_states.h"
#include "scope_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace tachyon::analysis {
namespace {

using Clock = std::chrono::steady_clock;
using model::ThreadState;
using testutil::HandModel;
using testutil::ScopeBuilder;
using testutil::TestFunction;

double seconds(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

struct Rng {
  uint64_t s;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

// 8 threads, 41 667 groups of 3 nested scopes and 15 samples each: 1 M scopes, 5 M samples.
struct Synthetic {
  static constexpr uint32_t kThreads = 8, kGroups = 41667, kSamplesPerGroup = 15, kGroupTicks = 1500;
  static constexpr uint32_t kFunctions = 200, kStacks = 2000, kTemplates = 300, kScopeIds = 100;
  HandModel h{kThreads};
  ScopeBuilder sc{h};
  std::vector<std::string> names;
  std::vector<TestFunction> functions;

  Synthetic() {
    Rng rng{0x9E3779B97F4A7C15ull};
    h.m.header.end_ts = uint64_t{kGroups} * kGroupTicks + 1000;
    names.reserve(kFunctions);
    for (uint32_t i = 0; i < kFunctions; ++i) {
      names.push_back("fn" + std::to_string(i));
      functions.push_back(TestFunction{names.back().c_str(), 0x1000 + uint64_t{i} * 0x400, 0x400});
    }
    std::vector<CallstackIdx> stacks;
    for (uint32_t i = 0; i < kStacks; ++i) {
      CallstackIdx cur(0);
      const uint32_t depth = 6 + rng.below(9);
      for (uint32_t d = 0; d < depth; ++d)
        cur = h.push(cur, functions[rng.below(kFunctions)].start + 8 * rng.below(100));
      stacks.push_back(cur);
    }
    std::vector<std::string> ids;
    for (uint32_t i = 0; i < kScopeIds; ++i) ids.push_back("Scope" + std::to_string(i));
    struct Template {
      uint32_t id[3];
      uint32_t stack[10];
    };
    std::vector<Template> templates(kTemplates);
    for (Template& t : templates) {
      for (uint32_t& v : t.id) v = rng.below(kScopeIds);
      for (uint32_t& v : t.stack) v = rng.below(kStacks);
    }
    auto caller = [&](uint32_t id) { return functions[(id * 7) % kFunctions].start + 0x10; };
    for (uint32_t t = 0; t < kThreads; ++t) {
      h.state(t, 0, h.m.header.end_ts, ThreadState::Running);
      const uint32_t tid = 100 + t;
      for (uint32_t g = 0; g < kGroups; ++g) {
        const Template& tp = templates[rng.below(kTemplates)];
        const uint64_t base = uint64_t{g} * kGroupTicks;
        sc.begin(base, ids[tp.id[0]].c_str(), caller(tp.id[0]), tid);
        sc.begin(base + 100, ids[tp.id[1]].c_str(), caller(tp.id[1]), tid);
        sc.begin(base + 300, ids[tp.id[2]].c_str(), caller(tp.id[2]), tid);
        sc.end(base + 700, tid);
        sc.end(base + 1000, tid);
        sc.end(base + 1200, tid);
        for (uint32_t s = 0; s < kSamplesPerGroup; ++s)
          h.sample(t, base + 100 * s + 50, stacks[tp.stack[rng.below(10)]]);
      }
    }
    sc.sort();
    h.finish(functions);
  }
};

double bestOf(int reps, const std::function<void(int)>& fn) {
  double best = 1e30;
  for (int i = 0; i < reps; ++i) {
    const auto t0 = Clock::now();
    fn(i);
    best = std::min(best, seconds(t0));
  }
  return best;
}

TEST(ScopeBlendPerf, BlendedBottomUpWithinThreeTimesTheUnblendedOne) {
  Synthetic s;
  size_t samples = 0;
  for (const auto& c : s.h.m.samplesByThread) samples += c.ts.size();
  ASSERT_EQ(samples, size_t{Synthetic::kThreads} * Synthetic::kGroups * Synthetic::kSamplesPerGroup);
  ASSERT_EQ(s.h.m.scopeEvents.size(), size_t{Synthetic::kThreads} * Synthetic::kGroups * 6);

  AggregationOptions on;
  on.blendScopes = true;
  QueryContext blended(s.h.m, *s.h.svc, on);
  QueryContext plain(s.h.m, *s.h.svc);

  // Distinct filters per repetition defeat the result cache but keep the blend index (not filter dependent).
  auto filterFor = [&](int i) {
    Filter f;
    f.t1 = s.h.m.header.end_ts + 1 + static_cast<uint64_t>(i);
    return f;
  };
  const double unblended = bestOf(9, [&](int i) {
    auto r = bottomUpFunctions(plain, filterFor(i));
    ASSERT_TRUE(r.ok()) << r.error().toString();
  });

  const auto cold0 = Clock::now();
  auto first = bottomUpFunctions(blended, filterFor(100));
  const double cold = seconds(cold0);
  ASSERT_TRUE(first.ok()) << first.error().toString();
  size_t scopeRows = 0;
  for (const FunctionRow& r : *first) scopeRows += r.key.isScope() ? 1 : 0;
  EXPECT_GT(scopeRows, 50u);

  const double warm = bestOf(9, [&](int i) {
    auto r = bottomUpFunctions(blended, filterFor(i));
    ASSERT_TRUE(r.ok());
  });
  auto idx = blendIndexFor(blended);
  ASSERT_TRUE(idx.ok());
  std::printf(
      "[scope blend perf] %zu samples, %zu scopes, %llu trie nodes, %llu segments, %u hw threads\n"
      "[scope blend perf] unblended bottomUp best %.4f s; blended cold (pair + index + query) %.4f s; "
      "blended "
      "warm best %.4f s; ratio warm/unblended %.2f (budget 3.0, assert 6.0)\n",
      samples, size_t{Synthetic::kThreads} * Synthetic::kGroups * 3,
      static_cast<unsigned long long>((*idx)->nodes.size()),
      static_cast<unsigned long long>((*idx)->segmentCount), std::thread::hardware_concurrency(), unblended,
      cold, warm, warm / unblended);
  // The unblended query is a dense array update per sample (about 2 ns); below kFloor, timer and scheduler
  // noise of the shared CI machines dominates the ratio, so the budget is 3x max(unblended, kFloor).
  constexpr double kFloor = 0.015;
  EXPECT_LT(warm, 3.0 * std::max(unblended, kFloor) * 2) << "blended bottomUp over 2x its 3x budget";
  EXPECT_LT(cold, 3.0 * 2) << "cold blended query over 2x its 3 s budget";

  // topDownTree and butterfly through the same flattener: informational, with a loose bound.
  auto t0 = Clock::now();
  auto tree = topDownTree(blended, filterFor(7));
  const double treeS = seconds(t0);
  ASSERT_TRUE(tree.ok());
  t0 = Clock::now();
  auto tree0 = topDownTree(plain, filterFor(7));
  const double treeP = seconds(t0);
  ASSERT_TRUE(tree0.ok());
  std::printf(
      "[scope blend perf] topDownTree blended %.4f s, unblended %.4f s; blended nodes counted by root %llu "
      "ticks\n",
      treeS, treeP, static_cast<unsigned long long>(tree->inclusiveTicks));
  EXPECT_EQ(tree->inclusiveTicks, tree0->inclusiveTicks);
}

TEST(ScopeBlendPerf, ScopesFixtureBlendedVersusUnblended) {
  const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
  if (!std::filesystem::exists(dir / "scopes.tachyon")) GTEST_SKIP() << "scopes fixture not generated";
  auto r = model::TraceModel::load(dir / "scopes.tachyon");
  ASSERT_TRUE(r.ok());
  model::TraceModel m = std::move(*r);
  ASSERT_TRUE(buildThreadStates(m).ok());
  symbols::SymbolService svc{std::vector<std::string>{}, std::filesystem::path{}};
  ASSERT_TRUE(svc.addSymbolFileOverride(dir / "scopes.symbols.json").ok());
  svc.resolveAllAsync(m);
  svc.waitAll();
  AggregationOptions on;
  on.blendScopes = true;
  QueryContext blended(m, svc, on);
  QueryContext plain(m, svc);
  auto t0 = Clock::now();
  ASSERT_TRUE(bottomUpFunctions(blended, {}).ok());
  const double cold = seconds(t0);
  auto filterFor = [&](int i) {
    Filter f;
    f.t1 = m.header.end_ts + 1 + static_cast<uint64_t>(i);
    return f;
  };
  const double warm = bestOf(9, [&](int i) { ASSERT_TRUE(bottomUpFunctions(blended, filterFor(i)).ok()); });
  const double base = bestOf(9, [&](int i) { ASSERT_TRUE(bottomUpFunctions(plain, filterFor(i)).ok()); });
  size_t samples = 0;
  for (const auto& c : m.samplesByThread) samples += c.ts.size();
  std::printf(
      "[scope blend perf] scopes fixture (%zu samples, %zu scope events): unblended %.5f s, blended cold "
      "%.5f "
      "s, blended warm %.5f s (ratio %.2f)\n",
      samples, m.scopeEvents.size(), base, cold, warm, warm / std::max(base, 1e-9));
  EXPECT_LT(warm, 0.05);
}

TEST(ScopeBlendPerf, PairingAndIndexBuildOfOneMillionScopes) {
  Synthetic s;
  AggregationOptions on;
  on.blendScopes = true;
  QueryContext ctx(s.h.m, *s.h.svc, on);
  auto t0 = Clock::now();
  auto paired = pairScopes(s.h.m);
  const double pairS = seconds(t0);
  ASSERT_TRUE(paired.ok());
  t0 = Clock::now();
  auto idx = buildBlendIndex(s.h.m, *paired, nullptr);
  const double indexS = seconds(t0);
  ASSERT_TRUE(idx.ok());
  t0 = Clock::now();
  auto fibers = pairScopesByFiber(s.h.m);
  const double fiberS = seconds(t0);
  ASSERT_TRUE(fibers.ok());
  std::printf(
      "[scope blend perf] pairScopes %.4f s, buildBlendIndex %.4f s (%llu segments, %zu trie nodes), "
      "pairScopesByFiber without fiber events %.4f s\n",
      pairS, indexS, static_cast<unsigned long long>(idx->segmentCount), idx->nodes.size(), fiberS);
  EXPECT_LT(pairS, 1.0);
  EXPECT_LT(indexS, 1.0);
}

TEST(ScopeBlendPerf, FibersWithManySwitchesPairQuickly) {
  // 4 fibers on 2 threads switching every 1000 ticks for 250 000 switches, 1 scope pair per fiber stay.
  HandModel h{2};
  ScopeBuilder sc(h);
  h.m.header.end_ts = 400000000;
  const uint32_t switches = 250000;
  for (uint32_t t = 0; t < 2; ++t) h.state(t, 0, h.m.header.end_ts, ThreadState::Running);
  for (uint32_t t = 0; t < 2; ++t) sc.fiber(0, t, trace::kFiberSwitchEnd, 1 + 2 * t);
  uint64_t ts = 1000;
  for (uint32_t i = 0; i < switches; ++i, ts += 1000) {
    const uint32_t t = i % 2;
    const uint64_t cur = 1 + 2 * t + (i / 2) % 2, next = 1 + 2 * t + (1 - (i / 2) % 2);
    sc.begin(ts - 500, "Work", 0x1000, 100 + t);
    sc.end(ts - 100, 100 + t);
    sc.fiber(ts, t, trace::kFiberSwitchBegin, cur, next);
    sc.fiber(ts + 10, t, trace::kFiberSwitchEnd, next, cur);
  }
  sc.sort();
  h.finish();
  const auto t0 = Clock::now();
  auto r = pairScopesByFiber(h.m);
  const double s = seconds(t0);
  ASSERT_TRUE(r.ok());
  size_t pieces = 0;
  for (const FiberLane& l : r->lanes) pieces += l.pieces.size();
  std::printf("[scope blend perf] pairScopesByFiber: %zu switches, %zu scopes, %zu pieces: %.4f s\n",
              size_t{switches} * 2, size_t{switches}, pieces, s);
  EXPECT_EQ(pieces, size_t{switches});
  EXPECT_LT(s, 1.0);
}

}  // namespace
}  // namespace tachyon::analysis
