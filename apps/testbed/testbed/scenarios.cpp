#include "testbed/scenarios.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "core/platform.h"
#include "testbed/hooks.h"
#include "testbed/platform.h"
#include "testbed/work_kernel.h"

using tachyon::testbed::burn;
using tachyon::testbed::burnUntil;

// ---------------------------------------------------------------------------------------------------------------
// Workload functions. Global, unmangled, noinline: they are the "known answers" a profiler must find.
// Each uses a distinct Salt so identical-code folding cannot merge them.
// ---------------------------------------------------------------------------------------------------------------
extern "C" {

// spin
TACHYON_NOINLINE uint64_t hot_a(uint64_t iterations, uint64_t state) {
  return burn<0xA1ull>(state, iterations);
}
TACHYON_NOINLINE uint64_t hot_b(uint64_t iterations, uint64_t state) {
  return burn<0xB2ull>(state, iterations);
}
TACHYON_NOINLINE uint64_t hot_c(uint64_t iterations, uint64_t state) {
  return burn<0xC3ull>(state, iterations);
}
TACHYON_NOINLINE uint64_t spin_rest(uint64_t iterations, uint64_t state) {
  return burn<0xD4ull>(state, iterations);
}

// sleep
TACHYON_NOINLINE uint64_t sleep_busy_work(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0x51ull>(deadlineTsc, state);
}
TACHYON_NOINLINE void sleep_wait(uint64_t nanoseconds) {
  std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
}

// lock
TACHYON_NOINLINE void lock_wait(std::mutex* mutex) {
  mutex->lock();
}
TACHYON_NOINLINE uint64_t lock_critical_section(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0x1C1ull>(deadlineTsc, state);
}
TACHYON_NOINLINE uint64_t lock_outside_work(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0x1C2ull>(deadlineTsc, state);
}

// io
TACHYON_NOINLINE uint64_t io_read_file(std::ifstream* file, char* buffer, uint64_t size) {
  file->read(buffer, static_cast<std::streamsize>(size));
  return static_cast<uint64_t>(file->gcount());
}
TACHYON_NOINLINE uint64_t io_checksum(const char* buffer, uint64_t size, uint64_t state) {
  for (uint64_t i = 0; i + 8 <= size; i += 8) {
    uint64_t word;
    std::memcpy(&word, buffer + i, 8);
    state = (state ^ word) * 0x100000001B3ull;
  }
  return state;
}

// recursion
TACHYON_NOINLINE uint64_t recurse_leaf(uint64_t iterations, uint64_t state, uint64_t* leafTicks) {
  const uint64_t t0 = tachyon::testbed::readTsc();
  state = burn<0xEC1ull>(state, iterations);
  *leafTicks += tachyon::testbed::readTsc() - t0;
  return state;
}
TACHYON_NOINLINE uint64_t recurse_level(uint32_t depth, uint64_t iterations, uint64_t state,
                                        uint64_t* leafTicks) {
  const uint64_t result = depth <= 1 ? recurse_leaf(iterations, state, leafTicks)
                                     : recurse_level(depth - 1, iterations, state + depth, leafTicks);
  return ((result << 1) | (result >> 63)) ^
         depth;  // non-associative post-work: the call cannot become a loop
}

// jobs
TACHYON_NOINLINE uint64_t job_execute(uint64_t iterations, uint64_t state) {
  return burn<0x10Bull>(state, iterations);
}

// frames
TACHYON_NOINLINE uint64_t frame_update(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0xF01ull>(deadlineTsc, state);
}
TACHYON_NOINLINE uint64_t frame_render(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0xF02ull>(deadlineTsc, state);
}
TACHYON_NOINLINE uint64_t frame_hitch(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0xF03ull>(deadlineTsc, state);
}
TACHYON_NOINLINE void frame_wait(uint64_t nanoseconds) {
  std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
}

// fibers: four distinct work functions, one per fiber, so each fiber is separately attributable by function.
TACHYON_NOINLINE uint64_t fiber_work_0(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0xF1B0ull>(deadlineTsc, state);
}
TACHYON_NOINLINE uint64_t fiber_work_1(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0xF1B1ull>(deadlineTsc, state);
}
TACHYON_NOINLINE uint64_t fiber_work_2(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0xF1B2ull>(deadlineTsc, state);
}
TACHYON_NOINLINE uint64_t fiber_work_3(uint64_t deadlineTsc, uint64_t state) {
  return burnUntil<0xF1B3ull>(deadlineTsc, state);
}

}  // extern "C"

