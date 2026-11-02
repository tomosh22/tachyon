// Builds a Report from the analysis queries (WP-35 frames/stats/thread states, WP-53 scopes and locks).
#pragma once

#include "core/result.h"
#include "model/model.h"
#include "report/report.h"
#include "symbols/symbol_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::report {

struct ReportOptions {
  uint32_t topN = 20;  // functions, scopes and locks listed
  double frameBudgetMs = 16.67;
  std::optional<std::string> frameSeries;  // only the series with this name; nullopt: every series
  bool blendScopes = true;                 // blend instrumentation scopes into the function table
};

// Thread-safe: pure functions. Ticks to milliseconds, rounded to 1e-6 ms; percentages round to 1e-4.
double ticksToMs(uint64_t ticks, uint64_t clockFrequencyHz);
double roundMs(double ms);
double roundPct(double pct);

// Thread-safe: pure function. The lower order statistic d[floor(num * (n - 1) / den)] of an ascending
// sorted, non-empty vector (the rule of the 3.7 query clarifications, evaluated in integer arithmetic).
uint64_t lowerQuantile(const std::vector<uint64_t>& sorted, uint64_t num, uint64_t den);

// Thread-safe for concurrent const access to the arguments. `model.statesByThread` must be built
// (analysis::buildThreadStates) and `symbols` resolved (SymbolService::waitAll). InvalidArgument when
// options.frameSeries names no series of the trace.
Result<Report> buildReport(const model::TraceModel& model, const symbols::SymbolService& symbols,
                           const ReportOptions& options);

}  // namespace tachyon::report
