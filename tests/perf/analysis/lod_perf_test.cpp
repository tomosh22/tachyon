// Perf budgets (WP-36, WP-36b, Part 3.9): LOD build time and per-frame query CPU time.
//
// A frame is one band + strip (+ optionally the flame chart, depth 1..32) query per row at 1920 px. Its cost
// is the CPU time of the calling thread (testutil::currentThreadCpuNs, so a loaded machine does not matter);
// per zoom the best of 5 repetitions is taken and the worst zoom is asserted at 2x the 4 ms budget. Wall time
// is printed too.
//   medium (CI): build, and the 16-row frame with and without the flame chart on every row.
//   large (local, GTEST_SKIP when tachyon_fixtures_large was not built): build < 15 s and, for 64 rows, the
//                band+strip frame and the frame with the flame chart on 4 rows, both over all zooms of
//                makeZooms (the whole trace down to 1 ms plus the zooms just below and above every level
//                width).
//
// Measured numbers and the LOD memory are printed.
#include "analysis/lod.h"
#include "analysis/thread_states.h"
#include "thread_cpu_time.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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
// stable when the machine is shared). The helper is shared with the cancellation tests (WP-QA1).
double threadCpuMs() {
  return static_cast<double>(testutil::currentThreadCpuNs()) * 1e-6;
}

struct Buffers {
  StateBandResult band;
  SampleStripResult strip;
  TimelineResult flame;
};

// One frame: every thread's row. `flameRows` threads (from the first) also get the expanded flame chart.
// `parts` selects what is drawn per row: bit 0 state band, bit 1 sample strip (default both).
constexpr unsigned kBand = 1, kStrip = 2, kBandStrip = 3;
double frameMs(const TimelineLod& lod, size_t rows, size_t flameRows, uint64_t t0, uint64_t t1, Buffers& buf,
               uint64_t& sink, unsigned parts = kBandStrip) {
  const double start = threadCpuMs();
  for (size_t t = 0; t < rows; ++t) {
    const ThreadIdx th(static_cast<uint32_t>(t));
    StateBandQuery bq;
    bq.thread = th;
    bq.t0 = t0;
    bq.t1 = t1;
    bq.pixelWidth = kPixels;
    if (parts & kBand) {
      (void)lod.stateBands(bq, buf.band);
      sink += static_cast<uint64_t>(buf.band.columns[7].running * 100);
    }
    SampleStripQuery sq;
    sq.thread = th;
    sq.t0 = t0;
    sq.t1 = t1;
    sq.pixelWidth = kPixels;
    if (parts & kStrip) {
      (void)lod.sampleStrip(sq, buf.strip);
      sink += buf.strip.columns[kPixels / 2].stack.get();
    }
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
  double worstMs = 0;      // worst zoom's best-of-reps thread CPU time
  double worstWallMs = 0;  // wall time of that same repetition
  std::string worstLabel;
  std::vector<std::pair<double, std::string>> perZoom;  // best-of-reps CPU ms and label of every zoom
};

// The `n` slowest zooms as "ms (label); ..." for the log.
std::string slowest(const FrameReport& r, size_t n) {
  auto v = r.perZoom;
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string out;
  for (size_t i = 0; i < std::min(n, v.size()); ++i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v[i].first);
    out += std::string(i ? "; " : "") + buf + " (" + v[i].second + ")";
  }
  return out;
}

FrameReport worstFrame(const TimelineLod& lod, const std::vector<Zoom>& zooms, size_t rows, size_t flameRows,
                       int reps, unsigned parts = kBandStrip) {
  Buffers buf;
  uint64_t sink = 0;
  FrameReport r;
  for (const Zoom& z : zooms) {
    double best = 1e30, bestWall = 0;
    for (int i = 0; i < reps; ++i) {
      const auto w0 = Clock::now();
      const double cpu = frameMs(lod, rows, flameRows, z.t0, z.t1, buf, sink, parts);
      const double wall = seconds(w0, Clock::now()) * 1e3;
      if (cpu < best) {
        best = cpu;
        bestWall = wall;
      }
    }
    r.perZoom.emplace_back(best, z.label);
    if (best > r.worstMs) {
      r.worstMs = best;
      r.worstWallMs = bestWall;
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
      "[lod perf] medium: worst frame (best of 5 reps, thread CPU ms / wall ms) over %zu zooms, %zu rows at "
      "%u "
      "px: band+strip %.3f / %.3f (%s); band+strip+flame depth 1..32 on every row %.3f / %.3f (%s); budget "
      "%.1f ms, assert %.1f ms\n",
      zooms.size(), rows, kPixels, collapsed.worstMs, collapsed.worstWallMs, collapsed.worstLabel.c_str(),
      expanded.worstMs, expanded.worstWallMs, expanded.worstLabel.c_str(), kFrameBudgetMs,
      kFrameBudgetMs * 2);
  EXPECT_LT(collapsed.worstMs, kFrameBudgetMs * 2);
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
  {
    LodOptions one;
    one.threads = 1;
    const auto s0 = Clock::now();
    auto single = buildLod(m, one);
    ASSERT_TRUE(single.ok());
    std::printf("[lod perf] large: build with 1 worker %.3f s (informational)\n", seconds(s0, Clock::now()));
    EXPECT_EQ(single->contentHash(), lod->contentHash());
  }

  const auto zooms = makeZooms(m, *lod);
  const size_t rows = m.threads.size();
  // Best of 5 repetitions per zoom, measured in calling-thread CPU time (robust against a loaded machine).
  const FrameReport collapsed = worstFrame(*lod, zooms, rows, 0, 5);
  const FrameReport some = worstFrame(*lod, zooms, rows, 4, 5);
  const FrameReport all = worstFrame(*lod, zooms, rows, rows, 5);
  std::printf(
      "[lod perf] large: worst frame (best of 5 reps, thread CPU ms / wall ms) over %zu zooms, %zu rows at "
      "%u "
      "px: band+strip %.3f / %.3f (%s); +flame on 4 rows %.3f / %.3f (%s); +flame on every row %.3f / %.3f "
      "(%s); budget %.1f ms, assert %.1f ms\n",
      zooms.size(), rows, kPixels, collapsed.worstMs, collapsed.worstWallMs, collapsed.worstLabel.c_str(),
      some.worstMs, some.worstWallMs, some.worstLabel.c_str(), all.worstMs, all.worstWallMs,
      all.worstLabel.c_str(), kFrameBudgetMs, kFrameBudgetMs * 2);
  std::printf("[lod perf] large: slowest band+strip zooms (thread CPU ms): %s\n",
              slowest(collapsed, 6).c_str());
  // Where the band+strip time goes at its worst zoom (informational).
  {
    const FrameReport b = worstFrame(*lod, zooms, rows, 0, 5, kBand);
    const FrameReport st = worstFrame(*lod, zooms, rows, 0, 5, kStrip);
    std::printf("[lod perf] large: worst frame band only %.3f ms (%s); strip only %.3f ms (%s)\n", b.worstMs,
                b.worstLabel.c_str(), st.worstMs, st.worstLabel.c_str());
  }
  EXPECT_LT(collapsed.worstMs, kFrameBudgetMs * 2);
  EXPECT_LT(some.worstMs, kFrameBudgetMs * 2);
}

}  // namespace
}  // namespace tachyon::analysis
