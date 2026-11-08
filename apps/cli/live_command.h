// `tachyon-cli live` (WP-65a): starts a live capture, subscribes to it and prints a one-line summary of the
// live window once a second, built from LiveModel snapshots (docs/specs/live.md).
//
//   tachyon-cli [--agent ADDR | --socket PATH | --pipe NAME] [--token-file F] [--json] [--timeout S]
//       live (--launch EXE [--args "A B" | --arg A]... | --attach PID | --system)
//            [--duration S] [--window S] [--interval S] [--hz N] [--follow-children] [--child-filter GLOB]
//            [--cwd D] [--env K=V]... [--circular-mb N] [--out PATH]
//
// --duration S   stop the capture after S seconds (default: until the launched target exits or Ctrl-C kills
//                the CLI; --system and --attach need --duration)
// --window S     seconds of trace the CLI keeps in its live window (default 30; what a late subscriber can get is
//                the agent's own --live-window-seconds)
// --interval S   seconds between summary lines (default 1)
//
// One line per interval: elapsed time, the window span, samples in the window, the five functions with the
// most exclusive samples (module+RVA, or the absolute address; no symbols are resolved), the latency of the
// newest sample (this machine's clock minus its timestamp: only for a local agent on Linux, "n/a" otherwise),
// the number of live gaps survived and events the agent dropped. With --json each line is a JSON object
// {"t","window_s","samples","latency_s","gaps","dropped","top":[{"name","samples","pct"}]}. At the end the
// capture is stopped and the path of the final trace is printed. Exit codes as in cli.h.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace tachyon::cli {

// Thread-safe (no shared state). `args` are the global options plus the live options (everything except the
// word "live").
int runLive(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err);

}  // namespace tachyon::cli
