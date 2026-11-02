// WP-71: real scheduler-event captures of the testbed (label linux_privileged).
//
//  * Tracepoint tests (sched:sched_switch / sched_waking): they need tracefs for the tracepoint ids and
//  formats
//    and a kernel that allows tracepoint events (kernel.perf_event_paranoid <= -1, or CAP_PERFMON). They
//    skip, with the reason, when either is missing (a container without tracefs, the development sandbox); on
//    the CI runner (tracefs mounted, paranoid -1) they must run. When tracefs exists but this user cannot
//    read it, the test copies the three format files with `sudo -n cat` into a temporary tree and points the
//    backend there (the tracepoint ids are the only thing it needs).
//  * Fallback tests (attr.context_switch / PERF_RECORD_SWITCH): need only perf_event_open, so they run
//    wherever the WP-70 tests run, tracefs or not.
//
// The workload is the testbed's `lock` scenario: two threads (tb-lock-0, tb-lock-1) alternate on one mutex,
// the unlocking thread is the waker of the waiting one.
#include "analysis/thread_states.h"
#include "capture/linux_perf/launch.h"
#include "capture/linux_perf/linux_perf_backend.h"
#include "model/model.h"

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;
using model::ThreadState;

class TempDir {
 public:
  TempDir() {
    m_path = fs::temp_directory_path() / ("tachyon-sched-live-" + std::to_string(::getpid()));
    fs::remove_all(m_path);
    fs::create_directories(m_path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(m_path, ec);
  }
  const fs::path& path() const { return m_path; }

 private:
  fs::path m_path;
};

std::string readFile(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Runs `sudo -n cat <path>` and returns its output (empty when sudo or the file is not available).
std::string sudoCat(const fs::path& path) {
  const std::string cmd = "sudo -n cat '" + path.string() + "' 2>/dev/null";
  std::FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return {};
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
  ::pclose(pipe);
  return out;
}

// The tracefs root the backend should read the formats from: empty = "let the backend find it"; sets `reason`
// and returns false when the tracepoint path cannot work here at all.
bool locateTracefs(const fs::path& scratch, fs::path& root, std::string& reason) {
  root.clear();
  const auto system = perf::makeLinuxPerfSystem();
  const fs::path direct = system->tracefsRoot();
  if (!direct.empty() && std::ifstream(direct / "events/sched/sched_switch/format").good()) return true;
  // Present but unreadable for this user?
  for (const char* candidate : {"/sys/kernel/tracing", "/sys/kernel/debug/tracing"}) {
    const fs::path base(candidate);
    const std::string text = sudoCat(base / "events/sched/sched_switch/format");
    if (text.empty()) continue;
    for (const char* event : {"sched_switch", "sched_waking", "sched_wakeup"}) {
      const std::string fmt =
          event == std::string("sched_switch") ? text : sudoCat(base / "events/sched" / event / "format");
      if (fmt.empty()) continue;
      fs::create_directories(scratch / "tracefs/events/sched" / event);
      std::ofstream(scratch / "tracefs/events/sched" / event / "format") << fmt;
    }
    root = scratch / "tracefs";
    return true;
  }
  reason =
      "tracefs is not available (/sys/kernel/tracing and /sys/kernel/debug/tracing have no sched events, or "
      "are "
      "not readable even with sudo): the sched tracepoints cannot be used on this machine";
  return false;
}

#define REQUIRE_SYSTEM_WIDE_PERF()                                                                      \
  do {                                                                                                  \
    LinuxPerfBackend probe;                                                                             \
    auto priv = probe.checkPrivileges();                                                                \
    if (!priv.ok())                                                                                     \
      GTEST_SKIP() << "system-wide perf_event_open is not available here: " << priv.error().toString(); \
  } while (0)

// The first cpu this process may run on.
int firstAllowedCpu() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof(set), &set) != 0) return -1;
  for (int c = 0; c < CPU_SETSIZE; ++c)
    if (CPU_ISSET(c, &set)) return c;
  return -1;
}

void pinTask(pid_t tid, int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  ::sched_setaffinity(tid, sizeof(set), &set);
}

struct ThreadCounters {
  uint64_t voluntary = 0, nonvoluntary = 0;
};

