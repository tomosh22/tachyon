// Turns a StatsSnapshot into the numbers WP-01 acceptance needs, and formats them as text or JSON.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "etw_stats.h"

namespace tachyon::etlstat {

// Facts from the ETL logfile header (TRACE_LOGFILE_HEADER); `valid` is false for hex replays.
struct TraceFileInfo {
  bool valid = false;
  uint64_t perfFreq = 10'000'000;  // QPC ticks per second
  uint32_t eventsLost = 0;
  uint32_t buffersLost = 0;
  uint32_t buffersWritten = 0;
  uint32_t numberOfProcessors = 0;
  uint32_t pointerSize = 8;
};

struct ReportOptions {
  uint32_t expectedHz = 8000;
  std::optional<uint32_t> pid;
  std::string image;
  // Acceptance thresholds from the WP-01 brief.
  double minRatePerBusyCore = 7500.0;
  double minMatchedFraction = 0.95;
  // A CPU is "busy" when at least this fraction of its samples hit a non-idle thread.
  double busyFraction = 0.5;
};

struct CpuRate {
  uint32_t cpu = 0;
  double samplesPerSecond = 0;
  double nonIdleFraction = 0;
  bool busy = false;
};

struct Report {
  double sampleSeconds = 0;  // first to last SampledProfile event
  double traceSeconds = 0;   // first to last event of any kind
  std::vector<CpuRate> cpus;
  uint32_t busyCpuCount = 0;
  double minBusyRate = 0;  // lowest per-busy-CPU rate (0 when no busy CPU)
  double meanBusyRate = 0;
  double totalSamplesPerSecond = 0;
  double matchedFraction = 0;         // SampledProfile with stack / all SampledProfile
  double matchedFractionNonIdle = 0;  // same, excluding idle (ThreadId 0)
  bool hasProcessFilter = false;
  std::vector<uint32_t> filterPids;
  uint64_t processSamples = 0;
  double processSamplesPerSecond = 0;
  std::vector<std::pair<uint32_t, double>>
      topThreads;  // (thread id, samples/s) of the process, busiest first
  bool rateAcceptance = false;
  bool matchAcceptance = false;
};

// Thread-safe: pure function.
Report buildReport(const StatsSnapshot& snapshot, const TraceFileInfo& info, const ReportOptions& options);

// Thread-safe: pure function. Multi-line human readable text.
std::string formatReportText(const StatsSnapshot& snapshot, const TraceFileInfo& info, const Report& report,
                             const ReportOptions& options);
// Thread-safe: pure function. Pretty-printed JSON.
std::string formatReportJson(const StatsSnapshot& snapshot, const TraceFileInfo& info, const Report& report,
                             const ReportOptions& options);

}  // namespace tachyon::etlstat
