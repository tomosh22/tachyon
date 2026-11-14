// WP-65b: the viewer's Live mode against the real thing on Linux (docs/specs/live.md section 5): the real
// `tachyon-agent --backend perf`, the testbed's `spin` scenario launched through the Live dialog, a headless
// App (Null renderer, real ImGui frames). Label linux_privileged: skips itself, with the reason, when the
// kernel refuses system-wide perf_event_open (the CI privileged step fails when anything skips).
//
// A live run of $TACHYON_LIVE_VIEWER_SECONDS (default 60) seconds at 8 kHz is checked for:
//   * the Live document shows samples within 1 s of their timestamp (p95 of the age of the newest sample on
//     screen, sampled every ~50 ms, asserted at 2 s; the local clock is the trace clock);
//   * the window of the shown document never exceeds the viewer's window (the default 30 s; 5 s in a run
//     shortened below 50 s, so that it still fills);
//   * the viewer's memory is flat once the window is full (RSS growth < 10% between the second after the
//     window filled and the end);
//   * every module is located and its symbol file loaded at most once over the whole run, although the
//     tables grew (new symbol services adopt the old ones' finished modules);
//   * the functions of the spin scenario are named in the live document, and their shares in the last live
//     snapshot match the final trace's within 5 percentage points;
//   * Stop's counterpart: the capture ends by itself, live_end turns the document static and the final
//     trace opens through the normal capture_done path.
// The numbers are printed for the report.
#include "../../unit/agent/process_util.h"
#include "../../unit/viewer/status_test_util.h"
#include "capture/linux_perf/linux_perf_backend.h"
#include "control/client.h"
#include "control/test_util.h"
#include "viewer/app.h"
#include "viewer/capture/capture_controller.h"
#include "viewer/paths.h"
#include "viewer/timeline/timeline_data.h"

#include <gtest/gtest.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using agent::testing_util::Process;
using agent::testing_util::readTextFile;
using status_test::Harness;
using status_test::TempDir;
namespace fs = std::filesystem;

namespace {

double heapMiB() {
  const struct mallinfo2 mi = mallinfo2();
  return static_cast<double>(mi.uordblks + mi.hblkhd) / (1024.0 * 1024.0);
}

int threadCount() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line))
    if (line.rfind("Threads:", 0) == 0) return std::atoi(line.c_str() + 8);
  return -1;
}

double rssMiB() {
  std::ifstream in("/proc/self/statm");
  long pages = 0, resident = 0;
  in >> pages >> resident;
  return static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

// Leaf function shares (percent of the target's samples) of a document, named through its symbol service.
std::map<std::string, double> leafShares(const Document& doc) {
  const model::TraceModel& m = doc.model();
  const symbols::SymbolService* svc = doc.symbols().get();
  std::map<std::string, uint64_t> counts;
  uint64_t total = 0;
  for (size_t t = 0; t < m.threads.size(); ++t) {
    if (!m.threads[t].is_target) continue;
    const auto& cols = m.samplesByThread[t];
    for (size_t i = 0; i < cols.ts.size(); ++i) {
      ++total;
      std::string name;
      const uint32_t node = cols.callstack[i].get();
      if (svc != nullptr && node != 0 && node < m.nodes.size()) {
        const symbols::ResolvedAddress& ra = svc->lookup(m.nodes[node].frame);
        if (ra.resolved) name = std::string(svc->symbolStrings().get(ra.function));
      }
      ++counts[name];
    }
  }
  std::map<std::string, double> shares;
  for (const auto& [name, n] : counts)
    shares[name] = total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0;
  return shares;
}

}  // namespace

