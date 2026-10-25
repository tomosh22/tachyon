// Frame boundaries from FrameMark events (WP-35, spec 3.8 rule 6). The queries themselves (frameSeries,
// frames, slowestFrames) are declared in query.h.
#pragma once

#include "analysis/query.h"
#include "model/model.h"

#include <cstdint>
#include <vector>

namespace tachyon::analysis {

// One frame of a series: [start, end) between two consecutive marks of the series.
struct FrameSpan {
  uint64_t index = 0;   // ordinal in the series
  uint64_t number = 0;  // frame_number of the opening mark
  uint64_t start = 0, end = 0;
};

// Thread-safe (const access). The frames of `series` (matched on thread and name; markCount is ignored): the
// series' marks stably sorted by ts, frame i = [mark_i, mark_{i+1}); n marks give n - 1 frames, none for n
// < 2.
std::vector<FrameSpan> frameSpans(const model::TraceModel& model, const FrameSeries& series);

}  // namespace tachyon::analysis
