// Trace file -> Report: loads the model, builds thread states, resolves symbols and runs buildReport.
#pragma once

#include "core/result.h"
#include "report/report.h"
#include "report/report_builder.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::report {

struct AnalyzeOptions {
  ReportOptions report;
  // `.json` files are tracegen/JSON symbol files registered as overrides; directories and other files are
  // symbol search paths (symbol store layout, `.build-id`, next to the module).
  std::vector<std::filesystem::path> symbolPaths;
  std::vector<std::string> symbolServers;
  std::filesystem::path symbolCacheDir;  // empty: the locator's default under the system temp directory
  // Register "<trace stem>.symbols.json" next to the trace when it exists (the test fixtures and developer
  // convenience, like the viewer).
  bool useSidecarSymbols = true;
};

// Thread-safe (no shared state); loads and resolves on worker threads internally. NotFound / IoError /
// CorruptData when the trace cannot be read; InvalidArgument for bad options (unknown frame series, missing
// symbol path).
Result<Report> analyzeTrace(const std::filesystem::path& tracePath, const AnalyzeOptions& options);

}  // namespace tachyon::report