// tid -> context-switch counters of the threads of `pid` whose comm starts with `prefix`.
std::map<uint32_t, ThreadCounters> readCounters(uint32_t pid, const std::string& prefix) {
  std::map<uint32_t, ThreadCounters> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator("/proc/" + std::to_string(pid) + "/task", ec)) {
    const uint32_t tid = static_cast<uint32_t>(std::stoul(e.path().filename().string()));
    std::string comm = readFile(e.path() / "comm");
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == ' ')) comm.pop_back();
    if (comm.rfind(prefix, 0) != 0) continue;
    std::ifstream status(e.path() / "status");
    std::string line;
    ThreadCounters c;
    while (std::getline(status, line)) {
      if (line.rfind("voluntary_ctxt_switches:", 0) == 0) c.voluntary = std::stoull(line.substr(24));
      if (line.rfind("nonvoluntary_ctxt_switches:", 0) == 0) c.nonvoluntary = std::stoull(line.substr(27));
    }
    out[tid] = c;
  }
  return out;
}

struct Loaded {
  model::TraceModel model;
  uint32_t pid = 0;
};

Loaded loadWithStates(const fs::path& trace, uint32_t pid) {
  Loaded l;
  l.pid = pid;
  auto loaded = model::TraceModel::load(trace);
  EXPECT_TRUE(loaded.ok()) << (loaded.ok() ? "" : loaded.error().toString());
  if (!loaded.ok()) return l;
  l.model = std::move(*loaded);
  EXPECT_TRUE(analysis::buildThreadStates(l.model).ok());
  return l;
}

// tid -> thread index of the lock threads of the target.
std::map<uint32_t, size_t> lockThreads(const Loaded& l) {
  std::map<uint32_t, size_t> out;
  for (size_t t = 0; t < l.model.threads.size(); ++t) {
    const auto& th = l.model.threads[t];
    if (th.pid == l.pid && th.name.rfind("tb-lock-", 0) == 0) out[th.tid] = t;
  }
  return out;
}

bool stackTouchesModule(const model::TraceModel& m, CallstackIdx leaf, const std::string& modulePath) {
  for (uint32_t node = leaf.get(); node != 0 && node < m.nodes.size(); node = m.nodes[node].parent.get()) {
    const model::Frame& f = m.frames[m.nodes[node].frame.get()];
    if (f.module.valid() && m.modules[f.module.get()].path == modulePath) return true;
  }
  return false;
}

class SchedCapture : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ifstream in("/proc/sys/kernel/perf_event_max_sample_rate");
    long long maxRate = 0;
    if (in >> maxRate && maxRate > 0 && maxRate < 4000)
      GTEST_SKIP() << "kernel.perf_event_max_sample_rate is " << maxRate
                   << ", below the 4000 Hz these tests use";
  }

  CaptureOptions options(const std::string& scenario, double seconds) {
    CaptureOptions o;
    o.mode = CaptureMode::Launch;
    o.exe = TACHYON_TESTBED_EXE;
    o.args = {"--scenario", scenario, "--duration", std::to_string(seconds)};
    o.outPath = m_dir.path() / (scenario + ".tachyon");
    o.samplingHz = 4000;
    return o;
  }

  static Result<CaptureResult> finish(LinuxPerfBackend& backend, double limitSeconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(limitSeconds);
    while (!backend.targetExited() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return backend.stop();
  }

  // Waits for the lock threads to exist and reads their counters; empty when they never showed up.
  static std::map<uint32_t, ThreadCounters> countersWhenRunning(uint32_t pid, double afterSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(afterSeconds));
    return readCounters(pid, "tb-lock-");
  }

  // Counts the off-CPU intervals of a thread by state.
  struct Intervals {
    size_t waiting = 0, ready = 0, running = 0, withWaker = 0, withBlockingStack = 0, withUnblockingStack = 0;
  };
  static Intervals count(const Loaded& l, size_t thread, uint64_t minNs = 0) {
    Intervals c;
    for (const auto& iv : l.model.statesByThread[thread]) {
      const uint64_t len = iv.end - iv.start;
      switch (iv.state) {
        case ThreadState::Waiting:
          if (len >= minNs) ++c.waiting;
          break;
        case ThreadState::Ready:
          if (len >= minNs) ++c.ready;
          break;
        case ThreadState::Running: ++c.running; break;
        default: break;
      }
      if (iv.state == ThreadState::Waiting || iv.state == ThreadState::Ready) {
        c.withWaker += iv.woke_by_tid != 0 ? 1 : 0;
        c.withBlockingStack += iv.blocking_stack.valid() && iv.blocking_stack.get() != 0 ? 1 : 0;
        c.withUnblockingStack += iv.unblocking_stack.valid() && iv.unblocking_stack.get() != 0 ? 1 : 0;
      }
    }
    return c;
  }

  TempDir m_dir;
};

