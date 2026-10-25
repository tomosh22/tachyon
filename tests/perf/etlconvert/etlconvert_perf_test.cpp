// Perf budget (WP-12): convert a 2 GB ETL in < 90 s on 8 cores.
//
// A 2 GB kernel ETL holds roughly 25-30 M events including the StackWalk events, so the whole conversion
// needs about 300 k events/s end to end. ProcessTrace has to read the file twice (metadata pass + main pass)
// and that is outside this test, so the pipeline alone is budgeted at 600 k events/s, i.e. half of the time.
// The assertion is at 2x that target (300 k events/s sustained, which is also the bare 90 s requirement).
//
// The benchmark feeds synthetic decoded events through ConversionPipeline (parse stage on this thread,
// intern stage on the worker): 10 M parent events (80% samples, 12% context switches, 8% readies), each
// followed by a 30-deep StackWalk, = 20 M input events. 10% of the stacks are unique, picked with a skewed
// distribution like real sampling data; frames fall into 64 modules of the target process plus the kernel.
#include "core/span_util.h"
#include "core/time.h"
#include "etlconvert/etl_converter.h"
#include "etlconvert/etl_events.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

namespace tachyon::etlconvert {
namespace {

constexpr uint64_t kParents = 10'000'000;
constexpr double kTargetEventsPerSecond = 600'000;  // pipeline-only target (see header comment)
constexpr size_t kDepth = 30;
constexpr size_t kUniqueStacks = 100'000;  // 10% of the parent count
constexpr uint32_t kModules = 64;
constexpr uint64_t kModuleSize = 0x100000;
constexpr uint64_t kModuleBase = 0x7ff600000000ull;
constexpr uint64_t kKernelBase = 0xFFFFF80000000000ull;
constexpr uint32_t kPid = 100;
constexpr uint32_t kThreads = 16;

template <typename T>
void put(std::vector<std::byte>& out, T value) {
  core::appendPod(out, value);
}

std::vector<std::byte> imageEvent(uint64_t base, uint64_t size, uint32_t pid, const char16_t* path) {
  std::vector<std::byte> b;
  put<uint64_t>(b, base);
  put<uint64_t>(b, size);
  put<uint32_t>(b, pid);
  put<uint32_t>(b, 0);
  put<uint32_t>(b, 0x5F000000);
  put<uint32_t>(b, 0);
  put<uint64_t>(b, base);
  for (int i = 0; i < 4; ++i) put<uint32_t>(b, 0);
  for (const char16_t* c = path; *c != 0; ++c) put<uint16_t>(b, static_cast<uint16_t>(*c));
  put<uint16_t>(b, 0);
  return b;
}

std::vector<std::byte> threadEvent(uint32_t pid, uint32_t tid) {
  std::vector<std::byte> b;
  put<uint32_t>(b, pid);
  put<uint32_t>(b, tid);
  for (int i = 0; i < 12; ++i) put<uint32_t>(b, 0);
  return b;
}

class Generator {
 public:
  Generator() : m_rng(12345) {
    // Unique stacks: 28 user frames then 2 kernel-less user frames; frame addresses come from a bounded set
    // of functions per module, so the callstack DAG has realistic sharing.
    m_stacks.resize(kUniqueStacks * kDepth);
    for (size_t s = 0; s < kUniqueStacks; ++s) {
      for (size_t d = 0; d < kDepth; ++d) {
        const uint32_t module = static_cast<uint32_t>(m_rng() % kModules);
        const uint64_t function = m_rng() % 4000;
        m_stacks[s * kDepth + d] = kModuleBase + module * kModuleSize + 0x1000 + function * 64 + m_rng() % 32;
      }
    }
    // Some stacks start in the kernel (context switches).
    for (size_t s = 0; s < kUniqueStacks; s += 3) {
      m_stacks[s * kDepth] = kKernelBase + 0x100 + (m_rng() % 500) * 16;
      m_stacks[s * kDepth + 1] = kKernelBase + 0x100 + (m_rng() % 500) * 16;
    }
  }

  const std::vector<uint64_t>& stacks() const { return m_stacks; }

  size_t pickStack() {
    const double u = std::generate_canonical<double, 32>(m_rng);
    return static_cast<size_t>(static_cast<double>(kUniqueStacks) * u * u * u * u) % kUniqueStacks;
  }
  uint64_t random() { return m_rng(); }

