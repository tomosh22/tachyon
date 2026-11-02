// tachyon-report command line (WP-62). apps/report/main.cpp is a thin wrapper over run().
//
//   tachyon-report <trace.tachyon> [--baseline base.json] [--out report.json|report.md]...
//                  [--format json|md] [--top N] [--frame-series NAME] [--frame-budget-ms MS]
//                  [--thresholds thresholds.json] [--symbols PATH...] [--symbol-server URL...]
//                  [--symbol-cache DIR] [--no-blend] [--fail-on-lost]
//
// Exit codes:
//   0  report written; no regression against the baseline
//   1  regression against the baseline (the reports are still written)
//   2  usage error (bad option, bad thresholds file, unknown frame series, bad symbol path)
//   3  trace, baseline or output file cannot be read or written, or is corrupt
//   4  --fail-on-lost was given and the trace is partial or lost events (and there is no regression)
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace tachyon::report {

inline constexpr int kExitOk = 0;
inline constexpr int kExitRegression = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitIoError = 3;
inline constexpr int kExitLostEvents = 4;

// Thread-safe (no shared state). args excludes argv[0]. Reports go to the --out files, or to `out` in
// --format when no --out is given; diagnostics and the usage text go to `err`.
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace tachyon::report
