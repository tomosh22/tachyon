// Duration statistics (WP-35): Stats from a list of instance durations. The query entry points
// (functionStats, scopeStats, threadStateTotals) are declared in query.h.
#pragma once

#include "analysis/query.h"

#include <cstdint>
#include <vector>

namespace tachyon::analysis {

// Thread-safe (no shared state). Builds the Stats of `durations` (ticks): count, min, quartiles (linear
// interpolation, see Stats), max, mean and the log2 histogram (bucket scheme in query.h). Reorders
// `durations` (selection, not a full sort: O(n)). Empty input gives an all-zero Stats.
Stats makeStats(std::vector<uint64_t>& durations);

// Thread-safe: pure function. Approximate heap bytes of a cached Stats (capacity-based).
size_t statsBytes(const Stats& s);

}  // namespace tachyon::analysis
