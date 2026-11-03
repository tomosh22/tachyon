// Small pure helpers shared by the analysis panels (extracted from the Call Graph by WP-44).
#pragma once

#include "analysis/query.h"
#include "viewer/selection_bus.h"

#include <cstdint>
#include <optional>
#include <string>

namespace tachyon::viewer::common {

// Thread-safe: pure function. SelectionBus filter -> analysis::Filter. No bus filter means the whole trace.
// ViewFilter::function is deliberately not mapped: a bus-level FunctionIdx is not a handle of a particular
// QueryContext (contexts are rebuilt when symbols resolve).
analysis::Filter toQueryFilter(const std::optional<ViewFilter>& filter);

// Thread-safe: pure function. WP-33b: a bus filter with a subtree path restricts queries to it only under the
// frame options the path was taken with. Overwrites the four option values with them when the filter carries
// a path (with options); otherwise leaves them unchanged.
void applySubtreeOptions(const std::optional<ViewFilter>& filter, bool& expandInline,
                         analysis::KernelFrames& kernelFrames, bool& blendScopes, bool& groupByFiber);

// Thread-safe: pure function. True when the bus filter has any subtree restriction (root node or path).
bool hasSubtreeRestriction(const std::optional<ViewFilter>& filter);

// Thread-safe: pure function. Removes every subtree restriction (root node and path, with its options) from
// an analysis filter / a bus filter.
void clearSubtree(analysis::Filter& filter);
void clearSubtree(ViewFilter& filter);

// Thread-safe: pure function. Short description of the subtree restriction for status texts: the label of the
// path's last function, "path of N frames", or "node #N" for a subtreeRoot; empty without a restriction.
std::string subtreeText(const ViewFilter& filter);

// Thread-safe: pure function. Ticks to milliseconds.
double ticksToMs(uint64_t ticks, double clockHz);

}  // namespace tachyon::viewer::common