namespace tachyon::testbed {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kSpinCycleS = 0.001;
constexpr double kSleepCycleS = 0.010;
constexpr double kSleepFraction = 0.30;
constexpr double kInlineCycleS = 0.001;
constexpr double kInlineShares[3] = {0.5, 0.3, 0.2};
constexpr uint32_t kRecursionDepth = 50;
constexpr double kRecursionLeafS = 0.0002;
constexpr uint32_t kFiberCount = 4;
constexpr double kFiberSliceS = 0.001;
constexpr uint32_t kJobWorkers = 8;
constexpr double kJobPeriodS = 0.001;
constexpr double kJobCostS = 0.00025;
constexpr uint64_t kIoFileBytes = 8ull << 20;
constexpr uint64_t kIoChunkBytes = 64ull << 10;

double elapsedSeconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

uint64_t secondsToNs(double seconds) {
  return static_cast<uint64_t>(seconds * 1e9);
}

void identifyThread(ThreadReport& report, const std::string& name) {
  setCurrentThreadName(name);
  onThreadNamed(name);
  report.name = name;
  report.tid = currentThreadId();
}

// Sink for the final burn state so the optimizer cannot discard the workload; reported as "checksum".
nlohmann::json checksumOf(uint64_t value) {
  return value;
}

ScenarioReport beginReport(const char* name, const char* description, double durationS) {
  ScenarioReport report;
  report.name = name;
  report.description = description;
  report.plannedDurationS = durationS;
  return report;
}

// ---- spin
// -------------------------------------------------------------------------------------------------------

ThreadReport spinImpl(const Calibration& cal, double durationS, uint64_t* checksum) {
  ThreadReport report;
  const auto counts = loopCountsForShares(kSpinShares, secondsToTsc(cal, kSpinCycleS), cal.itersPerTsc);
  uint64_t ticks[4] = {0, 0, 0, 0};
  uint64_t state = 1;
  const uint64_t deadline = readTsc() + secondsToTsc(cal, durationS);
  uint64_t t = readTsc();
  while (t < deadline) {
    TB_SCOPE("spin_cycle");
    {
      TB_SCOPE("hot_a");
      state = hot_a(counts[0], state);
    }
    uint64_t next = readTsc();
    ticks[0] += next - t;
    t = next;
    {
      TB_SCOPE("hot_b");
      state = hot_b(counts[1], state);
    }
    next = readTsc();
    ticks[1] += next - t;
    t = next;
    {
      TB_SCOPE("hot_c");
      state = hot_c(counts[2], state);
    }
    next = readTsc();
    ticks[2] += next - t;
    t = next;
    {
      TB_SCOPE("spin_rest");
      state = spin_rest(counts[3], state);
    }
    next = readTsc();
    ticks[3] += next - t;
    t = next;
  }
  report.enforced = true;
  report.tolerance = 0.05;
  report.expected = makeExpected({{"hot_a", kSpinShares[0]},
                                  {"hot_b", kSpinShares[1]},
                                  {"hot_c", kSpinShares[2]},
                                  {"spin_rest", kSpinShares[3]}});
  report.measured =
      makeMeasured({{"hot_a", ticks[0]}, {"hot_b", ticks[1]}, {"hot_c", ticks[2]}, {"spin_rest", ticks[3]}});
  if (checksum != nullptr) *checksum = state;
  return report;
}

Result<ScenarioReport> runSpin(const RunConfig& config, const Calibration& cal) {
  ScenarioReport report =
      beginReport("spin", "One thread: hot_a 40%, hot_b 20%, hot_c 10%, spin_rest 30%.", config.durationS);
  ThreadReport thread;
  uint64_t checksum = 0;
  std::thread worker([&] {
    // Name the thread before the workload so samples taken during the run see "tb-spin".
    ThreadReport identity;
    identifyThread(identity, "tb-spin");
    thread = spinImpl(cal, config.durationS, &checksum);
    thread.name = identity.name;
    thread.tid = identity.tid;
  });
  worker.join();
  report.threads.push_back(std::move(thread));
  report.details = {{"cycle_ms", kSpinCycleS * 1e3}, {"checksum", checksumOf(checksum)}};
  return report;
}

// ---- sleep
// ------------------------------------------------------------------------------------------------------

Result<ScenarioReport> runSleep(const RunConfig& config, const Calibration& cal) {
  ScenarioReport report =
      beginReport("sleep", "One thread that sleeps 30% of every 10 ms cycle.", config.durationS);
  ThreadReport thread;
  uint64_t checksum = 0;
  std::thread worker([&] {
    identifyThread(thread, "tb-sleep");
    const uint64_t cycleTsc = secondsToTsc(cal, kSleepCycleS);
    const uint64_t busyTsc = static_cast<uint64_t>(static_cast<double>(cycleTsc) * (1.0 - kSleepFraction));
    uint64_t busyTicks = 0, sleepTicks = 0, state = 1;
    const uint64_t deadline = readTsc() + secondsToTsc(cal, config.durationS);
    uint64_t t = readTsc();
    while (t < deadline) {
      state = sleep_busy_work(t + busyTsc, state);
      uint64_t next = readTsc();
      busyTicks += next - t;
      t = next;
      sleep_wait(secondsToNs(kSleepCycleS * kSleepFraction));
      next = readTsc();
      sleepTicks += next - t;
      t = next;
    }
    thread.enforced = true;
    thread.tolerance = 0.05;
    thread.expected =
        makeExpected({{"sleep_busy_work", 1.0 - kSleepFraction}, {"sleep_wait", kSleepFraction}});
    thread.measured = makeMeasured({{"sleep_busy_work", busyTicks}, {"sleep_wait", sleepTicks}});
    checksum = state;
  });
  worker.join();
  report.threads.push_back(std::move(thread));
  report.details = {{"cycle_ms", kSleepCycleS * 1e3},
                    {"sleep_fraction", kSleepFraction},
                    {"checksum", checksumOf(checksum)}};
  return report;
}

// ---- lock
// -------------------------------------------------------------------------------------------------------

Result<ScenarioReport> runLock(const RunConfig& config, const Calibration& cal) {
  const LockModel model;
  const LockExpectation expect = lockExpectation(model);
  ScenarioReport report = beginReport(
      "lock", "Two threads contend on one mutex; the holder runs lock_critical_section.", config.durationS);
  std::mutex mutex;
  ThreadReport threads[2];
  uint64_t acquisitions[2] = {0, 0};
  uint64_t checksums[2] = {0, 0};
  const uint64_t deadline = readTsc() + secondsToTsc(cal, config.durationS);
  const uint64_t holdTsc = secondsToTsc(cal, model.holdMs * 1e-3);
  const uint64_t outsideTsc = secondsToTsc(cal, model.outsideMs * 1e-3);
  std::thread workers[2];
  for (int i = 0; i < 2; ++i) {
    workers[i] = std::thread([&, i] {
      ThreadReport& thread = threads[i];
      identifyThread(thread, "tb-lock-" + std::to_string(i));
      uint64_t waitTicks = 0, criticalTicks = 0, outsideTicks = 0, state = static_cast<uint64_t>(i) + 1;
      uint64_t t = readTsc();
      while (t < deadline) {
        {
          InstLockScope waiting(&mutex, "testbed.lock");
          lock_wait(&mutex);
        }
        uint64_t next = readTsc();
        waitTicks += next - t;
        t = next;
        {
          TB_SCOPE("lock_critical_section");
          state = lock_critical_section(t + holdTsc, state);
        }
        ++acquisitions[i];
        mutex.unlock();
        next = readTsc();
        criticalTicks += next - t;
        t = next;
        state = lock_outside_work(t + outsideTsc, state);
        next = readTsc();
        outsideTicks += next - t;
        t = next;
      }
      thread.enforced = false;  // fairness of the mutex decides the split
      thread.tolerance = 0.35;
      thread.expected = makeExpected({{"lock_critical_section", expect.critical},
                                      {"lock_wait", expect.wait},
                                      {"lock_outside_work", expect.outside}});
      thread.measured = makeMeasured({{"lock_critical_section", criticalTicks},
                                      {"lock_wait", waitTicks},
                                      {"lock_outside_work", outsideTicks}});
      checksums[i] = state;
    });
  }
  for (auto& worker : workers) worker.join();
  for (auto& thread : threads) report.threads.push_back(std::move(thread));
  report.details = {{"holder_function", "lock_critical_section"},
                    {"waiter_function", "lock_wait"},
                    {"hold_ms", model.holdMs},
                    {"outside_ms", model.outsideMs},
                    {"acquisitions", {acquisitions[0], acquisitions[1]}},
                    {"checksum", checksumOf(checksums[0] ^ checksums[1])}};
  return report;
}

// ---- io
// ---------------------------------------------------------------------------------------------------------

Result<ScenarioReport> runIo(const RunConfig& config, const Calibration& cal) {
  ScenarioReport report =
      beginReport("io", "One thread reading an 8 MiB temp file in 64 KiB chunks, looping.", config.durationS);
  const auto path = std::filesystem::temp_directory_path() /
                    ("tachyon-testbed-io-" + std::to_string(currentThreadId()) + ".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    uint64_t x = 0x243F6A8885A308D3ull;
    std::vector<uint64_t> block(kIoChunkBytes / 8);
    for (uint64_t written = 0; written < kIoFileBytes && out; written += kIoChunkBytes) {
      for (auto& word : block) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        word = x;
      }
      out.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(kIoChunkBytes));
    }
    if (!out) return makeError(ErrorCode::IoError, "cannot write temp file " + path.string());
  }
  ThreadReport thread;
  uint64_t bytesRead = 0, checksum = 0;
  bool openFailed = false;
  std::thread worker([&] {
    identifyThread(thread, "tb-io");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      openFailed = true;
      return;
    }
    std::vector<char> buffer(kIoChunkBytes);
    uint64_t readTicks = 0, checksumTicks = 0, state = 0xCBF29CE484222325ull;
    const uint64_t deadline = readTsc() + secondsToTsc(cal, config.durationS);
    uint64_t t = readTsc();
    while (t < deadline) {
      const uint64_t got = io_read_file(&in, buffer.data(), kIoChunkBytes);
      uint64_t next = readTsc();
      readTicks += next - t;
      t = next;
      if (got == 0) {  // end of file: rewind and keep looping
        in.clear();
        in.seekg(0);
        continue;
      }
      bytesRead += got;
      state = io_checksum(buffer.data(), got, state);
      next = readTsc();
      checksumTicks += next - t;
      t = next;
    }
    thread.enforced = false;  // kernel read cost versus user checksum cost is machine dependent
    thread.tolerance = 0.5;
    thread.expected = makeExpected({{"io_read_file", 0.5}, {"io_checksum", 0.5}});
    thread.measured = makeMeasured({{"io_read_file", readTicks}, {"io_checksum", checksumTicks}});
    checksum = state;
  });
  worker.join();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (openFailed) return makeError(ErrorCode::IoError, "cannot open temp file " + path.string());
  report.threads.push_back(std::move(thread));
  report.details = {{"file_bytes", kIoFileBytes},
                    {"chunk_bytes", kIoChunkBytes},
                    {"bytes_read", bytesRead},
                    {"checksum", checksumOf(checksum)}};
  return report;
}

