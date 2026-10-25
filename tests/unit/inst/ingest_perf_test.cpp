// Perf budgets for the instrumentation path (Part 2.4): spooling raw frames, decoding them and merging them
// into a trace. Each budget is a target; the assertion fails at 2x of it (a 2x regression), and the measured
// numbers are printed.
#include "inst/decoder.h"
#include "inst/merge.h"
#include "inst/spool.h"
#include "trace/trace_reader.h"
#include "trace_builder.h"
#include "wire_builder.h"

#include "../control/test_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

namespace tachyon::inst {
namespace {

using Clock = std::chrono::steady_clock;
using control::testing_util::TempDir;
using testing_util::StreamBuilder;

constexpr uint64_t kPairs = 1'000'000;  // 2 M scope events, ~100 MB on the wire with data strings

double secondsSince(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

// A realistic mix: several threads, nested scopes, a short dynamic data string on every other scope.
StreamBuilder bigStream() {
  StreamBuilder b;
  b.header(1234, 1'000'000'000, 1);
  uint64_t names[8];
  for (int i = 0; i < 8; ++i) names[i] = b.string("scope_" + std::to_string(i));
  for (uint64_t i = 0; i < kPairs; ++i) {
    const uint32_t tid = 1000 + static_cast<uint32_t>(i % 8);
    const uint64_t ts = 10'000 + i * 20;
    b.scopeBegin(ts, tid, names[i % 8], (i % 2) ? "n=" + std::to_string(i % 1000) : std::string());
    b.scopeEnd(ts + 10, tid);
  }
  return b;
}

TEST(InstPerf, SpoolDecodeAndMergeThroughput) {
  const StreamBuilder stream = bigStream();
  const double megabytes = static_cast<double>(stream.bytes().size()) / (1024.0 * 1024.0);
  TempDir dir;

  // 1) Spooling: frame boundaries + append, fed in 64 KiB chunks like the socket reader does. Target 0.5 s
  // (measured 0.07 s).
  {
    auto spool = Spool::create(dir.path() / "s.tachyon-inst", kDefaultMaxSpoolBytes);
    ASSERT_TRUE(spool.ok());
    FrameAssembler assembler;
    const auto t0 = Clock::now();
    const auto& bytes = stream.bytes();
    for (size_t off = 0; off < bytes.size(); off += 65536) {
      const size_t n = std::min<size_t>(65536, bytes.size() - off);
      assembler.feed(bytes.data() + off, n,
                     [&](const uint8_t* f, size_t len) { (*spool)->appendFrame(f, len); });
    }
    ASSERT_TRUE((*spool)->close().ok());
    const double s = secondsSince(t0);
    std::fprintf(stderr, "[inst-perf] spool: %.0f MB in %.2f s = %.0f MB/s, %.1f M frames/s\n", megabytes, s,
                 megabytes / s, static_cast<double>(stream.frames()) / s / 1e6);
    EXPECT_LT(s, 2 * 0.5) << "spooling regressed";
  }

  // 2) Decoding the spool file: remap, clock mapping, balance accounting, sort. Target 1.5 s (measured 0.45
  // s).
  DecodedInstrumentation decoded;
  {
    InstDecoder decoder;
    const auto t0 = Clock::now();
    ASSERT_TRUE(decoder.addFile(dir.path() / "s.tachyon-inst").ok());
    decoded = decoder.finish();
    const double s = secondsSince(t0);
    std::fprintf(stderr, "[inst-perf] decode: %.0f MB, %zu records in %.2f s = %.0f MB/s, %.1f M records/s\n",
                 megabytes, decoded.recordCount(), s, megabytes / s,
                 static_cast<double>(decoded.recordCount()) / s / 1e6);
    EXPECT_EQ(decoded.stats.scopeBegins, kPairs);
    EXPECT_EQ(decoded.stats.unmatchedScopeEnds, 0u);
    EXPECT_EQ(decoded.stats.unclosedScopes, 0u);
    EXPECT_LT(s, 2 * 1.5) << "decoding regressed";
  }

  // 3) Merging into a trace with a small sampling part: string remap, union, chunked write with zstd. Target
  // 1 s (measured 0.23 s).
  {
    const auto base = testing_util::writeBaseTrace(dir.path() / "base.tachyon", 1000, 100'000'000);
    const auto t0 = Clock::now();
    MergeOptions options;
    options.outPath = dir.path() / "merged.tachyon";
    auto merged = mergeDecoded(base.path, std::move(decoded), options);
    ASSERT_TRUE(merged.ok()) << merged.error().toString();
    const double s = secondsSince(t0);
    std::fprintf(stderr, "[inst-perf] merge: %llu scope events in %.2f s = %.1f M events/s, output %.0f MB\n",
                 static_cast<unsigned long long>(merged->scopeEvents), s,
                 static_cast<double>(merged->scopeEvents) / s / 1e6,
                 static_cast<double>(std::filesystem::file_size(options.outPath)) / (1024.0 * 1024.0));
    EXPECT_EQ(merged->scopeEvents, 2 * kPairs);
    EXPECT_LT(s, 2 * 1.0) << "merging regressed";
  }
}

}  // namespace
}  // namespace tachyon::inst
