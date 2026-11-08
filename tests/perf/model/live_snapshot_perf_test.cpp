// Perf budgets (WP-65a): LiveModel. A snapshot of 30 s x 8 kHz x 16 threads (3.84 M samples) must take less
// than 150 ms (asserted at 2x = 300 ms); applying the batches that build that window must keep up with
// capture speed (asserted: applying 30 s of samples takes less than 3 s, 10x faster than real time). The
// numbers are printed.
#include "model/live_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tachyon::model {
namespace {

using namespace live_wire;

constexpr uint32_t kThreads = 16;
constexpr uint64_t kRateHz = 8000;
constexpr uint64_t kSeconds = 30;
constexpr uint64_t kNs = 1'000'000'000ull;
constexpr uint32_t kNodes = 2000;

std::span<const std::byte> bytesOf(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// The definitions: one process, 16 threads, one module, kNodes frames and a chain of nodes.
std::string definitions() {
  std::string out;
  InfoPayload info{kNs, 2, static_cast<uint32_t>(kRateHz), 1};
  putRecord(out, RecordType::Info, &info, sizeof(info));
  const std::string name = "perf-test";
  putIndexedRecord(out, RecordType::StringDef, 1, "<overflow>", 10);
  putIndexedRecord(out, RecordType::StringDef, 2, name.data(), name.size());
  trace::ProcessRec pr{};
  pr.pid = 7;
  pr.name = StringIdx(2);
  putIndexedRecord(out, RecordType::ProcessDef, 0, &pr, sizeof(pr));
  for (uint32_t t = 0; t < kThreads; ++t) {
    trace::ThreadRec tr{};
    tr.tid = 100 + t;
    tr.pid = 7;
    tr.start_ts = 1;
    tr.name = StringIdx(2);
    tr.flags = 1;
    putIndexedRecord(out, RecordType::ThreadDef, t, &tr, sizeof(tr));
  }
  trace::ModuleRec mr{};
  mr.pid = 7;
  mr.base = 0x400000;
  mr.size = 0x100000;
  mr.path = StringIdx(2);
  putIndexedRecord(out, RecordType::ModuleDef, 0, &mr, sizeof(mr));
  for (uint32_t f = 0; f < kNodes; ++f) {
    trace::FrameRec fr{ModuleIdx(0), f * 16ull};
    putIndexedRecord(out, RecordType::FrameDef, f, &fr, sizeof(fr));
  }
  // A chain of nodes: node n's parent is n - 1 (depth grows), so stacks have up to 20 frames when sampled at
  // nodes near the chain tail of each of 100 chains.
  for (uint32_t n = 1; n < kNodes; ++n) {
    trace::CallstackNodeRec nr{CallstackIdx(n % 20 == 1 ? 0 : n - 1), FrameIdx(n)};
    putIndexedRecord(out, RecordType::NodeDef, n, &nr, sizeof(nr));
  }
  return out;
}

// `count` samples of the 16 threads starting at `first` (one per thread per tick, 125 us apart).
std::string samples(uint64_t first, uint32_t count) {
  std::vector<uint64_t> ts(count);
  std::vector<uint32_t> th(count), node(count);
  std::vector<uint16_t> cpu(count, 0), flags(count, 0);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t k = first + i;
    ts[i] = 1000 + (k / kThreads) * (kNs / kRateHz);
    th[i] = static_cast<uint32_t>(k % kThreads);
    node[i] = 1 + static_cast<uint32_t>((k * 2654435761u) % (kNodes - 1));
    cpu[i] = static_cast<uint16_t>(th[i] % 8);
  }
  std::string out;
  const uint32_t len = 4 + count * 20;
  RecordHeader h{static_cast<uint32_t>(RecordType::Samples), len};
  out.append(reinterpret_cast<const char*>(&h), sizeof(h));
  out.append(reinterpret_cast<const char*>(&count), 4);
  out.append(reinterpret_cast<const char*>(ts.data()), count * 8ull);
  out.append(reinterpret_cast<const char*>(th.data()), count * 4ull);
  out.append(reinterpret_cast<const char*>(node.data()), count * 4ull);
  out.append(reinterpret_cast<const char*>(cpu.data()), count * 2ull);
  out.append(reinterpret_cast<const char*>(flags.data()), count * 2ull);
  const uint64_t last = ts.back();
  putRecord(out, RecordType::TimeAdvance, &last, sizeof(last));
  return out;
}

TEST(LiveModelPerf, SnapshotOfThirtySecondsAt8kHzAnd16Threads) {
  LiveModel m;
  const std::string defs = definitions();
  ASSERT_TRUE(m.apply(bytesOf(defs), true).ok());
  const uint64_t total = kSeconds * kRateHz * kThreads;  // 3.84 M samples
  constexpr uint32_t kBatch = 4000;                      // 31 ms of capture per batch: about 8 per 250 ms
  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t first = 0; first < total; first += kBatch) {
    const std::string batch =
        samples(first, static_cast<uint32_t>(std::min<uint64_t>(kBatch, total - first)));
    ASSERT_TRUE(m.apply(bytesOf(batch), false).ok());
  }
  const double applySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const LiveModelStats st = m.stats();
  EXPECT_EQ(st.samplesInWindow, total);

  double best = 1e30, worst = 0;
  for (int i = 0; i < 5; ++i) {
    const auto s0 = std::chrono::steady_clock::now();
    auto snap = m.snapshot();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
    best = std::min(best, ms);
    worst = std::max(worst, ms);
    ASSERT_NE(snap, nullptr);
    EXPECT_EQ(snap->samplesByThread.size(), kThreads);
  }
  std::printf(
      "[live perf] %llu samples: apply %.3f s (%.1fx real time), snapshot best %.1f ms worst %.1f ms (target "
      "150 ms, assert 300 ms), resident %.1f MiB\n",
      static_cast<unsigned long long>(total), applySeconds, static_cast<double>(kSeconds) / applySeconds,
      best, worst, static_cast<double>(st.residentBytes) / (1024.0 * 1024.0));
  EXPECT_LT(best, 300.0) << "snapshot of 4 M samples";
  EXPECT_LT(applySeconds, 3.0) << "applying 30 s of capture must be 10x faster than real time";
}

TEST(LiveModelPerf, WindowTrimmingKeepsMemoryFlatAndApplyFast) {
  LiveModel m;
  const std::string defs = definitions();
  ASSERT_TRUE(m.apply(bytesOf(defs), true).ok());
  constexpr uint32_t kBatch = 4000;
  uint64_t first = 0;
  auto feed = [&](uint64_t seconds) {
    const uint64_t n = seconds * kRateHz * kThreads;
    for (uint64_t done = 0; done < n; done += kBatch, first += kBatch)
      ASSERT_TRUE(m.apply(bytesOf(samples(first, kBatch)), false).ok());
  };
  feed(40);  // past the window
  const uint64_t atFull = m.stats().residentBytes;
  feed(40);  // and a lot more: the window slides, memory must not grow
  const LiveModelStats st = m.stats();
  std::printf("[live perf] resident after 40 s: %.1f MiB, after 80 s: %.1f MiB, samples in window %llu\n",
              static_cast<double>(atFull) / (1024.0 * 1024.0),
              static_cast<double>(st.residentBytes) / (1024.0 * 1024.0),
              static_cast<unsigned long long>(st.samplesInWindow));
  EXPECT_LE(st.samplesInWindow, kSeconds * kRateHz * kThreads + kBatch);
  EXPECT_LT(static_cast<double>(st.residentBytes), 1.10 * static_cast<double>(atFull));
}

}  // namespace
}  // namespace tachyon::model
