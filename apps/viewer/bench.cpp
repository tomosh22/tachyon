#include "viewer/bench.h"

#include "viewer/app.h"
#include "viewer/paths.h"
#include "viewer/timeline/timeline_panel.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ostream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

namespace tachyon::viewer {

namespace {

using Clock = std::chrono::steady_clock;

// CPU time of the calling thread in milliseconds (wall time where there is no per-thread clock).
double threadCpuMs() {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
    const auto to100ns = [](const FILETIME& f) {
      return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(to100ns(kernel) + to100ns(user)) * 1e-4;
  }
#elif defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
#endif
  return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

struct PhaseStats {
  double p50 = 0, p95 = 0, max = 0, wallMean = 0;
};

PhaseStats summarize(std::vector<double> cpu, double wallTotalMs) {
  PhaseStats s;
  if (cpu.empty()) return s;
  std::sort(cpu.begin(), cpu.end());
  s.p50 = cpu[cpu.size() / 2];
  s.p95 = cpu[std::min(cpu.size() - 1, cpu.size() * 95 / 100)];
  s.max = cpu.back();
  s.wallMean = wallTotalMs / static_cast<double>(cpu.size());
  return s;
}

}  // namespace

int runBench(App& app, const std::filesystem::path& trace, const BenchOptions& options, std::ostream& out) {
  const auto start = Clock::now();
  app.openFile(trace);
  while (app.loading()) {
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (!app.document()) {
    out << "bench: FAILED " << (app.lastError().empty() ? "trace did not load" : app.lastError()) << '\n';
    return 1;
  }
  const auto loaded = Clock::now();
  const model::TraceModel& model = app.document()->model();
  PanelEntry* entry = app.panels().find(ThreadsTimelinePanel::kName);
  auto* panel = entry ? dynamic_cast<ThreadsTimelinePanel*>(entry->panel.get()) : nullptr;
  if (!panel) {
    out << "bench: FAILED the timeline panel is missing\n";
    return 1;
  }
  timeline::TimelineWidget& widget = panel->widget();
  entry->open = true;

  // The timeline index (background job), then the symbols; the analysis panels query meanwhile.
  while (!widget.lodFinished()) {
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto indexed = Clock::now();
  for (int spins = 0; spins < 60000 && app.document()->symbols(); ++spins) {
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    if (p.total == 0 || p.done >= p.total) break;
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // Let the panels' first queries finish and the layout settle; then give the timeline the whole display.
  for (int i = 0; i < 30; ++i) {
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ImGui::SetWindowPos(ThreadsTimelinePanel::kName, ImVec2(0, 20));
  ImGui::SetWindowSize(ThreadsTimelinePanel::kName, ImVec2(1900, 5900));
  for (int i = 0; i < 10; ++i) app.frame(0);
  const auto ready = Clock::now();

  uint64_t samples = 0;
  for (const auto& s : model.samplesByThread) samples += s.ts.size();
  const timeline::TimeLimits limits = widget.limits();
  out << "bench: file=" << pathToUtf8(trace.filename()) << " threads=" << model.threads.size()
      << " samples=" << samples << " modules=" << model.modules.size() << " nodes=" << model.nodes.size()
      << '\n';
  char line[320];
  std::snprintf(line, sizeof line,
                "bench: open %.2f s (load + thread states), timeline index +%.2f s, symbols + settle +%.2f s",
                seconds(start, loaded), seconds(loaded, indexed), seconds(indexed, ready));
  out << line << '\n';
  std::snprintf(line, sizeof line, "bench: resident %.0f MiB, timeline index %.0f MiB, ceiling %.0f MiB",
                static_cast<double>(model.residentBytes()) / 1048576.0,
                static_cast<double>(widget.lodBytes()) / 1048576.0,
                static_cast<double>(app.memoryCeiling()) / 1048576.0);
  out << line << '\n';

  const int n = options.framesPerPhase;
  struct Phase {
    const char* name;
    PhaseStats stats;
    uint32_t rowsDrawn = 0, rowsPending = 0;
  };
  std::vector<Phase> phases;
  const double total = limits.total();
  auto runPhase = [&](const char* name, const auto& viewAt) {
    std::vector<double> cpu;
    cpu.reserve(static_cast<size_t>(n));
    const auto wall0 = Clock::now();
    uint32_t drawn = 0, pending = 0;
    for (int i = 0; i < n; ++i) {
      viewAt(i);
      const double c0 = threadCpuMs();
      app.frame(0);
      cpu.push_back(threadCpuMs() - c0);
      drawn = std::max(drawn, widget.frameStats().rowsDrawn);
      pending = std::max(pending, widget.frameStats().rowsPending);
    }
    const double wallMs = std::chrono::duration<double, std::milli>(Clock::now() - wall0).count();
    phases.push_back({name, summarize(std::move(cpu), wallMs), drawn, pending});
  };

  widget.setView({0.0, total});
  runPhase("steady", [&](int) {});
  // Pan across a 1/20 window; the offset is not a multiple of a pixel so the row caches never hit.
  const double panSpan = total / 20.0;
  runPhase("pan", [&](int i) {
    widget.setView(
        {(total - panSpan) * static_cast<double>(i) / static_cast<double>(std::max(n - 1, 1)) + 0.37,
         panSpan});
  });
  // Zoom from the whole trace to ~1 ms (clock-dependent: 1e-3 * ticks/s) and back out, logarithmically.
  const double minSpan =
      std::max(static_cast<double>(model.header.clock_frequency_hz) * 1e-3, limits.minSpan);
  runPhase("zoom", [&](int i) {
    const double t = std::fabs(2.0 * static_cast<double>(i) / static_cast<double>(std::max(n - 1, 1)) - 1.0);
    const double span = std::exp(std::log(minSpan) + (std::log(total) - std::log(minSpan)) * t);
    widget.setView({(total - span) * 0.4 + 0.11, span});
  });

  bool allUnderBudget = true;
  for (const Phase& p : phases) {
    std::snprintf(
        line, sizeof line,
        "bench: %-6s frames=%d cpu_ms p50=%.2f p95=%.2f max=%.2f (wall mean %.2f) rows drawn=%u pending=%u",
        p.name, n, p.stats.p50, p.stats.p95, p.stats.max, p.stats.wallMean, p.rowsDrawn, p.rowsPending);
    out << line << '\n';
    allUnderBudget = allUnderBudget && p.stats.p95 <= options.fpsBudgetMs;
  }
  std::snprintf(line, sizeof line, "bench: 60 fps budget %.1f ms (p95 of CPU frame time): %s",
                options.fpsBudgetMs, allUnderBudget ? "HOLDS" : "EXCEEDED");
  out << line << '\n';
  return 0;
}

}  // namespace tachyon::viewer