// ---- recursion
// --------------------------------------------------------------------------------------------------

Result<ScenarioReport> runRecursion(const RunConfig& config, const Calibration& cal) {
  ScenarioReport report = beginReport(
      "recursion", "One thread recursing 50 deep (recurse_level) to a hot recurse_leaf.", config.durationS);
  ThreadReport thread;
  uint64_t checksum = 0;
  std::thread worker([&] {
    identifyThread(thread, "tb-recursion");
    const uint64_t leafIters =
        static_cast<uint64_t>(kRecursionLeafS * static_cast<double>(cal.tscHz) * cal.itersPerTsc);
    uint64_t leafTicks = 0, totalTicks = 0, state = 1;
    const uint64_t deadline = readTsc() + secondsToTsc(cal, config.durationS);
    uint64_t t = readTsc();
    while (t < deadline) {
      state = recurse_level(kRecursionDepth, leafIters, state, &leafTicks);
      const uint64_t next = readTsc();
      totalTicks += next - t;
      t = next;
    }
    thread.enforced = true;
    thread.tolerance = 0.05;
    thread.expected = makeExpected({{"recurse_leaf", 1.0}, {"recurse_level", 0.0}});
    thread.measured = makeMeasured({{"recurse_leaf", leafTicks}, {"recurse_level", totalTicks - leafTicks}});
    checksum = state;
  });
  worker.join();
  report.threads.push_back(std::move(thread));
  report.details = {{"depth", kRecursionDepth}, {"checksum", checksumOf(checksum)}};
  return report;
}

