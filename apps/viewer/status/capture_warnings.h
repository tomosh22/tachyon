// Capture warnings (WP-48): what a trace says about the quality of its own data, read from the model and the
// CaptureInfo JSON the converters write (libs/etlconvert, libs/capture/linux_perf, libs/inst/merge.cpp):
//   * events_lost > 0 (and ETW `host.etl_buffers_lost`),
//   * `throttle` (perf: the kernel throttled sampling),
//   * dropped instrumentation records: `instrumentation.merges[].dropped_records` and `tachyon.dropped`
//     markers, plus the merges' own `warnings`,
//   * the converter's `convert_stats.warnings` and a top-level `warnings` array,
//   * a partial (unfinalized) trace.
#pragma once

#include "model/model.h"

#include <string>
#include <vector>

namespace tachyon::viewer {

enum class CaptureWarningKind : uint8_t {
  EventsLost,
  Throttled,
  DroppedInstrumentation,
  InstrumentationWarning,
  ConverterWarning,
  Partial,
  MemoryDegraded,  // WP-64: the memory ceiling made the viewer free optional data (added by the App, not by
                   // the model)
  AgentWarning     // WP-67: reported by the capture agent in `capture_done` (spool caps, refused connections,
                   // failed merges); not part of the trace
};

struct CaptureWarning {
  CaptureWarningKind kind;
  std::string text;
};

// Thread-safe: pure function on the model. Never fails; a CaptureInfo that is missing or not a JSON object
// contributes nothing. At most kMaxCaptureWarnings entries (a final "... and N more" line is added).
// WP-67: `agentWarnings` are the free-text warnings of the agent's `capture_done` for this trace; they are
// appended as AgentWarning. Duplicates are dropped by meaning, not only by exact text: a lost-events or a
// throttle sentence in `convert_stats.warnings` or in `agentWarnings` is not shown when the structured
// EventsLost / Throttled line already says it, and an exact repeat of an earlier text is dropped.
inline constexpr size_t kMaxCaptureWarnings = 32;
std::vector<CaptureWarning> collectCaptureWarnings(const model::TraceModel& model,
                                                   const std::vector<std::string>& agentWarnings = {});

}  // namespace tachyon::viewer
