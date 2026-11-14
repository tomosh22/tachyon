// Perf budget (WP-65b), asserted at 2x: while a Live document refreshes twice a second, the UI thread's cost
// per frame stays small because the snapshot, the thread states, the timeline index and the symbol service
// are all built on the snapshot worker; the UI thread only swaps the document.
//   * budget: p95 of the CPU time of one App::frame() < 2 ms on a medium-sized window (5 s of the `medium`
//     fixture: 16 threads at 8 kHz, about 640 000 samples), with the timeline, Call Graph, Function List and
//     the other default panels open; asserted at 2x (4 ms);
//   * the swap frames (the ones that replace the document) are reported separately.
// The mock backend replays the fixture in real time through the real agent code (an in-process Agent). The
// numbers are printed.
#include "../../unit/viewer/live_test_util.h"

#if defined(__linux__)
#include <time.h>
#endif

#include <algorithm>
#include <cstdio>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::live_test;

namespace {

constexpr double kFrameBudgetMs = 2.0;
#if defined(__linux__)
constexpr bool kHaveThreadClock = true;
#else
constexpr bool kHaveThreadClock = false;
#endif

// CPU time of the calling thread (Linux); elsewhere wall time, which only over-estimates.
double threadCpuMs() {
#if defined(__linux__)
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
#else
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

}  // namespace

// Runs a live replay of the `medium` fixture at `speed` with a window of `windowSeconds`, measures the frames
// from `measureFrom` to `total` wall seconds and prints / asserts them.
static void measureFrames(const char* label, uint32_t windowSeconds, double speed, double measureFrom,
                          double total) {
  AppOptions options = liveOptions(500, windowSeconds);
  LiveRig rig("live_perf", "medium.tachyon", speed, std::move(options));
  // The default panel set, like a real session (the rig closed all but the timeline).
  for (PanelEntry& e : rig.app().panels().entries()) {
    const std::string name = e.panel->name();
    e.open = name == "Threads Timeline" || name == "Call Graph" || name == "Function List" ||
             name == "Butterfly" || name == "Trace Info" || name == kLivePanelName;
  }
  ASSERT_TRUE(rig.startLive());
  ASSERT_TRUE(rig.h.until([&] { return rig.liveDocument(); }, 60));

  using Clock = std::chrono::steady_clock;
  const auto began = Clock::now();
  std::vector<double> all, swap, steady;
  uint64_t lastSwaps = rig.app().liveSwaps();
  while (true) {
    const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    if (elapsed > total) break;
    const double t0 = threadCpuMs();
    rig.app().frame(0);
    const double cost = threadCpuMs() - t0;
    const uint64_t swaps = rig.app().liveSwaps();
    if (elapsed >= measureFrom) {  // the window is full
      all.push_back(cost);
      (swaps != lastSwaps ? swap : steady).push_back(cost);
    }
    lastSwaps = swaps;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60 fps pacing
  }
  const LiveSessionStats stats = rig.app().live().stats();
  const double p50 = percentile(all, 0.5), p95 = percentile(all, 0.95), p99 = percentile(all, 0.99),
               pmax = percentile(all, 1.0);
  std::printf(
      "LIVE-PERF %s window=%us frames=%zu swap_frames=%zu frame_cpu_ms p50=%.3f p95=%.3f p99=%.3f max=%.3f "
      "swap_frame p50=%.3f max=%.3f steady_frame p95=%.3f worker: snapshot=%.0f states=%.0f lod=%.0f "
      "symbols=%.0f total=%.0f(max %.0f) samples_in_window=%llu\n",
      label, windowSeconds, all.size(), swap.size(), p50, p95, p99, pmax, percentile(swap, 0.5),
      percentile(swap, 1.0), percentile(steady, 0.95), stats.lastSnapshotMs, stats.lastStatesMs,
      stats.lastLodMs, stats.lastSymbolsMs, stats.lastTotalMs, stats.maxTotalMs,
      static_cast<unsigned long long>(rig.app().liveUiState().samples));
  ASSERT_GT(all.size(), 100u);
  EXPECT_GE(swap.size(), 5u) << "the document was refreshed during the measurement";
  EXPECT_LT(p95, 2.0 * kFrameBudgetMs)
      << "p95 CPU per frame (budget " << kFrameBudgetMs << " ms, asserted at 2x)";
}

TEST(LivePerf, FrameCostWithAMediumWindow) {
  if (!kHaveThreadClock)
    GTEST_SKIP() << "the budget is in CPU time of the UI thread (measured with the Linux thread clock)";
  // 5 s of the medium fixture: 16 threads at 8 kHz, about 640 000 samples in the window.
  measureFrames("medium", 5, 1.0, 6.0, 20.0);
}

TEST(LivePerf, FrameCostWithTheFullThirtySecondWindow) {
  if (!kHaveThreadClock)
    GTEST_SKIP() << "the budget is in CPU time of the UI thread (measured with the Linux thread clock)";
  // The specified window: 30 s x 16 threads x 8 kHz, about 3.8 million samples. The replay runs 3x as fast as
  // real time so that the window fills within 10 s.
  measureFrames("full", 30, 3.0, 12.0, 19.0);
}