// ---- inline
// -----------------------------------------------------------------------------------------------------

static TACHYON_FORCEINLINE uint64_t inline_a(uint64_t iterations, uint64_t state) {
  return burn<0x1A1ull>(state, iterations);
}
static TACHYON_FORCEINLINE uint64_t inline_b(uint64_t iterations, uint64_t state) {
  return burn<0x1B2ull>(state, iterations);
}
static TACHYON_FORCEINLINE uint64_t inline_c(uint64_t iterations, uint64_t state) {
  return burn<0x1C3ull>(state, iterations);
}

}  // namespace
}  // namespace tachyon::testbed

// The hot loop itself is a real (noinline) frame; inline_a/b/c are folded into it in optimized builds.
extern "C" TACHYON_NOINLINE uint64_t inline_hot_loop(uint64_t deadlineTsc, const uint64_t* counts,
                                                     uint64_t* ticks, uint64_t state) {
  using namespace tachyon::testbed;
  uint64_t t = readTsc();
  while (t < deadlineTsc) {
    state = inline_a(counts[0], state);
    uint64_t next = readTsc();
    ticks[0] += next - t;
    t = next;
    state = inline_b(counts[1], state);
    next = readTsc();
    ticks[1] += next - t;
    t = next;
    state = inline_c(counts[2], state);
    next = readTsc();
    ticks[2] += next - t;
    t = next;
  }
  return state;
}