// ---------------------------------------------------------------------------------------------------------
// tracepoints

class SchedTracepointCapture : public SchedCapture {
 protected:
  void SetUp() override {
    SchedCapture::SetUp();
    if (IsSkipped()) return;
    std::string reason;
    if (!locateTracefs(m_dir.path(), m_tracefs, reason)) GTEST_SKIP() << reason;
  }

  // Runs the lock scenario with the tracepoint path and returns the loaded trace; fails the test when the
  // backend did not use the tracepoints.
  Loaded runLock(double seconds, std::map<uint32_t, ThreadCounters>* counters = nullptr) {
    LinuxPerfBackendConfig cfg;
    cfg.tracefsRoot = m_tracefs;
    LinuxPerfBackend backend(cfg);
    CaptureOptions o = options("lock", seconds);
    auto started = backend.start(o);
    if (!started.ok() &&
        (started.error().code == ErrorCode::NoPrivilege || started.error().code == ErrorCode::Unsupported)) {
      ADD_FAILURE() << "perf_event_open refused: " << started.error().toString();
      return {};
    }
    EXPECT_TRUE(started.ok()) << (started.ok() ? "" : started.error().toString());
    if (!started.ok()) return {};
    const uint32_t pid = backend.rootPid();
    if (counters != nullptr) *counters = countersWhenRunning(pid, seconds * 0.6);
    auto result = finish(backend, 30.0);
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.error().toString());
    if (!result.ok()) return {};
    const LinuxPerfDiagnostics diag = backend.diagnostics();
    EXPECT_EQ(diag.schedPath, "tracepoint") << "the tracepoint events could not be opened; warnings: "
                                            << (result->warnings.empty() ? "none" : result->warnings[0]);
    EXPECT_EQ(result->eventsLost, 0u);
    return loadWithStates(o.outPath, pid);
  }

  fs::path m_tracefs;
};

