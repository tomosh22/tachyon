// CI report (WP-62): the data model of a tachyon-report run. A Report is computed from the analysis queries
// (report_builder.h), serialised as JSON schema version 1 (report_json.h, apps/report/README.md), compared
// with a baseline Report (baseline.h) and rendered as Markdown (markdown.h).
//
// Times are milliseconds (converted from trace ticks with TraceHeader.clock_frequency_hz) rounded to 1e-6;
// percentages are rounded to 1e-4. Every list has a deterministic order (ties broken by name).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::report {

inline constexpr int kSchemaVersion = 1;
inline constexpr size_t kMaxFrameSeries = 16;
inline constexpr size_t kMaxThreads = 256;
inline constexpr size_t kMaxWarnings = 32;

struct TraceSummary {
  double durationMs = 0;
  std::string platform;  // "windows", "linux" or "unknown"
  uint64_t clockFrequencyHz = 0;
  uint64_t samplingHz = 0;  // 0 if the capture did not record it
  uint64_t eventsLost = 0;
  bool partial = false;  // unfinalized or torn file
  uint64_t throttleEvents = 0;
  uint64_t threadCount = 0;
  uint64_t sampleCount = 0;
  bool hasInstrumentation = false;
  std::vector<std::string> warnings;  // CaptureInfo "warnings", at most kMaxWarnings
};

struct ReportSettings {
  uint32_t topN = 20;
  double frameBudgetMs = 16.67;
  bool blendScopes = false;  // effective: requested and the trace has instrumentation scopes
  std::optional<std::string> frameSeriesFilter;
};

struct FrameSeriesRow {
  std::string name;
  std::optional<uint32_t> threadTid;  // nullopt: global marks
  std::string threadName;
  std::string label;  // "name" or "name@thread"; the baseline matching key
  bool isDefault = false;
  uint32_t markCount = 0;
  uint64_t frameCount = 0;
  double p50Ms = 0, p95Ms = 0, p99Ms = 0, maxMs = 0, meanMs = 0;
  uint64_t overBudget = 0;
  uint64_t slowestFrameNumber = 0;
};

struct ThreadRow {
  uint32_t tid = 0;
  std::string name;
  double runningMs = 0, readyMs = 0, waitingMs = 0, unknownMs = 0;
  uint64_t sampleCount = 0;
};

struct FunctionReportRow {
  std::string name;
  std::string module;
  bool isScope = false;
  double exclusiveMs = 0, inclusiveMs = 0;
  double exclusivePct = 0;  // of the total exclusive time of all functions
  uint64_t samples = 0;
};

struct ScopeReportRow {
  std::string name;
  uint64_t count = 0;
  double minMs = 0, medianMs = 0, p95Ms = 0, maxMs = 0, totalMs = 0, meanMs = 0;
  uint64_t unterminated = 0, orphanEnds = 0;
};

struct LockWaiterRow {
  uint32_t tid = 0;
  std::string name;
  uint64_t waits = 0;
  double waitMs = 0;
};

struct LockReportRow {
  std::string name;     // "" when the lock was only identified by its address
  std::string address;  // "0x<hex>"
  uint64_t contentions = 0;
  double totalWaitMs = 0, maxWaitMs = 0;
  uint64_t unterminated = 0;
  std::vector<LockWaiterRow> topThreads;
};

struct Report {
  TraceSummary trace;
  ReportSettings settings;
  uint64_t frameSeriesTotal = 0;  // series in the trace; `frames` lists at most kMaxFrameSeries
  std::vector<FrameSeriesRow> frames;
  std::vector<ThreadRow> threads;
  double totalExclusiveMs = 0;
  uint64_t functionCount = 0;
  std::vector<FunctionReportRow> functions;  // top N by exclusive time, exclusive > 0
  uint64_t scopeCount = 0;
  std::vector<ScopeReportRow> scopes;  // top N by total time
  uint64_t lockCount = 0;
  uint64_t unparsedLockWaits = 0;
  std::vector<LockReportRow> locks;  // top N by total wait
};

}  // namespace tachyon::report