namespace tachyon::testbed {
namespace {

Result<ScenarioReport> runInline(const RunConfig& config, const Calibration& cal) {
  ScenarioReport report = beginReport(
      "inline", "One thread in inline_hot_loop with force-inlined inline_a 50%, inline_b 30%, inline_c 20%.",
      config.durationS);
  ThreadReport thread;
  uint64_t checksum = 0;
  std::thread worker([&] {
    identifyThread(thread, "tb-inline");
    const auto counts = loopCountsForShares(kInlineShares, secondsToTsc(cal, kInlineCycleS), cal.itersPerTsc);
    uint64_t ticks[3] = {0, 0, 0};
    checksum = inline_hot_loop(readTsc() + secondsToTsc(cal, config.durationS), counts.data(), ticks, 1);
    thread.enforced = true;
    thread.tolerance = 0.05;
    thread.expected = makeExpected(
        {{"inline_a", kInlineShares[0]}, {"inline_b", kInlineShares[1]}, {"inline_c", kInlineShares[2]}});
    thread.measured = makeMeasured({{"inline_a", ticks[0]}, {"inline_b", ticks[1]}, {"inline_c", ticks[2]}});
  });
  worker.join();
  report.threads.push_back(std::move(thread));
  report.details = {{"hot_loop_function", "inline_hot_loop"},
                    {"inlined_functions", {"inline_a", "inline_b", "inline_c"}},
                    {"checksum", checksumOf(checksum)}};
  return report;
}

// ---- fibers
// -----------------------------------------------------------------------------------------------------

// Fiber ids reported to the SDK: the worker thread's own context and fibers 1..kFiberCount.
constexpr uint64_t kMainFiberId = 100;

struct FiberCtx {
  FiberGroup* group = nullptr;
  uint64_t fiberId = 0;
  uint64_t sliceTsc = 0;
  uint64_t state = 1;
};

template <uint64_t (*WorkFn)(uint64_t, uint64_t)>
void fiberBody(void* arg) {
  auto* ctx = static_cast<FiberCtx*>(arg);
  for (;;) {
    onFiberSwitchEnd(ctx->fiberId);  // this fiber was just resumed
    {
      TB_SCOPE("fiber_slice");
      ctx->state = WorkFn(readTsc() + ctx->sliceTsc, ctx->state);
    }
    onFiberSwitchBegin(ctx->fiberId, kMainFiberId);
    ctx->group->yield();
  }
}

Result<ScenarioReport> runFibers(const RunConfig& config, const Calibration& cal) {
  ScenarioReport report = beginReport(
      "fibers", "One thread round-robining 4 fibers (fiber_work_0..3), equal slices.", config.durationS);
  ThreadReport thread;
  bool setupFailed = false;
  uint64_t checksum = 0;
  std::thread worker([&] {
    identifyThread(thread, "tb-fibers");
    FiberGroup group;
    if (!group.ok()) {
      setupFailed = true;
      return;
    }
    FiberCtx ctx[kFiberCount];
    const FiberGroup::Entry entries[kFiberCount] = {&fiberBody<&fiber_work_0>, &fiberBody<&fiber_work_1>,
                                                    &fiberBody<&fiber_work_2>, &fiberBody<&fiber_work_3>};
    for (uint32_t i = 0; i < kFiberCount; ++i) {
      ctx[i].group = &group;
      ctx[i].fiberId = i + 1;
      ctx[i].sliceTsc = secondsToTsc(cal, kFiberSliceS);
      ctx[i].state = i + 1;
      if (group.add(entries[i], &ctx[i]) < 0) {
        setupFailed = true;
        return;
      }
    }
    uint64_t ticks[kFiberCount] = {};
    onFiberRegister(kMainFiberId);
    for (uint32_t i = 0; i < kFiberCount; ++i) onFiberRegister(i + 1);
    const uint64_t deadline = readTsc() + secondsToTsc(cal, config.durationS);
    uint64_t t = readTsc();
    for (uint32_t turn = 0; t < deadline; ++turn) {
      const uint32_t i = turn % kFiberCount;
      onFiberSwitchBegin(kMainFiberId, i + 1);
      group.resume(static_cast<int>(i));
      onFiberSwitchEnd(kMainFiberId);
      const uint64_t next = readTsc();
      ticks[i] += next - t;
      t = next;
    }
    for (uint32_t i = 0; i < kFiberCount; ++i) onFiberUnregister(i + 1);
    onFiberUnregister(kMainFiberId);
    thread.enforced = true;
    thread.tolerance = 0.05;
    thread.expected = makeExpected(
        {{"fiber_work_0", 0.25}, {"fiber_work_1", 0.25}, {"fiber_work_2", 0.25}, {"fiber_work_3", 0.25}});
    thread.measured = makeMeasured({{"fiber_work_0", ticks[0]},
                                    {"fiber_work_1", ticks[1]},
                                    {"fiber_work_2", ticks[2]},
                                    {"fiber_work_3", ticks[3]}});
    for (const auto& c : ctx) checksum ^= c.state;
  });
  worker.join();
  if (setupFailed) return makeError(ErrorCode::Unsupported, "fiber setup failed");
  report.threads.push_back(std::move(thread));
  report.details = {
      {"fiber_count", kFiberCount}, {"slice_ms", kFiberSliceS * 1e3}, {"checksum", checksumOf(checksum)}};
  return report;
}

// ---- jobs
// -------------------------------------------------------------------------------------------------------

struct JobQueue {
  std::mutex mutex;
  std::condition_variable cv;
  uint64_t pending = 0;
  bool stop = false;
};

// Blocks until a job is available (returns true) or the queue is stopped and drained (returns false).
TACHYON_NOINLINE bool jobWait(JobQueue* queue) {
  std::unique_lock lock(queue->mutex);
  queue->cv.wait(lock, [&] { return queue->pending > 0 || queue->stop; });
  if (queue->pending == 0) return false;
  --queue->pending;
  return true;
}

Result<ScenarioReport> runJobs(const RunConfig& config, const Calibration& cal) {
  ScenarioReport report = beginReport(
      "jobs", "8 worker threads on one condition variable; a producer posts 8 x 0.25 ms jobs every 1 ms.",
      config.durationS);
  JobQueue queue;
  ThreadReport workers[kJobWorkers];
  uint64_t checksums[kJobWorkers] = {};
  uint64_t jobsRun[kJobWorkers] = {};
  const uint64_t jobIters =
      static_cast<uint64_t>(kJobCostS * static_cast<double>(cal.tscHz) * cal.itersPerTsc);
  std::thread threads[kJobWorkers];
  for (uint32_t i = 0; i < kJobWorkers; ++i) {
    threads[i] = std::thread([&, i] {
      identifyThread(workers[i], "tb-job-" + std::to_string(i));
      uint64_t waitTicks = 0, execTicks = 0, state = i + 1;
      uint64_t t = readTsc();
      for (;;) {
        bool gotJob = false;
        {
          TB_SCOPE("job_wait");
          gotJob = jobWait(&queue);
        }
        uint64_t next = readTsc();
        waitTicks += next - t;
        t = next;
        if (!gotJob) break;
        {
          TB_SCOPE("job_execute");
          state = job_execute(jobIters, state);
        }
        ++jobsRun[i];
        next = readTsc();
        execTicks += next - t;
        t = next;
      }
      const double execShare = kJobCostS * kJobWorkers / kJobPeriodS / kJobWorkers;
      workers[i].enforced = false;  // wake-up distribution decides which worker gets which job
      workers[i].tolerance = 0.15;
      workers[i].expected = makeExpected({{"job_execute", execShare}, {"job_wait", 1.0 - execShare}});
      workers[i].measured = makeMeasured({{"job_execute", execTicks}, {"job_wait", waitTicks}});
      checksums[i] = state;
    });
  }
  ThreadReport producer;
  std::thread producerThread([&] {
    identifyThread(producer, "tb-job-prod");
    const auto end = Clock::now() + std::chrono::nanoseconds(secondsToNs(config.durationS));
    while (Clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(secondsToNs(kJobPeriodS)));
      {
        std::lock_guard lock(queue.mutex);
        queue.pending += kJobWorkers;
      }
      queue.cv.notify_all();
    }
    {
      std::lock_guard lock(queue.mutex);
      queue.stop = true;
    }
    queue.cv.notify_all();
  });
  producerThread.join();
  for (auto& thread : threads) thread.join();
  uint64_t checksum = 0, total = 0;
  for (uint32_t i = 0; i < kJobWorkers; ++i) {
    checksum ^= checksums[i];
    total += jobsRun[i];
    report.threads.push_back(std::move(workers[i]));
  }
  report.details = {{"worker_count", kJobWorkers},
                    {"producer", {{"name", producer.name}, {"tid", producer.tid}}},
                    {"job_cost_ms", kJobCostS * 1e3},
                    {"jobs_run", total},
                    {"checksum", checksumOf(checksum)}};
  return report;
}

// ---- frames
// -----------------------------------------------------------------------------------------------------

Result<ScenarioReport> runFrames(const RunConfig& config, const Calibration& cal) {
  FrameModel model;
  model.hitchIntervalMs = config.hitchIntervalMs;
  const FrameExpectation expect = frameExpectation(model);
  ScenarioReport report = beginReport(
      "frames", "Simulated 60 Hz frame loop (update/render/wait) with a 50 ms hitch periodically.",
      config.durationS);
  ThreadReport thread;
  uint64_t frames = 0, hitches = 0, checksum = 0;
  double maxFrameMs = 0.0;
  std::thread worker([&] {
    identifyThread(thread, "tb-frames");
    const uint64_t periodTsc = secondsToTsc(cal, model.periodMs * 1e-3);
    const uint64_t updateTsc = secondsToTsc(cal, model.updateMs * 1e-3);
    const uint64_t renderTsc = secondsToTsc(cal, model.renderMs * 1e-3);
    const uint64_t hitchTsc = secondsToTsc(cal, model.hitchMs * 1e-3);
    uint64_t updateTicks = 0, renderTicks = 0, hitchTicks = 0, waitTicks = 0, state = 1;
    const uint64_t deadline = readTsc() + secondsToTsc(cal, config.durationS);
    uint64_t frameStart = readTsc();
    while (frameStart < deadline) {
      onFrameBegin(frames);
      TB_SCOPE("frame");
      const bool hitchFrame = frames % expect.framesPerHitch == expect.framesPerHitch - 1;
      uint64_t t = frameStart;
      {
        TB_SCOPE("frame_update");
        state = frame_update(t + updateTsc, state);
      }
      uint64_t next = readTsc();
      updateTicks += next - t;
      t = next;
      {
        TB_SCOPE("frame_render");
        state = frame_render(t + renderTsc, state);
      }
      next = readTsc();
      renderTicks += next - t;
      t = next;
      if (hitchFrame) {
        onHitch(frames);
        {
          TB_SCOPE("frame_hitch");
          state = frame_hitch(t + hitchTsc, state);
        }
        next = readTsc();
        hitchTicks += next - t;
        t = next;
        ++hitches;
      }
      const uint64_t target = frameStart + periodTsc;
      if (t < target) {
        {
          TB_SCOPE("frame_wait");
          frame_wait(secondsToNs(tscToSeconds(cal, target - t)));
        }
        next = readTsc();
        waitTicks += next - t;
        t = next;
      }
      maxFrameMs = std::max(maxFrameMs, tscToSeconds(cal, t - frameStart) * 1e3);
      onFrameTime(tscToSeconds(cal, t - frameStart) * 1e3);
      onFrameEnd(frames);
      ++frames;
      // On time: keep the 60 Hz cadence. Late (hitch): start the next frame now, no catch-up.
      frameStart = t > target ? t : target;
    }
    thread.enforced = true;
    thread.tolerance = 0.05;
    thread.expected = makeExpected({{"frame_update", expect.update},
                                    {"frame_render", expect.render},
                                    {"frame_hitch", expect.hitch},
                                    {"frame_wait", expect.wait}});
    thread.measured = makeMeasured({{"frame_update", updateTicks},
                                    {"frame_render", renderTicks},
                                    {"frame_hitch", hitchTicks},
                                    {"frame_wait", waitTicks}});
    checksum = state;
  });
  worker.join();
  report.threads.push_back(std::move(thread));
  report.details = {{"frame_period_ms", model.periodMs},
                    {"update_ms", model.updateMs},
                    {"render_ms", model.renderMs},
                    {"hitch_ms", model.hitchMs},
                    {"hitch_interval_ms", model.hitchIntervalMs},
                    {"frames_per_hitch", expect.framesPerHitch},
                    {"frames", frames},
                    {"hitches", hitches},
                    {"max_frame_ms", maxFrameMs},
                    {"checksum", checksumOf(checksum)}};
  return report;
}

}  // namespace