TEST_F(SchedTracepointCapture, LockScenarioWaitersAreWokenByTheHolderSystemWide) {
  REQUIRE_SYSTEM_WIDE_PERF();
  std::map<uint32_t, ThreadCounters> counters;
  Loaded l = runLock(3.0, &counters);
  ASSERT_FALSE(l.model.threads.empty());
  const auto locks = lockThreads(l);
  ASSERT_EQ(locks.size(), 2u) << "the two tb-lock threads are in the trace";

  const std::string exe = TACHYON_TESTBED_EXE;
  uint64_t waits = 0, wokenByHolder = 0, wokenByOther = 0, withUnlockPathStack = 0, withWaitStack = 0,
           sleeping = 0;
  for (const auto& [tid, thread] : locks) {
    uint32_t holderTid = 0;
    for (const auto& [t2, idx2] : locks)
      if (t2 != tid) holderTid = t2;
    for (const auto& iv : l.model.statesByThread[thread]) {
      if (iv.state != ThreadState::Waiting || iv.end - iv.start < 20'000) continue;  // a real wait, >= 20 us
      ++waits;
      if (iv.wait_reason == 1) ++sleeping;  // S: interruptible sleep (a futex wait)
      if (iv.woke_by_tid == holderTid)
        ++wokenByHolder;
      else if (iv.woke_by_tid != 0)
        ++wokenByOther;
      if (iv.unblocking_stack.valid() && iv.woke_by_tid == holderTid &&
          stackTouchesModule(l.model, iv.unblocking_stack, exe))
        ++withUnlockPathStack;
      if (iv.blocking_stack.valid() && stackTouchesModule(l.model, iv.blocking_stack, exe)) ++withWaitStack;
    }
  }
  std::printf(
      "[sched] lock scenario (system-wide, tracepoints): %llu waits, %llu woken by the holder, %llu by "
      "others, "
      "%llu with an unlock-path stack in the testbed, %llu with a blocking stack in the testbed, %llu "
      "S-state\n",
      static_cast<unsigned long long>(waits), static_cast<unsigned long long>(wokenByHolder),
      static_cast<unsigned long long>(wokenByOther), static_cast<unsigned long long>(withUnlockPathStack),
      static_cast<unsigned long long>(withWaitStack), static_cast<unsigned long long>(sleeping));
  ASSERT_GT(waits, 20u) << "two threads alternating on a mutex for 3 s wait many times";
  EXPECT_GT(wokenByHolder * 2, waits) << "the majority of waits are ended by a wakeup from the lock holder";
  EXPECT_LE(wokenByOther * 4, waits) << "other wakers are rare";
  EXPECT_GT(withUnlockPathStack * 2, waits) << "unblocking stacks run through the testbed's unlock path";
  EXPECT_GT(withWaitStack * 2, waits) << "blocking stacks run through the testbed's lock path";
  EXPECT_GT(sleeping * 2, waits) << "futex waits are interruptible sleeps (wait reason S)";

  // No switches are lost: every switch-out the kernel counted up to the moment the counters were read is in
  // the trace.
  for (const auto& [tid, c] : counters) {
    auto it = locks.find(tid);
    ASSERT_NE(it, locks.end());
    const Intervals iv = count(l, it->second);
    EXPECT_GE(iv.waiting + iv.ready + 3, c.voluntary + c.nonvoluntary)
        << "tid " << tid << ": " << iv.waiting << " waits + " << iv.ready << " ready vs kernel "
        << c.voluntary << " voluntary + " << c.nonvoluntary << " involuntary switches";
  }
}

// ---------------------------------------------------------------------------------------------------------
// the PERF_RECORD_SWITCH fallback (runs without tracefs)

TEST_F(SchedCapture, FallbackRecordsSwitchesWithoutWaitReasonsOrWakers) {
  REQUIRE_SYSTEM_WIDE_PERF();
  LinuxPerfBackendConfig cfg;
  cfg.forceContextSwitchFallback = true;
  LinuxPerfBackend backend(cfg);
  CaptureOptions o = options("lock", 3.0);
  auto started = backend.start(o);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const uint32_t pid = backend.rootPid();
  const auto counters = countersWhenRunning(pid, 1.8);
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(backend.diagnostics().schedPath, "context_switch");
  EXPECT_EQ(result->eventsLost, 0u);
  bool explained = false;
  for (const std::string& w : result->warnings)
    explained |=
        w.find("PERF_RECORD_SWITCH") != std::string::npos && w.find("wait reasons") != std::string::npos;
  EXPECT_TRUE(explained) << "the capture explains that wait reasons and wakers are missing";

  Loaded l = loadWithStates(o.outPath, pid);
  const auto locks = lockThreads(l);
  ASSERT_EQ(locks.size(), 2u);
  uint64_t waiting = 0, ready = 0, waitReasons = 0, wakers = 0, stacks = 0;
  for (const auto& [tid, thread] : locks) {
    const Intervals iv = count(l, thread);
    waiting += iv.waiting;
    ready += iv.ready;
    stacks += iv.withBlockingStack + iv.withUnblockingStack;
    wakers += iv.withWaker;
    for (const auto& s : l.model.statesByThread[thread])
      if ((s.state == ThreadState::Waiting || s.state == ThreadState::Ready) && s.wait_reason != 0)
        ++waitReasons;
    // Every switch the kernel counted when the counters were read is in the trace.
    auto c = counters.find(tid);
    if (c != counters.end())
      EXPECT_GE(iv.waiting + iv.ready + 3, c->second.voluntary + c->second.nonvoluntary)
          << "tid " << tid << " lost switches: " << iv.waiting << " + " << iv.ready << " vs "
          << c->second.voluntary << " + " << c->second.nonvoluntary;
  }
  const LinuxPerfDiagnostics diag = backend.diagnostics();
  std::printf(
      "[sched] lock scenario (context_switch fallback): %llu waiting, %llu ready intervals; switch records "
      "%llu "
      "read / %llu written (%llu dropped not-target), drained %llu records, %llu throttle records, %llu "
      "malformed chunks\n",
      static_cast<unsigned long long>(waiting), static_cast<unsigned long long>(ready),
      static_cast<unsigned long long>(diag.convert.schedSwitchEvents),
      static_cast<unsigned long long>(diag.convert.switchesWritten),
      static_cast<unsigned long long>(diag.convert.schedDroppedNotTarget),
      static_cast<unsigned long long>(diag.drain.metaRecords + diag.drain.samples),
      static_cast<unsigned long long>(diag.drain.throttleRecords),
      static_cast<unsigned long long>(diag.drain.malformedChunks));
  EXPECT_GT(waiting, 20u) << "the lock threads block on the mutex over and over";
  EXPECT_EQ(waitReasons, 0u) << "the fallback has no wait reasons";
  EXPECT_EQ(wakers, 0u) << "the fallback has no wakeup events";
  EXPECT_EQ(stacks, 0u) << "the fallback has no stacks";
}

TEST_F(SchedCapture, PerThreadPlanRecordsTaskBoundSwitchesBothWays) {
  // The unprivileged plan: inherited per-task events on every cpu. A task-bound sched_switch tracepoint would
  // miss the switches into the task, so this plan uses PERF_RECORD_SWITCH out / in records.
  LinuxPerfBackendConfig cfg;
  cfg.forcePerThread = true;
  LinuxPerfBackend backend(cfg);
  CaptureOptions o = options("lock", 2.0);
  auto started = backend.start(o);
  if (!started.ok() &&
      (started.error().code == ErrorCode::NoPrivilege || started.error().code == ErrorCode::Unsupported))
    GTEST_SKIP() << "perf_event_open is not available here: " << started.error().toString();
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const uint32_t pid = backend.rootPid();
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(backend.diagnostics().plan, "per-thread");
  EXPECT_EQ(backend.diagnostics().schedPath, "context_switch");
  Loaded l = loadWithStates(o.outPath, pid);
  const auto locks = lockThreads(l);
  ASSERT_EQ(locks.size(), 2u);
  uint64_t waits = 0, unknownNs = 0, totalNs = 0;
  for (const auto& [tid, thread] : locks) {
    waits += count(l, thread, 20'000).waiting;
    for (const auto& iv : l.model.statesByThread[thread]) {
      totalNs += iv.end - iv.start;
      if (iv.state == ThreadState::Unknown) unknownNs += iv.end - iv.start;
    }
  }
  EXPECT_GT(waits, 10u) << "switch-outs and switch-ins pair up into waits";
  EXPECT_LT(unknownNs * 4, totalNs) << "no big gaps of unknown state: both directions were recorded";
}

TEST_F(SchedCapture, FallbackMarksPreemptionAsReadyNotWaiting) {
  REQUIRE_SYSTEM_WIDE_PERF();
  const int cpu = firstAllowedCpu();
  if (cpu < 0) GTEST_SKIP() << "cannot determine an allowed cpu";
  // Two busy threads pinned to one cpu compete with the pinned `spin` target: the target is preempted, so its
  // switch-outs carry the PREEMPT flag and must come back as Ready intervals.
  std::atomic<bool> stop{false};
  std::vector<std::thread> hogs;
  for (int i = 0; i < 2; ++i)
    hogs.emplace_back([&] {
      pinTask(0, cpu);
      volatile uint64_t x = 0;
      while (!stop.load(std::memory_order_relaxed)) x = x + 1;
    });

  LinuxPerfBackendConfig cfg;
  cfg.forceContextSwitchFallback = true;
  LinuxPerfBackend backend(cfg);
  CaptureOptions o = options("spin", 3.0);
  auto started = backend.start(o);
  if (!started.ok()) {
    stop = true;
    for (auto& h : hogs) h.join();
  }
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const uint32_t pid = backend.rootPid();
  for (int attempt = 0; attempt < 50; ++attempt) {  // pin the target's threads as soon as they exist
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc/" + std::to_string(pid) + "/task", ec))
      pinTask(static_cast<pid_t>(std::stoul(e.path().filename().string())), cpu);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  auto result = finish(backend, 30.0);
  stop = true;
  for (auto& h : hogs) h.join();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(backend.diagnostics().schedPath, "context_switch");

  Loaded l = loadWithStates(o.outPath, pid);
  uint64_t ready = 0, waiting = 0, readyNs = 0, withReason = 0;
  for (size_t t = 0; t < l.model.threads.size(); ++t) {
    if (l.model.threads[t].pid != pid) continue;
    for (const auto& iv : l.model.statesByThread[t]) {
      if (iv.state == ThreadState::Ready) {
        ++ready;
        readyNs += iv.end - iv.start;
        withReason += iv.wait_reason != 0 ? 1 : 0;
      }
      if (iv.state == ThreadState::Waiting) ++waiting;
    }
  }
  std::printf(
      "[sched] spin target against 2 pinned hogs on cpu %d: %llu ready intervals (%.1f ms), %llu waiting\n",
      cpu, static_cast<unsigned long long>(ready), static_cast<double>(readyNs) / 1e6,
      static_cast<unsigned long long>(waiting));
  EXPECT_GE(ready, 5u) << "the busy target is preempted again and again";
  EXPECT_GT(readyNs, 50'000'000u) << "and spends a substantial time runnable but not running";
  EXPECT_EQ(withReason, 0u);
}

}  // namespace
}  // namespace tachyon::capture
