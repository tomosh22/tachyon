// Small pure helpers shared by the analysis panels (extracted from the Call Graph by WP-44).
#pragma once

#include "analysis/query.h"
#include "viewer/selection_bus.h"

#include <cstdint>
#include <optional>

namespace tachyon::viewer::common {

// Thread-safe: pure function. SelectionBus filter -> analysis::Filter. No bus filter means the whole trace.
// ViewFilter::function is deliberately not mapped: a bus-level FunctionIdx is not a handle of a particular
// QueryContext (contexts are rebuilt when symbols resolve).
analysis::Filter toQueryFilter(const std::optional<ViewFilter>& filter);

// Thread-safe: pure function. Ticks to milliseconds.
double ticksToMs(uint64_t ticks, double clockHz);

}  // namespace tachyon::viewer::common
