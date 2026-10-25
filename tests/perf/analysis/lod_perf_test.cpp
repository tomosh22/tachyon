// Perf budgets (WP-36, Part 3.9): LOD build time and per-frame query CPU time.
//
//   medium (CI): build, and for 16 rows x (state band + sample strip + flame chart depth 1..32) at 1920 px
//   the
//                worst per-frame CPU over all zooms from the whole trace down to 1 ms windows must stay below
//                the 4 ms frame budget (asserted at 2x).
//   large (local, GTEST_SKIP when tachyon_fixtures_large was not built): build < 15 s and the worst frame
//   over
//                all zooms for 64 rows < 4 ms (both asserted at 2x).
//
// Measured numbers and the LOD memory are printed.
#include "analysis/lod.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <time.h>
#endif

namespace tachyon::analysis {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kFrameBudgetMs = 4.0;
constexpr double kLargeBuildBudgetS = 15.0;
constexpr uint32_t kPixels = 1920;
constexpr uint32_t kMaxFlameDepth = 33;  // depths 1..32

double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

// CPU time of the calling thread in milliseconds (the budget is CPU per frame; this also keeps the numbers
// stable when the machine is shared). Falls back to wall time where there is no per-thread clock.
double threadCpuMs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) * 1e-6;
#endif
  return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

struct Buffers {
  StateBandResult band;
  SampleStripResult strip;
  TimelineResult flame;
};

// One frame: every thread's row. `flameRows` threads (from the first) also get the expanded flame chart.
double frameMs(const TimelineLod& lod, size_t rows, size_t flameRows, uint64_t t0, uint64_t t1, Buffers& buf,
               uint64_t& sink) {
  const double start = threadCpuMs();
  for (size_t t = 0; t < rows; ++t) {
    const ThreadIdx th(static_cast<uint32_t>(t));
    StateBandQuery bq;
    bq.thread = th;
    bq.t0 = t0;
    bq.t1 = t1;
    bq.pixelWidth = kPixels;
    (void)lod.stateBands(bq, buf.band);
    SampleStripQuery sq;
    sq.thread = th;
    sq.t0 = t0;
    sq.t1 = t1;
    sq.pixelWidth = kPixels;
    (void)lod.sampleStrip(sq, buf.strip);
    sink +=
        buf.strip.columns[kPixels / 2].stack.get() + static_cast<uint64_t>(buf.band.columns[7].running * 100);
    if (t < flameRows) {
      TimelineQuery tq;
      tq.thread = th;
      tq.depthBegin = 1;
      tq.depthEnd = kMaxFlameDepth;
      tq.t0 = t0;
      tq.t1 = t1;
      tq.pixelWidth = kPixels;
      (void)lod.timeline(tq, buf.flame);
      sink += buf.flame.spans.size();
    }
  }
  return threadCpuMs() - start;
}

struct Zoom {
  uint64_t t0, t1;
  std::string label;
};

// Whole trace down to 1 ms windows in factors of 2, plus windows whose pixel span is just below and just
// above each stored level width (the switch points between levels and the direct path), at three positions.
std::vector<Zoom> makeZooms(const model::TraceModel& m, const TimelineLod& lod) {
  const uint64_t s = m.header.start_ts, total = m.header.end_ts - m.header.start_ts;
  const uint64_t oneMs = m.header.clock_frequency_hz / 1000;
  std::vector<uint64_t> spans;
  for (uint64_t w = total; w > oneMs; w /= 2) spans.push_back(w);
  spans.push_back(oneMs);
  for (const auto& l : lod.levels()) {
    for (double f : {0.9, 1.0, 1.1}) {
      const uint64_t w = static_cast<uint64_t>(static_cast<double>(l.bucketWidth) * f * kPixels);
      if (w < total && w > oneMs / 2) spans.push_back(w);
    }
  }
  std::vector<Zoom> out;
  for (uint64_t w : spans) {
    for (double pos : {0.0, 0.4, 0.73}) {
      const uint64_t t0 = s + static_cast<uint64_t>(static_cast<double>(total - w) * pos);
      out.push_back({t0, t0 + w,
                     std::to_string(static_cast<double>(w) * 1000.0 /
                                    static_cast<double>(m.header.clock_frequency_hz)) +
                         " ms @" + std::to_string(pos)});
    }
  }
  return out;
}

struct FrameReport {
  double worstMs = 0;
  std::string worstLabel;
};

FrameReport worstFrame(const TimelineLod& lod, const std::vector<Zoom>& zooms, size_t rows, size_t flameRows,
                       int reps) {
  Buffers buf;
  uint64_t sink = 0;
  FrameReport r;
  for (const Zoom& z : zooms) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i)
      best = std::min(best, frameMs(lod, rows, flameRows, z.t0, z.t1, buf, sink));
    if (best > r.worstMs) {
      r.worstMs = best;
      r.worstLabel = z.label;
    }
  }
  if (sink == 42) std::printf(" ");
  return r;
}

