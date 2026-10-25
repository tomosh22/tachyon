// WP-53b: cancellation latency of the cold fiber-aware path (pairing by fiber, pieces, blend index) on a
// model with 1 M scopes split by fiber switches: at most 50 ms after the flag (200 ms under
// AddressSanitizer).
#include "aggregation_test_util.h"
#include "analysis/fibers.h"
#include "analysis/query.h"
#include "scope_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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
using testutil::ScopeBuilder;

// Budget 50 ms (200 ms under asan); asserted at 2x like the perf budgets, so a loaded runner does not flake.
#ifdef TACHYON_TEST_ASAN
constexpr double kBudgetMs = 2 * 200.0;
#else
constexpr double kBudgetMs = 2 * 50.0;
#endif

// 4 threads x 250 000 scopes, two fibers per thread switching every 4 scopes: 1 M scopes in 8 big lanes.
struct FiberModel {
  HandModel h{4};
  ScopeBuilder sc{h};
  FiberModel() {
    h.m.header.end_ts = 400000000;
    for (uint32_t t = 0; t < 4; ++t) {
      h.state(t, 0, h.m.header.end_ts, ThreadState::Running);
      uint64_t cur = 1 + 2 * t;
      sc.fiber(0, t, trace::kFiberSwitchEnd, cur);
      for (uint32_t i = 0; i < 250000; ++i) {
        const uint64_t ts = 100 + 100ull * i;
        sc.begin(ts, "S", 0x1000 + 0x10 * (i % 7), 100 + t);
        sc.end(ts + 40, 100 + t);
        if (i % 4 == 3) {
          const uint64_t next = cur % 2 == 1 ? cur + 1 : cur - 1;
          sc.fiber(ts + 50, t, trace::kFiberSwitchBegin, cur, next);
          sc.fiber(ts + 60, t, trace::kFiberSwitchEnd, next, cur);
          cur = next;
        }
      }
      h.sample(t, 150, h.stack({0x1010}));
    }
    sc.sort();
    h.finish();
  }
};

TEST(FiberCancel, ColdFiberAwareBuildStopsWithinTheBudget) {
  FiberModel f;
  ASSERT_EQ(f.h.m.scopeEvents.size(), 2'000'000u);
  AggregationOptions o;
  o.blendScopes = true;
  o.groupByFiber = true;
  int cancelled = 0;
  double worst = 0;
  for (const auto delay : {1ms, 10ms, 25ms, 45ms, 70ms, 100ms, 140ms, 200ms}) {
    QueryContext ctx(f.h.m, *f.h.svc, o);  // cold: nothing cached
    std::atomic<bool> flag{false};
    std::atomic<int64_t> endNs{0};
    std::atomic<int> code{0};
    std::thread worker([&] {
      auto r = bottomUpFunctions(ctx, {}, Cancel{&flag});
      code = r.ok() ? 0 : static_cast<int>(r.error().code);
      endNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    });
    std::this_thread::sleep_for(delay);
    const auto raised = Clock::now();
    flag = true;
    worker.join();
    if (code.load() != static_cast<int>(ErrorCode::Cancelled)) continue;  // finished before the flag
    ++cancelled;
    const double ms = std::max(0.0, std::chrono::duration<double, std::milli>(
                                        Clock::time_point(std::chrono::nanoseconds(endNs.load())) - raised)
                                        .count());
    worst = std::max(worst, ms);
    EXPECT_LT(ms, kBudgetMs) << "cancel after " << delay.count() << " ms";
  }
  std::printf("[fiber cancel] %d cancelled runs, worst latency %.2f ms (budget %.0f ms)\n", cancelled, worst,
              kBudgetMs);
  EXPECT_GE(cancelled, 3);
}

TEST(FiberCancel, CancelledPairingLeavesNothingBehindAndCanBeRepeated) {
  FiberModel f;
  std::atomic<bool> flag{true};
  auto r = pairScopesByFiber(f.h.m, Cancel{&flag});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
  flag = false;
  auto ok = pairScopesByFiber(f.h.m, Cancel{&flag});
  ASSERT_TRUE(ok.ok());
  size_t pieces = 0;
  for (const FiberLane& l : ok->lanes) pieces += l.pieces.size();
  EXPECT_EQ(pieces, 1'000'000u);
}

}  // namespace
}  // namespace tachyon::analysis
