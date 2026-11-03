// WP-70: real perf_event_open captures. Label linux_privileged: every test skips itself, with the reason,
// when the kernel refuses (unprivileged runner, container without CAP_PERFMON, kernel.perf_event_paranoid,
// seccomp).
//
// The workload is the testbed's `spin` scenario (hot_a 40%, hot_b 20%, hot_c 10%, spin_rest 30%). The capture
// is converted to a .tachyon, loaded by the model loader and symbolised with the DWARF provider.
#include "capture/linux_perf/launch.h"
#include "capture/linux_perf/linux_perf_backend.h"
#include "model/model.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;
using perf::LaunchedProcess;
using perf::LaunchSpec;

class TempDir {
 public:
  TempDir() {
    m_path = fs::temp_directory_path() / ("tachyon-perf-live-" + std::to_string(::getpid()));
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

struct Analysis {
  model::TraceModel model;
  std::map<std::string, uint64_t> leafSamples;       // function name -> samples whose leaf is in it
  std::map<std::string, uint64_t> inclusiveSamples;  // function name -> samples with it anywhere in the stack
  uint64_t targetSamples = 0;
  uint64_t firstTargetTs = 0;
  uint64_t lastTargetTs = 0;
  uint64_t firstMainTs = 0;         // earliest sample with `main` in its stack
  bool firstSampleHasMain = false;  // does the earliest target sample's stack contain main?
  std::string firstSampleLeaf;
  std::string firstSampleStack;    // leaf-first names of the earliest target sample (diagnostics)
  uint64_t samplesBeforeMain = 0;  // target samples older than the first sample inside main
  uint64_t startTs = 0;            // trace header start
};

// Loads the trace and symbolises every frame with the DWARF provider (the modules are the files on disk).
Analysis analyse(const fs::path& trace, uint32_t pid) {
  Analysis a;
  auto loaded = model::TraceModel::load(trace);
  EXPECT_TRUE(loaded.ok()) << loaded.error().toString();
  if (!loaded.ok()) return a;
  a.model = std::move(*loaded);
  symbols::SymbolService svc({}, {});
  svc.setLocate([](const model::Module& m) -> Result<fs::path> {
    if (m.path.empty() || !fs::exists(m.path)) return makeError(ErrorCode::NotFound, "no file " + m.path);
    return fs::path(m.path);
  });
  symbols::registerDwarfProvider(svc);
  svc.resolveAllAsync(a.model);
  svc.waitAll(a.model);

  // Which nodes are leaves of a stack: a node is a leaf position if it is the sample's callstack value; the
  // caller rule applies to every other node (return addresses).
  const auto name = [&](StringIdx s) { return std::string(svc.symbolStrings().get(s)); };
  uint64_t firstTs = ~0ull, lastTs = 0;
  for (size_t t = 0; t < a.model.threads.size(); ++t) {
    if (a.model.threads[t].pid != pid) continue;
    const auto& cols = a.model.samplesByThread[t];
    for (size_t i = 0; i < cols.ts.size(); ++i) {
      ++a.targetSamples;
      std::set<std::string> seen;
      bool leaf = true, hasMain = false;
      std::string leafName, stackText;
      for (uint32_t node = cols.callstack[i].get(); node != 0 && node < a.model.nodes.size();
           node = a.model.nodes[node].parent.get()) {
        const FrameIdx fr = a.model.nodes[node].frame;
        const symbols::ResolvedAddress& ra = leaf ? svc.lookup(fr) : svc.lookupCaller(fr);
        // Exclusive time goes to the physical function (the testbed inlines its burn<> kernel into hot_a
        // ...); every name of the inline chain counts for inclusive time.
        std::string fn = ra.resolved ? name(ra.function) : std::string();
        std::vector<std::string> names;
        for (const auto& in : ra.inlines) names.push_back(name(in.function));
        if (!fn.empty()) names.push_back(fn);
        if (leaf) leafName = fn;
        stackText += (names.empty() ? std::string("?") : names.front()) + " < ";
        for (const std::string& n : names) {
          if (!n.empty() && seen.insert(n).second) ++a.inclusiveSamples[n];
          // The forked child is a copy of this test until its exec (visible under a slow fork handler such as
          // ASAN's), so only the testbed executable's main() counts.
          const model::Frame& frame = a.model.frames[fr.get()];
          hasMain |= n == "main" && frame.module.valid() &&
                     a.model.modules[frame.module.get()].path == TACHYON_TESTBED_EXE;
        }
        leaf = false;
      }
      ++a.leafSamples[leafName];
      if (hasMain) a.firstMainTs = a.firstMainTs == 0 ? cols.ts[i] : std::min(a.firstMainTs, cols.ts[i]);
      lastTs = std::max<uint64_t>(lastTs, cols.ts[i]);
      if (cols.ts[i] < firstTs) {
        firstTs = cols.ts[i];
        a.firstSampleHasMain = hasMain;
        a.firstSampleLeaf = leafName;
        a.firstSampleStack = stackText;
      }
    }
  }
  a.firstTargetTs = firstTs == ~0ull ? 0 : firstTs;
  a.lastTargetTs = lastTs;
  a.startTs = a.model.header.start_ts;
  for (size_t t = 0; t < a.model.threads.size(); ++t) {
    if (a.model.threads[t].pid != pid) continue;
    for (uint64_t ts : a.model.samplesByThread[t].ts) a.samplesBeforeMain += ts < a.firstMainTs ? 1 : 0;
  }
  return a;
}

// Skips the test, with the kernel's reason, unless system-wide sampling is allowed (CAP_PERFMON, root or
// kernel.perf_event_paranoid <= 0).
#define REQUIRE_SYSTEM_WIDE_PERF()                                                                      \
  do {                                                                                                  \
    LinuxPerfBackend probe;                                                                             \
    auto priv = probe.checkPrivileges();                                                                \
    if (!priv.ok())                                                                                     \
      GTEST_SKIP() << "system-wide perf_event_open is not available here: " << priv.error().toString(); \
  } while (0)

// Skips the test when starting a capture fails because the kernel refuses perf_event_open at all.
#define SKIP_IF_PERF_REFUSED(started)                                                            \
  do {                                                                                           \
    if (!(started).ok() && ((started).error().code == ErrorCode::NoPrivilege ||                  \
                            (started).error().code == ErrorCode::Unsupported))                   \
      GTEST_SKIP() << "perf_event_open is not available here: " << (started).error().toString(); \
  } while (0)

// Cpu seconds (user + system, all threads) the process `pid` has consumed so far; negative when unreadable.
double processCpuSeconds(uint32_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(in, line)) return -1;
  const size_t close = line.rfind(')');  // the command name may contain spaces and parentheses
  if (close == std::string::npos) return -1;
  std::istringstream rest(line.substr(close + 2));  // starts at field 3 (state)
  std::string field;
  unsigned long long utime = 0, stime = 0;
  for (int f = 3; f <= 15 && (rest >> field); ++f) {
    if (f == 14) utime = std::strtoull(field.c_str(), nullptr, 10);
    if (f == 15) stime = std::strtoull(field.c_str(), nullptr, 10);
  }
  return static_cast<double>(utime + stime) / static_cast<double>(sysconf(_SC_CLK_TCK));
}

// Polls the cpu consumption of a process in the background, so that the total is known after the process has
// exited (/proc/<pid> is gone by then).
class CpuMeter {
 public:
  // `fromZero`: count the whole life of the process (a launched target), otherwise only what it consumes from
  // now on (an attached one).
  CpuMeter(uint32_t pid, bool fromZero) : m_pid(pid) {
    const double now = processCpuSeconds(pid);
    m_base = fromZero || now < 0 ? 0 : now;
    m_last = now < 0 ? 0 : now;
    m_thread = std::thread([this] {
      while (!m_stop.load()) {
        const double c = processCpuSeconds(m_pid);
        if (c > m_last.load()) m_last = c;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }
  ~CpuMeter() { used(); }
  CpuMeter(const CpuMeter&) = delete;
  CpuMeter& operator=(const CpuMeter&) = delete;

  // Cpu seconds consumed since the meter started (since the process started when `fromZero`). Stops the
  // meter.
  double used() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    return m_last.load() - m_base;
  }

 private:
  uint32_t m_pid;
  double m_base = 0;
  std::atomic<double> m_last{0};
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
};

// Data pages per perf ring in these tests (8 MiB with 4 KiB pages; the default is 1 MiB). The backend's drain
// thread empties the rings every few milliseconds, but when the machine is busy (ctest -j, other builds) it
// can go without the cpu for much longer than that, and a ring that fills up meanwhile loses records
// (PERF_RECORD_LOST): the samples of the trace are then fewer than the target's cpu time says, and
// `eventsLost` is not 0. Rings that hold seconds of samples make these checks independent of the scheduler.
constexpr uint32_t kTestRingPages = 2048;

LinuxPerfBackendConfig testConfig() {
  LinuxPerfBackendConfig config;
  config.ringPages = kTestRingPages;
  return config;
}

class PerfCapture : public ::testing::Test {
 protected:
  void SetUp() override {
    // The tests ask for 8 kHz; a kernel that lowered kernel.perf_event_max_sample_rate (it does so by itself
    // when sampling costs more than perf_cpu_time_max_percent of the cpu) would refuse the events.
    std::ifstream in("/proc/sys/kernel/perf_event_max_sample_rate");
    long long maxRate = 0;
    if (in >> maxRate && maxRate > 0 && maxRate < 8000)
      GTEST_SKIP() << "kernel.perf_event_max_sample_rate is " << maxRate
                   << ", below the 8000 Hz these tests sample at";
  }
  CaptureOptions testbedOptions(double seconds) {
    CaptureOptions o;
    o.mode = CaptureMode::Launch;
    o.exe = TACHYON_TESTBED_EXE;
    o.args = {"--scenario", "spin", "--duration", std::to_string(seconds)};
    o.outPath = m_dir.path() / "spin.tachyon";
    o.samplingHz = 8000;
    return o;
  }

  // Waits for the launched target to exit (or the time limit), then stops the capture.
  static Result<CaptureResult> finish(LinuxPerfBackend& backend, double limitSeconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(limitSeconds);
    while (!backend.targetExited() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return backend.stop();
  }

  // "name=count" of the busiest leaf functions, for failure messages.
  static std::string topLeaves(const Analysis& a) {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (const auto& [n, c] : a.leafSamples) v.emplace_back(c, n.empty() ? "<unresolved>" : n);
    std::sort(v.rbegin(), v.rend());
    std::string out;
    for (size_t i = 0; i < v.size() && i < 8; ++i)
      out += v[i].second + "=" + std::to_string(v[i].first) + " ";
    return out;
  }

  // The sampler fires `hz` times per second of cpu time the target consumes, not per second of wall time, so
  // on a machine shared with other work (ctest -j, other builds) a target that got a fraction of a cpu yields
  // a proportionally smaller trace. The sample count is therefore judged against the cpu time the target
  // really got (a fixed count would only hold on an idle machine); `minRatio` allows for samples a busy
  // runner loses.
  static void expectSamplesForCpu(uint64_t samples, double hz, double cpuSeconds, double minRatio) {
    ASSERT_GT(cpuSeconds, 0.05) << "the target got no cpu at all";
    EXPECT_GT(static_cast<double>(samples), minRatio * hz * cpuSeconds)
        << samples << " samples for " << cpuSeconds << " cpu seconds at " << hz << " Hz";
  }

  void expectSpinOrdering(const Analysis& a, uint64_t minHot) {
    const uint64_t hotA = a.leafSamples.count("hot_a") ? a.leafSamples.at("hot_a") : 0;
    const uint64_t hotB = a.leafSamples.count("hot_b") ? a.leafSamples.at("hot_b") : 0;
    const uint64_t hotC = a.leafSamples.count("hot_c") ? a.leafSamples.at("hot_c") : 0;
    EXPECT_GT(hotA, minHot) << "hot_a=" << hotA << " hot_b=" << hotB << " hot_c=" << hotC
                            << "; top leaves: " << topLeaves(a);
    EXPECT_GT(hotA, hotB) << "hot_a (40%) must outweigh hot_b (20%)";
    EXPECT_GT(hotB, hotC) << "hot_b (20%) must outweigh hot_c (10%)";
    EXPECT_GT(hotC, 0u);
  }

  TempDir m_dir;
};

// Records the target's cpu consumption against the wall clock, so that the cpu time of any window can be read
// back afterwards.
class CpuTimeline {
 public:
  void sample(uint32_t pid) {
    m_points.emplace_back(std::chrono::steady_clock::now(), processCpuSeconds(pid));
  }
  // Cpu seconds consumed in the `seconds` before `end` (linear interpolation between the samples).
  double cpuInWindow(std::chrono::steady_clock::time_point end, double seconds) const {
    return at(end) - at(end - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(seconds)));
  }

 private:
  double at(std::chrono::steady_clock::time_point t) const {
    if (m_points.empty()) return 0;
    if (t <= m_points.front().first) return m_points.front().second;
    for (size_t i = 1; i < m_points.size(); ++i) {
      if (t > m_points[i].first) continue;
      const double span = std::chrono::duration<double>(m_points[i].first - m_points[i - 1].first).count();
      const double part = std::chrono::duration<double>(t - m_points[i - 1].first).count();
      return m_points[i - 1].second +
             (span > 0 ? (m_points[i].second - m_points[i - 1].second) * (part / span) : 0);
    }
    return m_points.back().second;
  }
  std::vector<std::pair<std::chrono::steady_clock::time_point, double>> m_points;
};

TEST_F(PerfCapture, SpinScenarioSystemWideLaunch) {
  REQUIRE_SYSTEM_WIDE_PERF();
  LinuxPerfBackend backend(testConfig());
  CaptureOptions o = testbedOptions(3.0);
  auto started = backend.start(o);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const uint32_t pid = backend.rootPid();
  ASSERT_GT(pid, 0u);
  CpuMeter cpu(pid, /*fromZero=*/true);
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
  const LinuxPerfDiagnostics diag = backend.diagnostics();
  EXPECT_EQ(diag.plan, "system-wide");
  EXPECT_EQ(result->eventsLost, 0u) << "no records may be lost on a 3 s spin capture";
  EXPECT_GT(result->durationS, 2.5);

  Analysis a = analyse(o.outPath, pid);
  ASSERT_GT(a.targetSamples, 0u);
  // 8 kHz on the one busy thread: thousands of samples (CPU-clock sampling on a busy shared runner can lose
  // some; the bound is generous).
  expectSamplesForCpu(a.targetSamples, 8000, cpu.used(), 0.125);
  expectSpinOrdering(a, 500);
  // The three hot functions and their callers appear in the stacks.
  EXPECT_GT(a.inclusiveSamples["hot_a"], 0u);
  EXPECT_GT(a.inclusiveSamples["main"], 0u) << "main is in the callchains (frame pointers)";

  std::printf(
      "[perf] system-wide launch: %llu target samples in %.2f s (%.0f/s), first sample %.3f ms after start, "
      "%llu samples before main (first leaf '%s'), %llu bytes spooled\n",
      static_cast<unsigned long long>(a.targetSamples), result->durationS,
      static_cast<double>(a.targetSamples) / result->durationS,
      static_cast<double>(a.firstTargetTs - a.startTs) / 1e6,
      static_cast<unsigned long long>(a.samplesBeforeMain), a.firstSampleLeaf.c_str(),
      static_cast<unsigned long long>(diag.drain.bytesDrained));

  // "First sample before main": the capture began before the target's first instruction, so the earliest
  // sample is from the exec / dynamic loader and has no `main` in its stack.
  ASSERT_GT(a.firstMainTs, 0u);
  EXPECT_LT(a.firstTargetTs, a.firstMainTs) << "samples exist from before main()";
  EXPECT_FALSE(a.firstSampleHasMain) << "the earliest sample (stack " << a.firstSampleStack << ") (leaf '"
                                     << a.firstSampleLeaf << "') is not inside main";

  // The target's own modules were identified by GNU build-id.
  bool exeModule = false;
  for (const auto& m : a.model.modules)
    if (m.pid == pid && m.path == TACHYON_TESTBED_EXE) {
      exeModule = true;
      EXPECT_EQ(m.id.kind, trace::kModuleIdGnuBuildId);
    }
  EXPECT_TRUE(exeModule);
}

TEST_F(PerfCapture, SpinScenarioPerThreadLaunch) {
  LinuxPerfBackendConfig cfg;
  cfg.forcePerThread = true;
  cfg.ringPages = kTestRingPages;
  LinuxPerfBackend backend(cfg);
  CaptureOptions o = testbedOptions(2.0);
  auto started = backend.start(o);
  SKIP_IF_PERF_REFUSED(started);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const uint32_t pid = backend.rootPid();
  CpuMeter cpu(pid, /*fromZero=*/true);
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(backend.diagnostics().plan, "per-thread");
  EXPECT_EQ(result->eventsLost, 0u);
  Analysis a = analyse(o.outPath, pid);
  expectSamplesForCpu(a.targetSamples, 8000, cpu.used(), 0.0625);
  expectSpinOrdering(a, 200);
  ASSERT_GT(a.firstMainTs, 0u);
  EXPECT_LT(a.firstTargetTs, a.firstMainTs) << "enable_on_exec starts the event at the exec, before main()";
}

TEST_F(PerfCapture, AttachToARunningProcess) {
  REQUIRE_SYSTEM_WIDE_PERF();
  LaunchSpec spec;
  spec.exe = TACHYON_TESTBED_EXE;
  spec.args = {"--scenario", "spin", "--duration", "4"};
  auto spawned = LaunchedProcess::spawnParked(spec);
  ASSERT_TRUE(spawned.ok());
  ASSERT_TRUE(spawned->release().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(700));  // let it get into the spin loop
  ASSERT_FALSE(spawned->exited());

  LinuxPerfBackend backend(testConfig());
  CaptureOptions o;
  o.mode = CaptureMode::Attach;
  o.pid = spawned->pid();
  o.outPath = m_dir.path() / "attach.tachyon";
  o.samplingHz = 8000;
  CpuMeter cpu(spawned->pid(), /*fromZero=*/false);
  auto started = backend.start(o);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(result->eventsLost, 0u);
  Analysis a = analyse(o.outPath, spawned->pid());
  expectSamplesForCpu(a.targetSamples, 8000, cpu.used(), 0.0833);
  expectSpinOrdering(a, 100);
  spawned->terminate();
}

TEST_F(PerfCapture, PerThreadAttachCoversEveryThreadOfAMultiThreadedTarget) {
  // The `jobs` scenario: 8 workers and a producer. Without privileges (the per-thread plan) the backend opens
  // every thread on every cpu and joins the events of a cpu into that cpu's ring.
  LaunchSpec spec;
  spec.exe = TACHYON_TESTBED_EXE;
  spec.args = {"--scenario", "jobs", "--duration", "5"};
  auto spawned = LaunchedProcess::spawnParked(spec);
  ASSERT_TRUE(spawned.ok());
  ASSERT_TRUE(spawned->release().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  ASSERT_FALSE(spawned->exited());

  LinuxPerfBackendConfig cfg;
  cfg.forcePerThread = true;
  cfg.ringPages = kTestRingPages;
  LinuxPerfBackend backend(cfg);
  CaptureOptions o;
  o.mode = CaptureMode::Attach;
  o.pid = spawned->pid();
  o.outPath = m_dir.path() / "jobs.tachyon";
  o.samplingHz = 4000;
  CpuMeter cpu(spawned->pid(), /*fromZero=*/false);
  auto started = backend.start(o);
  SKIP_IF_PERF_REFUSED(started);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(backend.diagnostics().plan, "per-thread");
  EXPECT_EQ(result->eventsLost, 0u);
  auto loaded = model::TraceModel::load(o.outPath);
  ASSERT_TRUE(loaded.ok());
  size_t threadsWithSamples = 0;
  uint64_t total = 0;
  for (size_t t = 0; t < loaded->threads.size(); ++t) {
    if (loaded->threads[t].pid != spawned->pid()) continue;
    const size_t n = loaded->samplesByThread[t].ts.size();
    threadsWithSamples += n > 50 ? 1 : 0;
    total += n;
  }
  EXPECT_GE(threadsWithSamples, 4u) << "samples from several worker threads, not only the main thread";
  expectSamplesForCpu(total, 4000, cpu.used(), 0.125);
  spawned->terminate();
}

TEST_F(PerfCapture, SystemModeSeesTheWholeMachine) {
  REQUIRE_SYSTEM_WIDE_PERF();
  LaunchSpec spec;
  spec.exe = TACHYON_TESTBED_EXE;
  spec.args = {"--scenario", "spin", "--duration", "3"};
  auto spawned = LaunchedProcess::spawnParked(spec);
  ASSERT_TRUE(spawned.ok());
  ASSERT_TRUE(spawned->release().ok());

  LinuxPerfBackend backend(testConfig());
  CaptureOptions o;
  o.mode = CaptureMode::System;
  o.outPath = m_dir.path() / "system.tachyon";
  o.samplingHz = 4000;
  ASSERT_TRUE(backend.start(o).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  auto loaded = model::TraceModel::load(o.outPath);
  ASSERT_TRUE(loaded.ok());
  EXPECT_GE(loaded->processes.size(), 2u) << "more than the one busy process";
  bool sawTestbed = false;
  for (const auto& t : loaded->threads) sawTestbed |= t.pid == spawned->pid();
  EXPECT_TRUE(sawTestbed);
  for (const auto& t : loaded->threads) EXPECT_FALSE(t.is_target) << "system mode has no target";
  spawned->terminate();
}

TEST_F(PerfCapture, CircularCaptureFlushesTheLastSeconds) {
  REQUIRE_SYSTEM_WIDE_PERF();
  LinuxPerfBackend backend(testConfig());
  CaptureOptions o = testbedOptions(4.0);
  o.circularMb = 64;
  ASSERT_TRUE(backend.start(o).ok());
  const uint32_t pid = backend.rootPid();
  // The target is a spinning thread, and the sampler fires 8000 times per second of cpu time it consumes, not
  // per second of wall time: on a machine shared with other work (ctest -j, other builds) the target gets a
  // fraction of a cpu and the last wall-clock second holds proportionally fewer samples. So the cpu time the
  // target really got is tracked while waiting, and the sample count is judged against that.
  CpuTimeline cpu;
  const auto waitUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
  while (std::chrono::steady_clock::now() < waitUntil) {
    cpu.sample(pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  cpu.sample(pid);
  const auto flushCalled = std::chrono::steady_clock::now();
  auto flushed = backend.flush("hitch-live", 1);
  ASSERT_TRUE(flushed.ok()) << flushed.error().toString();
  EXPECT_EQ(backend.state(), CaptureState::Capturing);
  Analysis a = analyse(flushed->tracePath, pid);
  const double cpuSeconds = cpu.cpuInWindow(flushCalled, 1.0);
  const double expected = 8000.0 * cpuSeconds;
  std::printf(
      "[perf] circular flush: %llu target samples, target cpu in the window %.3f s (expect ~%.0f), span %.3f "
      "s\n",
      static_cast<unsigned long long>(a.targetSamples), cpuSeconds, expected,
      static_cast<double>(a.lastTargetTs - a.firstTargetTs) / 1e9);
  ASSERT_GT(cpuSeconds, 0.05) << "the target got no cpu at all in the last second";
  EXPECT_GT(static_cast<double>(a.targetSamples), 0.5 * expected)
      << "about one second of 8 kHz samples per cpu second the target consumed";
  EXPECT_LT(a.targetSamples, 12000u) << "no more than ~1.3 s worth";
  // The flush keeps the last second of the capture and nothing older: the samples span at most that window
  // (plus the flush's own latency), and the capture is 1.8 s old, so an unwindowed dump would span more.
  EXPECT_LT(static_cast<double>(a.lastTargetTs - a.firstTargetTs) / 1e9, 1.3);
  EXPECT_GT(static_cast<double>(a.lastTargetTs - a.firstTargetTs) / 1e9, 0.5);
  expectSpinOrdering(a, std::min<uint64_t>(100, static_cast<uint64_t>(0.05 * expected)));
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
}

}  // namespace
}  // namespace tachyon::capture