TEST(LiveViewerLinux, SpinLiveDocumentLatencyWindowMemorySymbolsAndShares) {
  capture::LinuxPerfBackend probe;
  auto priv = probe.checkPrivileges();
  if (!priv.ok())
    GTEST_SKIP() << "system-wide perf_event_open is not available here: " << priv.error().toString();
  {
    std::ifstream in("/proc/sys/kernel/perf_event_max_sample_rate");
    long long maxRate = 0;
    if (in >> maxRate && maxRate > 0 && maxRate < 8000)
      GTEST_SKIP() << "kernel.perf_event_max_sample_rate is " << maxRate << ", below 8000 Hz";
  }
  int seconds = 60;
  if (const char* env = std::getenv("TACHYON_LIVE_VIEWER_SECONDS")) seconds = std::max(14, std::atoi(env));
  // The default run exercises the default 30 s window; a run shortened below 50 s uses a 5 s one so it fills.
  const uint32_t windowSeconds = seconds >= 50 ? 30 : 5;

  TempDir dir("live_viewer_linux");
  const std::string address = control::testing_util::testAddress("liveviewer");  // private, short
  Process agentProc = Process::spawn(TACHYON_AGENT_EXE,
                                     {"--backend", "perf", "--socket", address, "--perf-ring-pages", "2048"},
                                     dir.path() / "agent.log");
  ASSERT_TRUE(agentProc.valid());
  struct Reaper {
    Process& p;
    ~Reaper() {
      if (!p.valid()) return;
      p.requestTerminate();
      int code = -1;
      if (!p.waitFor(20000, &code)) p.kill();
    }
  } reaper{agentProc};

  // Noise: a busy machine creates and ends threads all the time (compiler jobs, other programs). Here ~150
  // short-lived threads a second of this very process; they are scheduled, so a system-wide capture names
  // them all, but they have no samples and are not the target: the live view must not carry them.
  std::atomic<bool> noiseStop{false};
  std::thread noise([&] {
    while (!noiseStop.load()) {
      std::thread t([] {
        volatile uint64_t x = 0;
        for (int i = 0; i < 20000; ++i) x = x + static_cast<uint64_t>(i);
      });
      t.join();
      std::this_thread::sleep_for(std::chrono::milliseconds(6));
    }
  });
  struct NoiseJoin {
    std::atomic<bool>& stop;
    std::thread& t;
    ~NoiseJoin() {
      stop.store(true);
      if (t.joinable()) t.join();
    }
  } noiseJoin{noiseStop, noise};

  AppOptions options = status_test::headlessOptions("live_linux");
  options.autoConnectAgent = false;
  options.liveWindowSeconds = windowSeconds;
  Harness h(std::move(options));
  App& app = h.app;
  app.settings().capture.agentAddress = address;
  app.settings().capture.capturesDir = pathToUtf8(dir.path() / "captures");
  app.settings().capture.autoConnect = false;
  app.settings().capture.autoOpen = true;
  CaptureController& cap = app.capture();
  {
    // Ready when it accepts a connection.
    auto probeClient = control::Client::connectWithRetry(address, 15000);
    ASSERT_TRUE(probeClient.ok()) << readTextFile(dir.path() / "agent.log");
  }
  cap.reconnect();
  ASSERT_TRUE(h.until([&] {
    return cap.session().snapshot().connection == ConnectionState::Connected &&
           cap.session().snapshot().phase == AgentPhase::Idle;
  })) << readTextFile(dir.path() / "agent.log");

  // The Live dialog, Run mode: the testbed's spin scenario.
  ASSERT_TRUE(app.commands().run("capture.live"));
  h.frames(2);
  ASSERT_TRUE(cap.form().live);
  cap.form().mode = capture::CaptureMode::Launch;
  cap.form().exe = TACHYON_TESTBED_EXE;
  cap.form().args = "--scenario spin --duration " + std::to_string(seconds);
  cap.form().samplingHz = 8000;
  ASSERT_TRUE(cap.submitDialog()) << (cap.formErrors().empty() ? "" : cap.formErrors().front().message);
  const std::string finalPath = cap.lastRequest().out_path;

  using Clock = std::chrono::steady_clock;
  const auto began = Clock::now();
  std::vector<double> latencies, rssAtFill, rssEnd, heapEnd;
  double heapFillRef = 0;
  double maxWindow = 0, firstDocSeconds = -1;
  uint64_t maxSamplesShown = 0;
  uint32_t maxGaps = 0, maxEpochsAlive = 0, maxModelThreads = 0;
  const bool trace = std::getenv("TACHYON_LIVE_VIEWER_TRACE") != nullptr;
  double nextTrace = 0;
  auto nextSample = began;
  bool ended = false;
  std::shared_ptr<Document> lastLive;
  double rssFillRef = 0;
  while (true) {
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    if (elapsed > seconds + 120)
      FAIL() << "the capture did not end\n" << readTextFile(dir.path() / "agent.log");
    const std::shared_ptr<Document>& doc = app.document();
    if (doc && doc->isLive()) {
      if (firstDocSeconds < 0) firstDocSeconds = elapsed;
      lastLive = doc;
      if (Clock::now() >= nextSample) {
        nextSample += std::chrono::milliseconds(50);
        const LiveUiState ui = app.liveUiState();
        if (ui.latencyS && !doc->live().ended) latencies.push_back(*ui.latencyS);
        maxWindow = std::max(maxWindow, ui.windowSeconds);
        maxSamplesShown = std::max(maxSamplesShown, ui.samples);
        maxGaps = std::max(maxGaps, ui.gaps);
        maxModelThreads = std::max(maxModelThreads, ui.threads);
        maxEpochsAlive = std::max(maxEpochsAlive, app.live().stats().symbols.epochsAlive);
        if (trace && elapsed >= nextTrace) {
          nextTrace = elapsed + 2.0;
          const model::TraceModel& tm = doc->model();
          std::printf(
              "LIVE-VIEWER trace t=%.1f heap=%.1fMiB rss=%.1fMiB frames=%zu nodes=%zu strings=%zu "
              "modules=%zu samples=%llu epochs_alive=%u threads=%d documents_alive=%d lods_alive=%d "
              "mthreads=%zu lod_mib=%.1f model_resident=%.1fMiB live_model_resident=%.1fMiB\n",
              elapsed, heapMiB(), rssMiB(), tm.frames.size(), tm.nodes.size(), tm.strings.size(),
              tm.modules.size(), static_cast<unsigned long long>(ui.samples),
              app.live().stats().symbols.epochsAlive, threadCount(), Document::aliveCount(),
              timeline::LodShared::alive().load(), tm.threads.size(),
              doc->prebuiltLod() && doc->prebuiltLod()->lodOwner
                  ? static_cast<double>(doc->prebuiltLod()->lodOwner->memoryBytes()) / 1048576.0
                  : 0.0,
              static_cast<double>(tm.residentBytes()) / (1024.0 * 1024.0),
              app.live().model()
                  ? static_cast<double>(app.live().model()->stats().residentBytes) / (1024.0 * 1024.0)
                  : 0.0);
        }
        if (elapsed >= windowSeconds + 5) {
          rssEnd.push_back(rssMiB());
          heapEnd.push_back(heapMiB());
        }
      }
    }
    if (doc && !doc->isLive() && !app.loading() && doc->path() == fs::path(finalPath)) {
      ended = true;
      break;
    }
  }
  ASSERT_TRUE(ended);
  ASSERT_NE(lastLive, nullptr);
  const LiveSessionStats stats = app.live().stats();

  // ---- numbers ----
  const double p50 = percentile(latencies, 0.5), p95 = percentile(latencies, 0.95),
               pmax = percentile(latencies, 1.0);
  // Memory: the median of the first quarter of the samples taken once the window was full against the median
  // of the last quarter (single instants fluctuate with the transient timeline index of a snapshot).
  auto quarterMedian = [](const std::vector<double>& v, bool first) {
    if (v.size() < 8) return 0.0;
    const size_t q = v.size() / 4;
    return percentile(first ? std::vector<double>(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(q))
                            : std::vector<double>(v.end() - static_cast<std::ptrdiff_t>(q), v.end()),
                      0.5);
  };
  rssFillRef = quarterMedian(rssEnd, true);
  heapFillRef = quarterMedian(heapEnd, true);
  const double rssLast = quarterMedian(rssEnd, false);
  const double heapLast = quarterMedian(heapEnd, false);
  std::printf(
      "LIVE-VIEWER seconds=%d first_document_s=%.2f swaps=%llu latency_p50=%.3f latency_p95=%.3f "
      "latency_max=%.3f max_window_s=%.2f max_samples_shown=%llu rss_early=%.1fMiB rss_late=%.1fMiB "
      "heap_early=%.1fMiB heap_late=%.1fMiB "
      "snapshot_ms=%.0f states_ms=%.0f lod_ms=%.0f symbols_ms=%.0f total_ms=%.0f(max %.0f) symbol_epochs=%u "
      "max_epochs_alive=%u "
      "modules_loaded=%u modules_adopted=%u\n",
      seconds, firstDocSeconds, static_cast<unsigned long long>(app.liveSwaps()), p50, p95, pmax, maxWindow,
      static_cast<unsigned long long>(maxSamplesShown), rssFillRef, rssLast, heapFillRef, heapLast,
      stats.lastSnapshotMs, stats.lastStatesMs, stats.lastLodMs, stats.lastSymbolsMs, stats.lastTotalMs,
      stats.maxTotalMs, stats.symbols.epochs, maxEpochsAlive, stats.symbols.modulesLoaded,
      stats.symbols.modulesAdopted);

  // ---- the assertions ----
  EXPECT_GE(app.liveSwaps(), static_cast<uint64_t>(seconds));  // about two a second
  ASSERT_GT(latencies.size(), 100u);
  EXPECT_LT(p95, 2.0) << "p95 age of the newest sample on screen (asserted at 2 s; the target is 1 s)";
  std::printf("LIVE-VIEWER latency p95=%.3f s against the 1 s target: %s\n", p95,
              p95 <= 1.0 ? "met" : "NOT met");
  EXPECT_LE(maxWindow, windowSeconds + 0.6) << "the window never exceeds the viewer's window";
  EXPECT_GE(maxWindow, windowSeconds - 0.5) << "the window fills up";
  EXPECT_EQ(maxGaps, 0u) << "no live_gap at 8 kHz";
  std::printf("LIVE-VIEWER threads in the live document: max %u\n", maxModelThreads);
  EXPECT_LE(maxModelThreads, 16u) << "threads of the machine's noise are not carried by the live view";
  EXPECT_EQ(lastLive->live().droppedLive, 0u) << "nothing dropped by the live path";
  ASSERT_GT(rssFillRef, 0.0);
  EXPECT_LT(rssLast, rssFillRef * 1.10)
      << "memory is flat once the window is full: " << rssFillRef << " MiB -> " << rssLast << " MiB";
  if (windowSeconds >= 30)
    EXPECT_LT(rssLast, 200.0) << "viewer memory with the full 30 s window (target < 200 MiB)";
  EXPECT_LT(heapLast, heapFillRef * 1.10 + 2.0)
      << "heap in use is flat once the window is full: " << heapFillRef << " MiB -> " << heapLast << " MiB";
  EXPECT_GE(stats.symbols.epochs, 2u) << "the tables grew while the capture ran";
  EXPECT_LE(stats.symbols.modulesLoaded, static_cast<uint32_t>(lastLive->symbols()->progress().total))
      << "each module's symbol file was loaded at most once over " << stats.symbols.epochs
      << " symbol services";
  EXPECT_GE(stats.symbols.modulesLoaded, 1u);

  // Functions are named in the live document, and the shares match the final trace.
  const std::map<std::string, double> live = leafShares(*lastLive);
  const std::shared_ptr<Document> finalDoc = app.document();
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  const std::map<std::string, double> fin = leafShares(*finalDoc);
  double named = 0;
  for (const auto& [name, share] : live)
    if (name.rfind("hot_", 0) == 0) named += share;
  EXPECT_GT(named, 50.0) << "the spin functions are named in the live snapshot";
  for (const char* fn : {"hot_a", "hot_b", "hot_c", "spin_rest"}) {
    const double a = live.count(fn) ? live.at(fn) : 0.0, b = fin.count(fn) ? fin.at(fn) : 0.0;
    std::printf("LIVE-VIEWER share %s live=%.2f final=%.2f\n", fn, a, b);
    EXPECT_NEAR(a, b, 5.0) << fn;
  }
}
