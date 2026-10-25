// Cooperative cancellation: a query returns within 50 ms of its flag being raised (4x under
// AddressSanitizer), on `medium` and on a synthetic 20 M-sample model, in every phase of the queries.
#include "analysis/query.h"

#include "aggregation_test_util.h"
#include "analysis/query_executor.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <random>
#include <thread>

#if defined(__SANITIZE_ADDRESS__)
#define TACHYON_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TACHYON_TEST_ASAN 1
#endif
#endif

namespace tachyon::analysis {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using model::ThreadState;
using testutil::HandModel;

#ifdef TACHYON_TEST_ASAN
constexpr double kBudgetMs = 50.0 * 4;  // allowed slowdown under asan: 4x
#else
constexpr double kBudgetMs = 50.0;
#endif

double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

enum class Q { BottomUp, TopDown, Butterfly };

// Runs `q` on a fresh thread, raises the flag after `delay` and returns {code of the result, latency in ms
// from the flag store to the return}.
struct Outcome {
  ErrorCode code = ErrorCode::Ok;
  double latencyMs = 0;
  bool cancelled() const { return code == ErrorCode::Cancelled; }
};

Outcome runCancelled(const QueryContext& ctx, Q q, const Filter& filter, FunctionKey key,
                     std::chrono::microseconds delay) {
  std::atomic<bool> flag{false};
  std::atomic<int> codeOut{0};
  std::atomic<int64_t> endNs{0};
  std::thread worker([&] {
    ErrorCode code = ErrorCode::Ok;
    switch (q) {
      case Q::BottomUp: {
        auto r = bottomUpFunctions(ctx, filter, Cancel{&flag});
        if (!r) code = r.error().code;
        break;
      }
      case Q::TopDown: {
        auto r = topDownTree(ctx, filter, Cancel{&flag});
        if (!r) code = r.error().code;
        break;
      }
      case Q::Butterfly: {
        auto r = butterfly(ctx, filter, key, Cancel{&flag});
        if (!r) code = r.error().code;
        break;
      }
    }
    endNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    codeOut = static_cast<int>(code);
  });
  std::this_thread::sleep_for(delay);
  const auto raised = Clock::now();
  flag.store(true);
  worker.join();
  Outcome o;
  o.code = static_cast<ErrorCode>(codeOut.load());
  const auto end = Clock::time_point(std::chrono::nanoseconds(endNs.load()));
  o.latencyMs = std::max(0.0, ms(raised, end));
  return o;
}

// Sweeps the cancel delay over every phase of the queries.
void sweep(QueryContext& ctx, const char* label, FunctionKey someKey) {
#ifdef TACHYON_TEST_ASAN
  const std::chrono::microseconds delays[] = {500us, 5ms, 40ms};  // asan: every run is several times slower
#else
  const std::chrono::microseconds delays[] = {0us, 500us, 2ms, 5ms, 10ms, 20ms, 40ms};
#endif
  double worst = 0;
  int cancelled = 0;
  auto check = [&](const Outcome& o, Q q, bool warm, std::chrono::microseconds d) {
    if (o.cancelled()) {
      ++cancelled;
      worst = std::max(worst, o.latencyMs);
      EXPECT_LT(o.latencyMs, kBudgetMs) << label << " query " << static_cast<int>(q)
                                        << (warm ? " (warm)" : " (cold)") << " delay " << d.count() << "us";
    } else {
      EXPECT_EQ(o.code, ErrorCode::Ok);  // finished before the flag: fine, nothing to measure
    }
  };
  for (Q q : {Q::BottomUp, Q::TopDown, Q::Butterfly}) {
    // Cold: cancelled during the sample scan (and, for the slowest delays, during the aggregation).
    for (auto d : delays) {
      ctx.clearCache();
      check(runCancelled(ctx, q, Filter{}, someKey, d), q, false, d);
    }
    // Warm: the weight table is cached (computed by another query kind), so the cancelled query spends its
    // time in the flatten / aggregate / materialise loops. A cancelled query caches nothing, so the table
    // survives the sweep; if a query completes instead, start over.
    auto rewarm = [&] {
      ctx.clearCache();
      auto warmUp = q == Q::BottomUp ? topDownTree(ctx, Filter{}) : Result<NodeAgg>(NodeAgg{});
      if (q != Q::BottomUp) ASSERT_TRUE(bottomUpFunctions(ctx, Filter{}).ok());
      ASSERT_TRUE(warmUp.ok());
    };
    rewarm();
    for (auto d : delays) {
      const Outcome o = runCancelled(ctx, q, Filter{}, someKey, d);
      check(o, q, true, d);
      if (!o.cancelled()) rewarm();
    }
  }
  std::printf("[cancellation] %s: %d cancelled runs, worst latency %.2f ms (budget %.0f ms)\n", label,
              cancelled, worst, kBudgetMs);
  EXPECT_GT(cancelled, 0);
}

TEST(CancellationTest, MediumFixtureReturnsWithinBudget) {
  const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
  auto r = model::TraceModel::load(dir / "medium.tachyon");
  ASSERT_TRUE(r.ok());
  model::TraceModel m = std::move(*r);
  ASSERT_TRUE(buildThreadStates(m).ok());
  symbols::SymbolService svc(std::vector<std::string>{}, std::filesystem::path{});
  ASSERT_TRUE(svc.addSymbolFileOverride(dir / "medium.symbols.json").ok());
  svc.resolveAllAsync(m);
  svc.waitAll();
  QueryContext ctx(m, svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  sweep(ctx, "medium", (*rows)[0].key);
}

// 16 threads x 1.25 M samples = 20 M samples over a 300 000-node call tree, all Running.
struct Synthetic {
  static constexpr uint32_t kThreads = 16;
  static constexpr size_t kPerThread = 1'250'000;
  static constexpr size_t kNodes = 300'000;
  HandModel h{kThreads};
  Synthetic() {
    std::mt19937_64 rng(5);
    for (size_t i = 1; i < kNodes; ++i) {
      const size_t lo = i > 1500 ? i - 1500 : 0;
      const CallstackIdx parent(static_cast<uint32_t>(lo + rng() % (i - lo)));
      h.pushFrame(parent, ModuleIdx(0), 0x1000 + (rng() % 20000) * 16);
    }
    for (uint32_t t = 0; t < kThreads; ++t) {
      h.state(t, 0, kPerThread * 100 + 1000, ThreadState::Running);
      auto& c = h.m.samplesByThread[t];
      c.ts.resize(kPerThread);
      c.callstack.resize(kPerThread);
      c.cpu.assign(kPerThread, 0);
      c.flags.assign(kPerThread, 0);
      for (size_t i = 0; i < kPerThread; ++i) {
        c.ts[i] = i * 100;
        c.callstack[i] = CallstackIdx(static_cast<uint32_t>(rng() % h.m.nodes.size()));
      }
    }
    h.m.header.end_ts = kPerThread * 100 + 1000;
    h.finish();
  }
};

// Built once for the three tests that use it (a few seconds, more under sanitizers).
Synthetic& synthetic() {
  static Synthetic s;
  return s;
}

TEST(CancellationTest, TwentyMillionSamplesReturnWithinBudget) {
  Synthetic& s = synthetic();
  size_t total = 0;
  for (const auto& c : s.h.m.samplesByThread) total += c.ts.size();
  ASSERT_EQ(total, 20'000'000u);
  QueryContext ctx(s.h.m, *s.h.svc);
  sweep(ctx, "20M samples", FunctionKey{ModuleIdx(0), 0x1000 + 7 * 16});
}

TEST(CancellationTest, ExecutorCancelReturnsWithinBudget) {
  Synthetic& s = synthetic();
  QueryExecutor::Options o;
  o.threads = 4;
  QueryExecutor ex(o);
  QueryContext ctx(s.h.m, *s.h.svc);
  ctx.setExecutor(&ex);
  double worst = 0;
  int cancelled = 0;
  for (auto delay : {1ms, 5ms, 15ms, 30ms}) {
    ctx.clearCache();
    auto f = ex.submitQuery<NodeAgg>(SubmitOptions{}, [&](Cancel c) { return topDownTree(ctx, {}, c); });
    std::this_thread::sleep_for(delay);
    const auto t0 = Clock::now();
    ex.cancel(f.handle());
    f.handle().wait();
    const double lat = ms(t0, Clock::now());
    if (!f.get().ok()) {
      EXPECT_EQ(f.get().error().code, ErrorCode::Cancelled);
      ++cancelled;
      worst = std::max(worst, lat);
      EXPECT_LT(lat, kBudgetMs) << "delay " << delay.count() << "ms";
    }
  }
  ex.waitIdle();
  std::printf("[cancellation] executor, 20M samples: %d cancelled runs, worst latency %.2f ms\n", cancelled,
              worst);
  EXPECT_GT(cancelled, 0);
}

TEST(CancellationTest, CancelledScanLeavesTheContextUsable) {
  Synthetic& s = synthetic();
  QueryContext ctx(s.h.m, *s.h.svc);
  const Outcome o = runCancelled(ctx, Q::BottomUp, Filter{}, {}, 3ms);
  EXPECT_TRUE(o.cancelled());
  EXPECT_EQ(ctx.cacheStats().entries, 0u);
  Filter small;
  small.t1 = 100000;
  EXPECT_TRUE(bottomUpFunctions(ctx, small).ok());
}

}  // namespace
}  // namespace tachyon::analysis
