// Perf budget (WP-65w): the ETW live decode path (copy -> timestamp sort -> LiveEtwDecoder) must sustain the
// event rate of an 8 kHz capture of a 16 core machine on a fraction of ONE core.
//
// 16 cores x 8.19 kHz = about 131 k SampledProfile events/s, each followed by a StackWalk event: 262 k
// events/s (context switches come on top, but only those touching the target are decoded). The target is 25%
// of a core, i.e. 1.05 M events/s through the whole path; the assertion is at 2x that (525 k events/s), so a
// regression to half the target fails CI. The sink is a counting stub: LiveEncoder has its own budget
// (tests/perf/model/live_encoder_perf_test.cpp).
//
// The benchmark feeds 400 k samples (0.8 M events, 3 s of such a capture, 30-deep stacks, 1000 distinct
// stacks, 16 target threads) through LiveEtwSource exactly like the real-time consumer does: submit() per
// event, endBuffer() per ETW buffer (4000 events), on this thread while the source's thread decodes. Prints
// the numbers.
#include "capture/windows_etw/live_etw_source.h"
#include "core/span_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace tachyon::capture::etw {
namespace {

using namespace etlconvert;

constexpr uint64_t kSamples = 400'000;
constexpr size_t kDepth = 30;
constexpr size_t kUniqueStacks = 1000;
constexpr uint32_t kPid = 100;
constexpr uint32_t kThreads = 16;
constexpr uint64_t kBase = 0x7ff600000000ull;
constexpr double kTargetEventsPerSecond = 1'050'000;  // 25% of a core at 262 k events/s
constexpr size_t kBufferEvents = 4000;

template <typename T>
void put(std::vector<std::byte>& out, T value) {
  core::appendPod(out, value);
}

class CountingSink final : public ILiveSink {
 public:
  void onSample(uint64_t, uint32_t, uint32_t, uint32_t, uint16_t, std::span<const uint64_t> ips) override {
    ++samples;
    frames += ips.size();
  }
  uint64_t samples = 0, frames = 0;
};

TEST(EtwLivePerf, DecodePathSustainsASixteenCoreCaptureOnAQuarterOfACore) {
  // Events are built before the clock starts.
  std::mt19937_64 rng(7);
  std::vector<std::vector<uint64_t>> stacks(kUniqueStacks);
  for (auto& s : stacks)
    for (size_t i = 0; i < kDepth; ++i) s.push_back(kBase + (rng() & 0xFFFFF) * 16);

  struct Ev {
    EtwEvent e;
    std::vector<std::byte> data;
  };
  std::vector<Ev> events;
  events.reserve(kSamples * 2 + 40);
  auto add = [&](const Guid& g, uint8_t op, uint8_t ver, uint64_t ts, uint32_t tid, uint32_t pid,
                 uint16_t cpu, std::vector<std::byte> data) {
    Ev ev;
    ev.e.provider = g;
    ev.e.opcode = op;
    ev.e.version = ver;
    ev.e.timestamp = ts;
    ev.e.threadId = tid;
    ev.e.processId = pid;
    ev.e.cpu = cpu;
    ev.data = std::move(data);
    events.push_back(std::move(ev));
  };
  {  // the process, its threads and its image
    std::vector<std::byte> p;
    put<uint64_t>(p, 0xFFFF800012345678ull);
    put<uint32_t>(p, kPid);
    put<uint32_t>(p, 4);
    put<uint32_t>(p, 1);
    put<int32_t>(p, 0);
    put<uint64_t>(p, 0x1000);
    put<uint64_t>(p, 0);
    for (char c : std::string("game.exe")) put<uint8_t>(p, static_cast<uint8_t>(c));
    put<uint8_t>(p, 0);
    put<uint16_t>(p, 0);
    add(guids::kProcess, opcodes::kProcessStart, 3, 1, 4, 4, 0, std::move(p));
    for (uint32_t t = 0; t < kThreads; ++t) {
      std::vector<std::byte> th;
      put<uint32_t>(th, kPid);
      put<uint32_t>(th, 1000 + t);
      for (int i = 0; i < 12; ++i) put<uint32_t>(th, 0);
      add(guids::kThread, opcodes::kThreadStart, 3, 2, 4, 4, 0, std::move(th));
    }
    std::vector<std::byte> img;
    put<uint64_t>(img, kBase);
    put<uint64_t>(img, 0x20000000);
    put<uint32_t>(img, kPid);
    put<uint32_t>(img, 0);
    put<uint32_t>(img, 0x5F000000);
    put<uint32_t>(img, 0);
    put<uint64_t>(img, kBase);
    for (int i = 0; i < 4; ++i) put<uint32_t>(img, 0);
    for (char16_t c : std::u16string(u"C:\\game\\game.exe")) put<uint16_t>(img, static_cast<uint16_t>(c));
    put<uint16_t>(img, 0);
    add(guids::kImage, opcodes::kImageLoad, 3, 3, 4, 4, 0, std::move(img));
  }
  uint64_t ts = 1000;
  for (uint64_t i = 0; i < kSamples; ++i) {
    const uint32_t tid = 1000 + static_cast<uint32_t>(i % kThreads);
    const auto& st = stacks[(i * 2654435761u) % kUniqueStacks];
    std::vector<std::byte> sp;
    put<uint64_t>(sp, st[0]);
    put<uint32_t>(sp, tid);
    put<uint16_t>(sp, 1);
    put<uint16_t>(sp, 0);
    add(guids::kPerfInfo, opcodes::kPerfInfoSampledProfile, 2, ts, tid, kPid, static_cast<uint16_t>(i % 16),
        std::move(sp));
    std::vector<std::byte> sw;
    put<uint64_t>(sw, ts);
    put<uint32_t>(sw, kPid);
    put<uint32_t>(sw, tid);
    for (uint64_t f : st) put<uint64_t>(sw, f);
    add(guids::kStackWalk, opcodes::kStackWalkStack, 2, ts + 3, tid, kPid, static_cast<uint16_t>(i % 16),
        std::move(sw));
    ts += 76;  // 16 CPUs at 8.19 kHz on a 10 MHz clock: one sample every ~76 ticks
  }
  for (Ev& ev : events) ev.e.payload = Bytes(ev.data.data(), ev.data.size());

  auto sink = std::make_shared<CountingSink>();
  LiveEtwSourceConfig cfg;
  cfg.decoder.sink = sink;
  cfg.decoder.clockFrequencyHz = 10'000'000;
  cfg.decoder.startTs = 1;
  cfg.decoder.stackWindowMs = 250;
  cfg.decoder.peLookup = [](const std::string& path) -> Result<PeInfo> {
    return makeError(ErrorCode::NotFound, path);
  };
  cfg.holdBack = std::chrono::milliseconds(0);
  cfg.maxLag = std::chrono::seconds(600);
  cfg.queueBytes = 8ull << 30;  // the benchmark measures decoding, not the drop policy
  LiveEtwSource source(cfg);
  source.start();
  source.arm({kPid});

  const auto t0 = std::chrono::steady_clock::now();
  size_t inBuffer = 0;
  for (const Ev& ev : events) {
    source.submit(ev.e);
    if (++inBuffer == kBufferEvents) {
      source.endBuffer();
      inBuffer = 0;
    }
  }
  source.endBuffer();
  source.finish();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const double rate = static_cast<double>(events.size()) / secs;
  const LiveEtwSourceStats st = source.stats();
  std::printf(
      "ETW-LIVE-PERF events=%zu samples=%llu seconds=%.2f events_per_s=%.0f decode_cpu_s=%.2f "
      "(target %.0f, asserted at %.0f)\n",
      events.size(), static_cast<unsigned long long>(sink->samples), secs, rate, st.decodeCpuSeconds,
      kTargetEventsPerSecond, kTargetEventsPerSecond / 2);
  EXPECT_EQ(sink->samples, kSamples);
  EXPECT_EQ(st.droppedEvents, 0u);
  EXPECT_EQ(st.decoder.stacksUnmatched, 0u);
  EXPECT_GT(rate, kTargetEventsPerSecond / 2) << "the decode path fell to under half of its target rate";
}

}  // namespace
}  // namespace tachyon::capture::etw