 private:
  std::mt19937_64 m_rng;
  std::vector<uint64_t> m_stacks;
};

// Runs `parents` parent events (+ their stacks) through a fresh pipeline. Returns events fed and seconds.
struct RunResult {
  uint64_t events = 0;
  double seconds = 0;
  ConvertStats stats;
};

RunResult run(uint64_t parents, bool threaded, const std::filesystem::path& out) {
  Generator gen;
  ConvertOptions options;
  options.targetPids = {kPid};
  options.threaded = threaded;
  options.peLookup = [](const std::string&) -> Result<PeInfo> {
    return makeError(ErrorCode::NotFound, "none");
  };
  static const DevicePathMapper mapper;
  options.deviceMapper = &mapper;
  EtlFileInfo info;
  info.clockFrequencyHz = 10'000'000;

  // Metadata: one process with kThreads threads, kModules modules, and a kernel image.
  std::vector<std::vector<std::byte>> payloads;
  std::vector<EtwEvent> meta;
  auto addMeta = [&](const Guid& provider, uint8_t opcode, uint32_t tid, std::vector<std::byte> payload) {
    payloads.push_back(std::move(payload));
    EtwEvent e;
    e.provider = provider;
    e.opcode = opcode;
    e.version = 3;
    e.timestamp = 1;
    e.threadId = tid;
    e.processId = kPid;
    e.payload = Bytes(payloads.back().data(), payloads.back().size());
    meta.push_back(e);
  };
  for (uint32_t t = 0; t < kThreads; ++t)
    addMeta(guids::kThread, opcodes::kThreadStart, 4, threadEvent(kPid, 1000 + t));
  for (uint32_t m = 0; m < kModules; ++m) {
    addMeta(guids::kImage, opcodes::kImageLoad, 4,
            imageEvent(kModuleBase + m * kModuleSize, kModuleSize, kPid, u"C:\\game\\module.dll"));
  }
  addMeta(guids::kImage, opcodes::kImageLoad, 4,
          imageEvent(kKernelBase, 0x1000000, 0xFFFFFFFFu, u"ntoskrnl.exe"));

  auto created = ConversionPipeline::create(out, options, info);
  EXPECT_TRUE(created.ok()) << (created.ok() ? "" : created.error().toString());
  RunResult result;
  if (!created.ok()) return result;
  ConversionPipeline& pipeline = **created;

  const uint64_t t0 = core::nowTicks();
  pipeline.beginPass(PipelinePass::Metadata);
  for (const EtwEvent& e : meta) pipeline.onEvent(e);
  pipeline.beginPass(PipelinePass::Main);

  std::vector<std::byte> parentPayload(24);
  std::vector<std::byte> stackPayload(16 + kDepth * 8);
  uint64_t ts = 1000;
  EtwEvent parent;
  parent.version = 2;
  parent.processId = kPid;
  EtwEvent stack;
  stack.provider = guids::kStackWalk;
  stack.opcode = opcodes::kStackWalkStack;
  stack.version = 2;
  stack.processId = kPid;
  for (uint64_t i = 0; i < parents; ++i) {
    const uint32_t tid = 1000 + static_cast<uint32_t>(i % kThreads);
    const uint32_t pick = static_cast<uint32_t>(gen.random() % 100);
    ts += 80 + gen.random() % 40;
    parent.timestamp = ts;
    parent.threadId = tid;
    parent.cpu = static_cast<uint16_t>(i % 16);
    uint32_t stackThread = tid;
    if (pick < 80) {
      parent.provider = guids::kPerfInfo;
      parent.opcode = opcodes::kPerfInfoSampledProfile;
      std::memset(parentPayload.data(), 0, 16);
      const uint64_t ip = gen.stacks()[0];
      std::memcpy(parentPayload.data(), &ip, 8);
      std::memcpy(parentPayload.data() + 8, &tid, 4);
      parent.payload = Bytes(parentPayload.data(), 16);
    } else if (pick < 92) {
      parent.provider = guids::kThread;
      parent.opcode = opcodes::kThreadCSwitch;
      std::memset(parentPayload.data(), 0, 24);
      const uint32_t other = 5000 + static_cast<uint32_t>(i % 7);  // thread of another process
      std::memcpy(parentPayload.data(), &tid, 4);                  // new thread
      std::memcpy(parentPayload.data() + 4, &other, 4);            // old thread
      parentPayload[12] = std::byte{16};                           // wait reason
      parent.payload = Bytes(parentPayload.data(), 24);
    } else {
      parent.provider = guids::kThread;
      parent.opcode = opcodes::kThreadReadyThread;
      std::memset(parentPayload.data(), 0, 8);
      std::memcpy(parentPayload.data(), &tid, 4);  // readied thread (header thread is the readying thread)
      const uint32_t readying = 5000 + static_cast<uint32_t>(i % 7);
      parent.threadId = readying;
      stackThread = readying;
      parent.payload = Bytes(parentPayload.data(), 8);
    }
    pipeline.onEvent(parent);

    // The stack follows its parent right away.
    const uint64_t stackTs = ts + 5;
    const uint32_t stackProc = kPid;
    std::memcpy(stackPayload.data(), &ts, 8);
    std::memcpy(stackPayload.data() + 8, &stackProc, 4);
    std::memcpy(stackPayload.data() + 12, &stackThread, 4);
    std::memcpy(stackPayload.data() + 16, gen.stacks().data() + gen.pickStack() * kDepth, kDepth * 8);
    stack.timestamp = stackTs;
    stack.threadId = stackThread;
    stack.payload = Bytes(stackPayload.data(), stackPayload.size());
    pipeline.onEvent(stack);
    ts = stackTs;
    result.events += 2;
  }
  auto stats = pipeline.finish();
  const uint64_t t1 = core::nowTicks();
  EXPECT_TRUE(stats.ok()) << (stats.ok() ? "" : stats.error().toString());
  if (stats.ok()) result.stats = std::move(stats).value();
  result.seconds = static_cast<double>(t1 - t0) / static_cast<double>(core::ticksFrequency());
  return result;
}

TEST(EtlConvertPerf, PipelineThroughputThreaded) {
  const auto out = std::filesystem::path(TACHYON_BINARY_DIR) / "etlconvert_perf.tachyon";
  const RunResult r = run(kParents, /*threaded=*/true, out);
  ASSERT_GT(r.seconds, 0.0);
  const double rate = static_cast<double>(r.events) / r.seconds;
  std::error_code ec;
  const double fileMb = static_cast<double>(std::filesystem::file_size(out, ec)) / (1024.0 * 1024.0);
  std::filesystem::remove(out, ec);
  const double secondsFor2Gb = 28e6 / rate;
  std::printf(
      "[etlconvert perf] %llu events (%llu parents) in %.2f s = %.0f events/s (target %.0f, assert >= %.0f); "
      "extrapolated 28M-event (2 GB) conversion, pipeline only: %.1f s; samples %llu, switches %llu, readies "
      "%llu; matched %llu, unmatched %llu; output %.1f MB\n",
      static_cast<unsigned long long>(r.events), static_cast<unsigned long long>(kParents), r.seconds, rate,
      kTargetEventsPerSecond, kTargetEventsPerSecond / 2, secondsFor2Gb,
      static_cast<unsigned long long>(r.stats.samplesWritten),
      static_cast<unsigned long long>(r.stats.cswitchesWritten),
      static_cast<unsigned long long>(r.stats.readiesWritten),
      static_cast<unsigned long long>(r.stats.stacksMatched),
      static_cast<unsigned long long>(r.stats.stacksUnmatched), fileMb);
  EXPECT_GT(r.stats.samplesWritten, kParents / 2);
  EXPECT_EQ(r.stats.stacksUnmatched, 0u);
  // Budget: 2x of the target.
  EXPECT_GE(rate, kTargetEventsPerSecond / 2);
}

TEST(EtlConvertPerf, PipelineThroughputInlineForReference) {
  const auto out = std::filesystem::path(TACHYON_BINARY_DIR) / "etlconvert_perf_inline.tachyon";
  const RunResult r = run(kParents / 5, /*threaded=*/false, out);
  ASSERT_GT(r.seconds, 0.0);
  const double rate = static_cast<double>(r.events) / r.seconds;
  std::error_code ec;
  std::filesystem::remove(out, ec);
  std::printf("[etlconvert perf] single thread (parse and intern on one thread): %.0f events/s\n", rate);
  EXPECT_GE(rate, kTargetEventsPerSecond / 4);  // sanity floor only
}

}  // namespace
}  // namespace tachyon::etlconvert