void report(const char* name, const model::TraceModel& m, const TimelineLod& lod, double buildSeconds) {
  size_t samples = 0;
  for (const auto& s : m.samplesByThread) samples += s.ts.size();
  const size_t bytes = lod.memoryBytes();
  const auto& lv = lod.levels();
  std::printf(
      "[lod perf] %s: build %.3f s (%u hw threads); %zu threads, %zu samples, base level L%u (%llu ticks, %u "
      "buckets/thread), %zu stored levels\n",
      name, buildSeconds, std::thread::hardware_concurrency(), m.threads.size(), samples, lv.front().level,
      static_cast<unsigned long long>(lv.front().bucketWidth), lv.front().bucketCount, lv.size());
  std::printf(
      "[lod perf] %s: LOD memory %.1f MiB = %.2f B/sample (model sample columns: 16 B/sample), %.1f MiB per "
      "thread\n",
      name, double(bytes) / (1024.0 * 1024.0), double(bytes) / double(samples),
      double(bytes) / (1024.0 * 1024.0) / double(m.threads.size()));
}

TEST(LodPerf, MediumBuildAndFrame) {
  auto loaded =
      model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "medium.tachyon");
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel& m = *loaded;
  ASSERT_TRUE(buildThreadStates(m).ok());

  double bestBuild = 1e30;
  for (int i = 0; i < 3; ++i) {
    const auto t0 = Clock::now();
    auto lod = buildLod(m);
    const double s = seconds(t0, Clock::now());
    ASSERT_TRUE(lod.ok());
    bestBuild = std::min(bestBuild, s);
  }
  auto lod = buildLod(m);
  ASSERT_TRUE(lod.ok());
  report("medium", m, *lod, bestBuild);

  const auto zooms = makeZooms(m, *lod);
  const size_t rows = m.threads.size();
  const FrameReport collapsed = worstFrame(*lod, zooms, rows, 0, 5);
  const FrameReport expanded = worstFrame(*lod, zooms, rows, rows, 5);
  std::printf(
      "[lod perf] medium: worst frame over %zu zooms, %zu rows at %u px: band+strip %.3f ms (%s); "
      "band+strip+flame "
      "depth 1..32 on every row %.3f ms (%s); budget %.1f ms, assert %.1f ms\n",
      zooms.size(), rows, kPixels, collapsed.worstMs, collapsed.worstLabel.c_str(), expanded.worstMs,
      expanded.worstLabel.c_str(), kFrameBudgetMs, kFrameBudgetMs * 2);
  EXPECT_LT(expanded.worstMs, kFrameBudgetMs * 2);
  // Build budget: the large budget scaled by sample count (7.49 M of 299.5 M), asserted at 2x.
  const double buildBudget = kLargeBuildBudgetS * 7.49 / 299.5;
  std::printf("[lod perf] medium: build budget %.3f s (large budget scaled by samples), assert %.3f s\n",
              buildBudget, buildBudget * 2);
  EXPECT_LT(bestBuild, buildBudget * 2);
}

TEST(LodPerf, LargeBuildAndFrame) {
  const auto path = std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "large.tachyon";
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "large fixture not built (target tachyon_fixtures_large)";
  auto loaded = model::TraceModel::load(path);
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel& m = *loaded;
  ASSERT_TRUE(buildThreadStates(m).ok());

  const auto t0 = Clock::now();
  auto lod = buildLod(m);
  const double buildSeconds = seconds(t0, Clock::now());
  ASSERT_TRUE(lod.ok());
  report("large", m, *lod, buildSeconds);
  std::printf("[lod perf] large: build budget %.1f s, assert %.1f s\n", kLargeBuildBudgetS,
              kLargeBuildBudgetS * 2);
  EXPECT_LT(buildSeconds, kLargeBuildBudgetS * 2);

  const auto zooms = makeZooms(m, *lod);
  const size_t rows = m.threads.size();
  const FrameReport collapsed = worstFrame(*lod, zooms, rows, 0, 5);
  const FrameReport some = worstFrame(*lod, zooms, rows, 4, 5);
  const FrameReport all = worstFrame(*lod, zooms, rows, rows, 3);
  std::printf(
      "[lod perf] large: worst frame over %zu zooms, %zu rows at %u px: band+strip %.3f ms (%s); +flame on 4 "
      "rows "
      "%.3f ms (%s); +flame on every row %.3f ms (%s); budget %.1f ms, assert %.1f ms\n",
      zooms.size(), rows, kPixels, collapsed.worstMs, collapsed.worstLabel.c_str(), some.worstMs,
      some.worstLabel.c_str(), all.worstMs, all.worstLabel.c_str(), kFrameBudgetMs, kFrameBudgetMs * 2);
  EXPECT_LT(some.worstMs, kFrameBudgetMs * 2);
}

}  // namespace
}  // namespace tachyon::analysis