// ---- public API
// -------------------------------------------------------------------------------------------------

FrameExpectation frameExpectation(const FrameModel& model) {
  FrameExpectation e;
  e.framesPerHitch = static_cast<uint32_t>(std::max(1.0, std::round(model.hitchIntervalMs / model.periodMs)));
  const double n = e.framesPerHitch;
  const double normalBusy = model.updateMs + model.renderMs;
  const double hitchFrameMs = std::max(model.periodMs, normalBusy + model.hitchMs);
  e.cycleMs = (n - 1.0) * model.periodMs + hitchFrameMs;
  e.update = n * model.updateMs / e.cycleMs;
  e.render = n * model.renderMs / e.cycleMs;
  e.hitch = model.hitchMs / e.cycleMs;
  e.wait = std::max(0.0, 1.0 - e.update - e.render - e.hitch);
  return e;
}

LockExpectation lockExpectation(const LockModel& model) {
  // Two threads strictly alternate; the mutex is always held, so one thread's cycle is 2 * holdMs.
  LockExpectation e;
  const double cycleMs = 2.0 * model.holdMs;
  e.critical = model.holdMs / cycleMs;
  e.outside = std::min(model.outsideMs / cycleMs, 1.0 - e.critical);
  e.wait = 1.0 - e.critical - e.outside;
  return e;
}

