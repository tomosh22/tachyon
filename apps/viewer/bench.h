// tachyon-viewer --bench (WP-64): a headless frame-time benchmark of the real viewer on a real trace.
//
// Opens the trace with the Null renderer on a tall display (so that every row of the timeline is on screen),
// waits for the load, the timeline index and the symbols, then measures the CPU time of whole App frames
// (events, jobs, every open panel's draw, ImGui NewFrame..Render, no GPU) in three phases:
//   steady  no input                        - the cost of an idle frame
//   pan     the view slides across the trace - every row re-queried each frame
//   zoom    the view zooms from the whole trace down to about a millisecond and back out
// and prints the p50 / p95 / max of each phase next to the 60 fps budget (16.7 ms). Also prints the load and
// index build times and the memory numbers. Meant to be run under the viewer's own capture (dogfooding):
//   tachyon-cli capture --launch tachyon-viewer --args --bench large.tachyon
#pragma once

#include <filesystem>
#include <iosfwd>

namespace tachyon::viewer {

class App;

struct BenchOptions {
  int framesPerPhase = 300;
  double fpsBudgetMs = 1000.0 / 60.0;
};

// Main-thread only. Returns 0 when the trace loaded and the benchmark ran (whether or not the budget held;
// the verdict is in the output), 1 when the trace could not be opened.
int runBench(App& app, const std::filesystem::path& trace, const BenchOptions& options, std::ostream& out);

}  // namespace tachyon::viewer
