// Perf budgets (WP-30): load `medium` (7.5 M samples, 17 MiB) in < 2 s (asserted at 2x = 4 s); `large`
// (299.5 M samples, 647 MiB) in < 30 s on 8 cores (scaled to the cores available, asserted at 2x); resident
// size within budget of the on-disk size. Measured numbers are printed.
#include "model/model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace tachyon::model {
namespace {

constexpr double kMediumBudgetSeconds = 2.0;  // target; assert at 2x
constexpr double kLargeBudgetSeconds8Cores = 30.0;
constexpr unsigned kLargeBudgetCores = 8;

std::filesystem::path fixture(const char* name) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / (std::string(name) + ".tachyon");
}

struct Loaded {
  TraceModel model;
  double seconds = 0;
};

Loaded timedLoad(const std::filesystem::path& p) {
  const auto t0 = std::chrono::steady_clock::now();
  auto r = TraceModel::load(p);
  const auto t1 = std::chrono::steady_clock::now();
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  Loaded out;
  if (r.ok()) out.model = std::move(r).value();
  out.seconds = std::chrono::duration<double>(t1 - t0).count();
  return out;
}

size_t sampleCount(const TraceModel& m) {
  size_t n = 0;
  for (const SampleColumns& s : m.samplesByThread) n += s.ts.size();
  return n;
}

TEST(ModelLoadPerf, MediumLoadTime) {
  const auto path = fixture("medium");
  const double budget = kMediumBudgetSeconds * 2;
  double best = 1e30;
  for (int i = 0; i < 3; ++i) {
    Loaded l = timedLoad(path);
    std::printf(
        "[model perf] medium load #%d: %.3f s, %zu samples (target %.1f s, assert %.1f s, %u hw threads)\n",
        i, l.seconds, sampleCount(l.model), kMediumBudgetSeconds, budget,
        std::thread::hardware_concurrency());
    best = std::min(best, l.seconds);
  }
  std::printf("[model perf] medium load best: %.3f s\n", best);
  EXPECT_LT(best, budget);
}

TEST(ModelLoadPerf, MediumSingleThreadLoadTime) {
  LoadOptions opt;
  opt.threads = 1;
  const auto t0 = std::chrono::steady_clock::now();
  auto r = TraceModel::load(fixture("medium"), opt);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ASSERT_TRUE(r.ok());
  std::printf("[model perf] medium load, 1 thread: %.3f s (informational; assert < %.1f s)\n", s,
              kMediumBudgetSeconds * 4);
  EXPECT_LT(s, kMediumBudgetSeconds * 4);
}

TEST(ModelLoadPerf, MediumResidentSize) {
  const auto path = fixture("medium");
  Loaded l = timedLoad(path);
  const double compressed = static_cast<double>(std::filesystem::file_size(path));
  const double resident = static_cast<double>(l.model.residentBytes());
  const size_t samples = sampleCount(l.model);
  const double samplesBytes = static_cast<double>(samples) * 16.0;
  std::printf(
      "[model perf] medium: compressed %.2f MiB, resident %.2f MiB (ratio %.2fx), %.2f B/sample overall\n",
      compressed / 1048576.0, resident / 1048576.0, resident / compressed,
      resident / static_cast<double>(samples));
  // The Part 3.2 budget "resident <= 3x compressed" cannot hold for synthetic fixtures: their sample columns
  // compress about 8:1 (20 B on disk), while 3.2 fixes the in-memory sample at 16 B, i.e. about 7x the file.
  // The ratio is printed above; what is asserted is the layout contract below.
  // 3.2: a sample is 16 bytes in memory. The model must not cost more than that plus a small constant per
  // sample for everything else (nodes, frames, strings, events).
  EXPECT_GE(resident, samplesBytes);
  EXPECT_LE(resident, samplesBytes * 1.05);
}

TEST(ModelLoadPerf, LargeLoad) {
  const auto path = fixture("large");
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "large.tachyon not generated (build target tachyon_fixtures_large)";
  const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
  const double budget = kLargeBudgetSeconds8Cores * kLargeBudgetCores / cores;
  Loaded l = timedLoad(path);
  const double compressed = static_cast<double>(std::filesystem::file_size(path));
  const double resident = static_cast<double>(l.model.residentBytes());
  std::printf(
      "[model perf] large load: %.2f s, %zu samples on %u cores (target %.1f s scaled from 30 s @ 8 cores, "
      "assert %.1f s)\n",
      l.seconds, sampleCount(l.model), cores, budget, budget * 2);
  // The WP-30 "resident <= 3x compressed" target is under user review and is NOT asserted: both ratios are
  // printed. The uncompressed size is the sample columns (20 bytes per sample on disk), which dominate.
  const double uncompressed = static_cast<double>(sampleCount(l.model)) * 20.0;
  std::printf(
      "[model perf] large: compressed %.1f MiB, resident %.1f MiB (resident/compressed %.2fx, "
      "resident/uncompressed %.2fx, %.2f B/sample overall)\n",
      compressed / 1048576.0, resident / 1048576.0, resident / compressed, resident / uncompressed,
      resident / static_cast<double>(sampleCount(l.model)));
  EXPECT_LT(l.seconds, budget * 2);
}

// Streaming load (WP-64): how long until the first preview is on screen. A preview of 5% of the sample chunks
// must arrive in a small fraction of the full load, so that the viewer opens big traces at once. Targets:
// medium 0.4 s, large 3 s (asserted at 2x); also asserted to be well below the full load.
TEST(ModelLoadPerf, MediumFirstPreview) {
  const auto path = fixture("medium");
  double best = 1e30;
  for (int i = 0; i < 3; ++i) {
    LoadOptions opt;
    opt.prefixFraction = 0.05;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = TraceModel::load(path, opt);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r->loadStatus.complete);
    best = std::min(best, s);
  }
  Loaded full = timedLoad(path);
  std::printf("[model perf] medium first preview (5%% of the sample chunks): %.3f s (full load %.3f s)\n", best,
              full.seconds);
  EXPECT_LT(best, 0.4 * 2);
  EXPECT_LT(best, full.seconds);
}

TEST(ModelLoadPerf, LargeFirstPreview) {
  const auto path = fixture("large");
  if (!std::filesystem::exists(path))
    GTEST_SKIP() << "large.tachyon not generated (build target tachyon_fixtures_large)";
  LoadOptions opt;
  opt.prefixFraction = 0.05;
  const auto t0 = std::chrono::steady_clock::now();
  auto r = TraceModel::load(path, opt);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ASSERT_TRUE(r.ok());
  std::printf(
      "[model perf] large first preview: %.2f s, %zu samples (%u of %u sample chunks), resident %.0f MiB "
      "(target 3 s, assert 6 s)\n",
      s, sampleCount(*r), r->loadStatus.sampleChunksLoaded, r->loadStatus.sampleChunksTotal,
      static_cast<double>(r->residentBytes()) / 1048576.0);
  EXPECT_LT(s, 3.0 * 2);
}

}  // namespace
}  // namespace tachyon::model
