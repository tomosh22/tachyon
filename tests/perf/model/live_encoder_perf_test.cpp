// Perf budget (WP-65a): LiveEncoder throughput. The encoder sits on the capture's live path and must absorb
// 16 threads x 8 kHz (128 k samples per second) with a small fraction of one cpu (target 5%, asserted at 2x =
// 10%). The samples carry realistic callchains (depth 12 drawn from 4000 distinct stacks that share prefixes,
// so the stack cache and the node table are both exercised). Numbers are printed; the assertion is at 2x the
// target.
#include "control/live_encoder.h"
#include "model/live_model.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <vector>

namespace tachyon::control {
namespace {

constexpr uint64_t kNs = 1'000'000'000ull;

TEST(LiveEncoderPerf, EncodesThirtySecondsOf16ThreadsAt8kHz) {
  LiveEncoderConfig config;
  config.autoTick = false;
  LiveEncoder enc(config);
  capture::LiveSessionInfo info;
  info.clockFrequencyHz = kNs;
  info.platform = 2;
  info.samplingHz = 8000;
  info.startTs = 1;
  enc.onSessionInfo(info);
  capture::LiveProcess p;
  p.pid = 7;
  p.name = "perf";
  enc.onProcess(p);
  capture::LiveModule m;
  m.pid = 7;
  m.base = 0x400000;
  m.size = 0x400000;
  m.path = "/bin/perf";
  enc.onModule(m);
  for (uint32_t t = 0; t < 16; ++t) {
    capture::LiveThread th;
    th.pid = 7;
    th.tid = 100 + t;
    th.name = "worker";
    enc.onThread(th);
  }

  // 4000 stacks of depth 12: the outer 8 frames come from 200 prefixes, the inner 4 are specific.
  std::vector<std::vector<uint64_t>> stacks(4000);
  for (size_t i = 0; i < stacks.size(); ++i) {
    for (size_t d = 0; d < 12; ++d) {
      const uint64_t id = d >= 4 ? (i % 200) * 16 + (11 - d) : i * 16 + d;
      stacks[i].push_back(0x400000 + (id % 0x3FFFF0) * 4 + d);
    }
  }
  const uint64_t seconds = 30, total = seconds * 8000 * 16;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t x = 88172645463325252ull;
  for (uint64_t k = 0; k < total; ++k) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    const uint64_t ts = 1000 + (k / 16) * (kNs / 8000);
    enc.onSample(ts, 7, 100 + static_cast<uint32_t>(k % 16), static_cast<uint32_t>(k % 8), 0,
                 stacks[x % stacks.size()]);
    if (k % 4000 == 3999) enc.onTimeAdvance(ts);
    if (k % 32000 == 31999) enc.flush();
  }
  enc.flush();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const LiveEncoderStats st = enc.stats();
  const double nsPerSample = secs * 1e9 / static_cast<double>(total);
  // 128 k samples per second: the fraction of one cpu the encoder needs.
  const double cpuFraction = nsPerSample * 128000.0 / 1e9;
  std::printf(
      "[live perf] encoder: %llu samples in %.2f s = %.0f ns/sample (%.1f%% of one cpu at 128 k samples/s), "
      "nodes %llu frames %llu, retained %.1f MiB in %llu batches\n",
      static_cast<unsigned long long>(total), secs, nsPerSample, 100.0 * cpuFraction,
      static_cast<unsigned long long>(st.nodes), static_cast<unsigned long long>(st.frames),
      static_cast<double>(st.retainedBytes) / (1024.0 * 1024.0),
      static_cast<unsigned long long>(st.retainedBatches));
  EXPECT_EQ(st.samples, total);
  EXPECT_LT(cpuFraction, 0.10)
      << "the encoder must need under 5% of a cpu (asserted at 2x) at 16 threads x 8 kHz";
}

}  // namespace
}  // namespace tachyon::control
