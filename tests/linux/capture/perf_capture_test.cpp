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
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
  uint64_t firstTs = ~0ull;
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
      if (cols.ts[i] < firstTs) {
        firstTs = cols.ts[i];
        a.firstSampleHasMain = hasMain;
        a.firstSampleLeaf = leafName;
        a.firstSampleStack = stackText;
      }
    }
  }
  a.firstTargetTs = firstTs == ~0ull ? 0 : firstTs;
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

TEST_F(PerfCapture, SpinScenarioSystemWideLaunch) {
  REQUIRE_SYSTEM_WIDE_PERF();
  LinuxPerfBackend backend;
  CaptureOptions o = testbedOptions(3.0);
  auto started = backend.start(o);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const uint32_t pid = backend.rootPid();
  ASSERT_GT(pid, 0u);
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
  const LinuxPerfDiagnostics diag = backend.diagnostics();
  EXPECT_EQ(diag.plan, "system-wide");
  EXPECT_EQ(result->eventsLost, 0u) << "no records may be lost on a 3 s spin capture";
  EXPECT_GT(result->durationS, 2.5);

  Analysis a = analyse(o.outPath, pid);
  ASSERT_GT(a.targetSamples, 0u);
  // 8 kHz for ~3 s on the one busy thread: thousands of samples (CPU-clock sampling on a busy shared runner
  // can lose some; the bound is generous).
  EXPECT_GT(a.targetSamples, 3000u) << "samples of the target";
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
  LinuxPerfBackend backend(cfg);
  CaptureOptions o = testbedOptions(2.0);
  auto started = backend.start(o);
  SKIP_IF_PERF_REFUSED(started);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const uint32_t pid = backend.rootPid();
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(backend.diagnostics().plan, "per-thread");
  EXPECT_EQ(result->eventsLost, 0u);
  Analysis a = analyse(o.outPath, pid);
  EXPECT_GT(a.targetSamples, 1000u);
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

  LinuxPerfBackend backend;
  CaptureOptions o;
  o.mode = CaptureMode::Attach;
  o.pid = spawned->pid();
  o.outPath = m_dir.path() / "attach.tachyon";
  o.samplingHz = 8000;
  auto started = backend.start(o);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(result->eventsLost, 0u);
  Analysis a = analyse(o.outPath, spawned->pid());
  EXPECT_GT(a.targetSamples, 1000u);
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
  LinuxPerfBackend backend(cfg);
  CaptureOptions o;
  o.mode = CaptureMode::Attach;
  o.pid = spawned->pid();
  o.outPath = m_dir.path() / "jobs.tachyon";
  o.samplingHz = 4000;
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
  EXPECT_GT(total, 1000u);
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

  LinuxPerfBackend backend;
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
  LinuxPerfBackend backend;
  CaptureOptions o = testbedOptions(4.0);
  o.circularMb = 64;
  ASSERT_TRUE(backend.start(o).ok());
  const uint32_t pid = backend.rootPid();
  std::this_thread::sleep_for(std::chrono::milliseconds(1800));
  auto flushed = backend.flush("hitch-live", 1);
  ASSERT_TRUE(flushed.ok()) << flushed.error().toString();
  EXPECT_EQ(backend.state(), CaptureState::Capturing);
  Analysis a = analyse(flushed->tracePath, pid);
  EXPECT_GT(a.targetSamples, 2000u) << "about one second of 8 kHz samples";
  EXPECT_LT(a.targetSamples, 12000u) << "no more than ~1.3 s worth";
  expectSpinOrdering(a, 100);
  auto result = finish(backend, 30.0);
  ASSERT_TRUE(result.ok()) << result.error().toString();
}

}  // namespace
}  // namespace tachyon::capture