std::vector<std::string> scenarioNames() {
  return {"spin", "sleep", "lock", "io", "recursion", "inline", "fibers", "jobs", "frames"};
}

std::vector<double> planDurations(const std::vector<std::string>& names, double totalS) {
  std::vector<double> weights;
  double sum = 0.0;
  for (const auto& name : names) {
    weights.push_back(name == "frames" ? 4.0 : 1.0);
    sum += weights.back();
  }
  for (double& w : weights) w = sum > 0.0 ? totalS * w / sum : 0.0;
  return weights;
}

ThreadReport spinWorker(const Calibration& cal, double durationS) {
  return spinImpl(cal, durationS, nullptr);
}

Result<ScenarioReport> runScenario(std::string_view name, const RunConfig& config, const Calibration& cal) {
  using Fn = Result<ScenarioReport> (*)(const RunConfig&, const Calibration&);
  struct Entry {
    std::string_view name;
    Fn fn;
  };
  static const Entry kEntries[] = {
      {"spin", &runSpin},     {"sleep", &runSleep},         {"lock", &runLock},
      {"io", &runIo},         {"recursion", &runRecursion}, {"inline", &runInline},
      {"fibers", &runFibers}, {"jobs", &runJobs},           {"frames", &runFrames}};
  for (const Entry& entry : kEntries) {
    if (entry.name != name) continue;
    const std::string nameStr(name);
    onScenarioBegin(nameStr.c_str());
    const auto start = Clock::now();
    Result<ScenarioReport> result = makeError(ErrorCode::Internal, "scenario did not run");
    try {
      result = entry.fn(config, cal);
    } catch (const std::exception& e) {
      result = makeError(ErrorCode::Internal, nameStr + ": " + e.what());
    }
    onScenarioEnd(nameStr.c_str());
    if (result.ok()) result.value().actualDurationS = elapsedSeconds(start);
    return result;
  }
  return makeError(ErrorCode::InvalidArgument, "unknown scenario '" + std::string(name) + "'");
}

Result<RunReport> runScenarios(const std::vector<std::string>& names, double totalS, double hitchIntervalMs,
                               const Calibration& cal) {
  RunReport run;
  run.platform = platformName();
  run.pid = currentProcessId();
  run.requestedDurationS = totalS;
  run.clockFrequencyHz = cal.traceFrequencyHz;
  run.tscFrequencyHz = cal.tscHz;
  run.kernelItersPerSecond = cal.itersPerSecond();
  const bool multi = names.size() > 1;
  const std::vector<double> durations = multi ? planDurations(names, totalS) : std::vector<double>{totalS};
  for (size_t i = 0; i < names.size(); ++i) {
    RunConfig config;
    config.durationS = durations[i];
    config.hitchIntervalMs =
        hitchIntervalMs > 0.0 ? hitchIntervalMs : (multi ? std::min(2000.0, durations[i] * 500.0) : 2000.0);
    auto result = runScenario(names[i], config, cal);
    if (!result.ok()) return std::move(result).error();
    run.scenarios.push_back(std::move(result).value());
  }
  return run;
}

}  // namespace tachyon::testbed
