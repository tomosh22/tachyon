// Perf budget (WP-70): converting a perf spool to .tachyon, 24-deep user callchains.
//   Skewed: 2 M samples over 20 k unique stacks (a few hot stacks dominate, like real sampling data): target
//           500 k samples/s, asserted at 2x (250 k/s). Most samples hit the converter's stack cache.
//   Unique: 400 k samples that all have a different stack (8 random leaf-side frames on top of one of 2000
//           shared 16-frame call paths, like the branching of a real call tree), so every sample is resolved
//           frame by frame and interned: the stack cache never hits. Target 60 k samples/s, asserted at 2x
//           (30 k/s); the cost is dominated by inserting millions of new frames and callstack nodes into the
//           writer's intern tables.
// At 8 kHz on 16 cpus a capture produces ~128 k samples/s, so a 60 s capture converts in well under a minute.
//
// The spool is synthetic: one process, 64 modules (identified by kernel build-ids, so no files are read), the
// samples spread over 4 rings and 8 threads.
#include "capture/linux_perf/perf_converter.h"
#include "capture/linux_perf/perf_records.h"
#include "capture/linux_perf/spool.h"
#include "core/time.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;

constexpr size_t kDepth = 24;
constexpr uint32_t kModules = 64;
constexpr uint64_t kModuleSize = 0x100000;
constexpr uint64_t kModuleBase = 0x7f0000000000ull;
constexpr uint32_t kPid = 100;
constexpr uint32_t kRings = 4;

void runBenchmark(const char* label, uint64_t samples, size_t uniqueStacks, bool skewed,
                  double targetSamplesPerSecond) {
  const fs::path dir = fs::temp_directory_path() /
                       ("tachyon-perf-bench-" + std::string(label) + "-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);

  const RecordLayout layout;
  std::mt19937_64 rng(99);
  std::vector<std::vector<uint64_t>> stacks(uniqueStacks);
  auto randomFrame = [&] { return kModuleBase + (rng() % kModules) * kModuleSize + (rng() % 0x8000) * 16; };
  std::vector<std::vector<uint64_t>> prefixes(2000);  // root-side call paths shared between stacks
  for (auto& p : prefixes)
    for (size_t d = 0; d < kDepth - 8; ++d) p.push_back(randomFrame());
  for (size_t n = 0; n < stacks.size(); ++n) {
    auto& s = stacks[n];
    s.push_back(kContextUser);
    for (size_t d = 0; d < 8; ++d) s.push_back(randomFrame());  // leaf side: different for every stack
    s.insert(s.end(), prefixes[n % prefixes.size()].begin(), prefixes[n % prefixes.size()].end());
  }

  {
    SessionInfo info;
    info.layout = layout;
    info.startTs = 1000;
    info.endTs = 1000 + samples * 125 / kRings + 1000;
    info.samplingHz = 8000;
    info.mode = "launch";
    info.finished = true;
    ASSERT_TRUE(writeSessionInfo(dir, info).ok());

    auto meta = SpoolWriter::create(spoolMetaPath(dir), SpoolKind::Meta, layout);
    ASSERT_TRUE(meta.ok());
    RecordBuilder m(layout);
    m.fork(kPid, 1, kPid, 1, 1001, {1, 1, 1001, 0, 0, 0});
    m.comm(kPid, kPid, "bench", true, {kPid, kPid, 1002, 0, 0, 0});
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kModules; ++i) names.push_back("/nonexistent/lib" + std::to_string(i) + ".so");
    for (uint32_t i = 0; i < kModules; ++i) {
      RecordBuilder::Mmap2 mm;
      mm.pid = mm.tid = kPid;
      mm.addr = kModuleBase + i * kModuleSize;
      mm.len = kModuleSize;
      mm.name = names[i];
      mm.buildId = {static_cast<uint8_t>(i), 1, 2, 3, 4, 5, 6, 7};
      mm.id = {kPid, kPid, 1010 + i, 0, 0, 0};
      m.mmap2(mm);
    }
    ASSERT_TRUE(meta->append(0, m.bytes()).ok());
    ASSERT_TRUE(meta->close().ok());

    auto data = SpoolWriter::create(spoolDataPath(dir, 0), SpoolKind::Data, layout);
    ASSERT_TRUE(data.ok());
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (uint32_t ring = 0; ring < kRings; ++ring) {
      RecordBuilder b(layout);
      const uint64_t perRing = samples / kRings;
      for (uint64_t i = 0; i < perRing; ++i) {
        const double u = unit(rng);
        const size_t pick = skewed ? static_cast<size_t>(static_cast<double>(uniqueStacks) * u * u * u * u)
                                   : static_cast<size_t>((i * kRings + ring) % uniqueStacks);
        RecordBuilder::Sample s;
        s.id = {kPid, kPid + static_cast<uint32_t>(i % 8), 2000 + i * 125 + ring, ring, 0, 0};
        s.chain = stacks[pick];
        s.ip = s.chain[1];
        b.sample(s);
        if (b.bytes().size() > (1u << 22)) {
          ASSERT_TRUE(data->append(ring, b.bytes()).ok());
          b.clear();
        }
      }
      if (!b.bytes().empty()) ASSERT_TRUE(data->append(ring, b.bytes()).ok());
    }
    ASSERT_TRUE(data->close().ok());
  }

  auto set = SpoolSet::openDirectory(dir);
  ASSERT_TRUE(set.ok());
  ConvertOptions options;
  options.rootPids = {kPid};
  options.procRoot = dir / "noproc";
  const uint64_t t0 = core::nowTicks();
  auto stats = convertSpool(*set, dir / "out.tachyon", options);
  const double seconds = core::ticksToSeconds(core::nowTicks() - t0, core::ticksFrequency());
  ASSERT_TRUE(stats.ok()) << stats.error().toString();
  ASSERT_EQ(stats->samplesWritten, samples / kRings * kRings);
  EXPECT_EQ(stats->framesUnmapped, 0u);
  const double rate = static_cast<double>(stats->samplesWritten) / seconds;
  std::printf(
      "[perf] %s: converted %llu samples (%zu frames each) in %.2f s: %.0f samples/s, %.1f M frames/s "
      "(target %.0f)\n",
      label, static_cast<unsigned long long>(stats->samplesWritten), kDepth, seconds, rate,
      rate * static_cast<double>(kDepth) / 1e6, targetSamplesPerSecond);
  EXPECT_GT(rate, targetSamplesPerSecond / 2) << "regressed past 2x the target";
  fs::remove_all(dir);
}

TEST(LinuxPerfConvertPerf, SkewedStacksAtSamplingRate) {
  runBenchmark("skewed", 2'000'000, 20'000, true, 500'000);
}

TEST(LinuxPerfConvertPerf, AllUniqueStacks) {
  runBenchmark("unique", 400'000, 400'000, false, 60'000);
}

}  // namespace
}  // namespace tachyon::capture::perf
