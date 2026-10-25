// The single enumeration of function / scope instances behind both the statistics (stats.cpp) and the
// instance lists (instances.cpp), WP-47. Not part of the public API.
#pragma once

#include "analysis/detail/query_support.h"
#include "analysis/instances.h"

#include <cstdint>
#include <vector>

namespace tachyon::analysis::detail {

struct EnumeratedInstances {
  // Per selected thread (ascending ThreadIdx), each thread's instances in time order.
  std::vector<Instance> list;
  uint32_t unterminated = 0;  // scopes: instances in `list` whose Begin has no End
  uint64_t orphanEnds = 0;    // scopes: orphan End events on the selected threads with ts in [t0, t1)
};

// Thread-safe. Sampled-function instances under the rules documented at functionStats in query.h. `rf` comes
// from resolveFilter(). Returns Cancelled when `cancel` fires.
Result<EnumeratedInstances> enumerateFunctionInstances(const QueryContext& ctx, const ResolvedFilter& rf,
                                                       FunctionKey key, Cancel cancel);

// Thread-safe. Scope instances under the rules documented at scopeStats in query.h. `rf` comes from
// resolveFilterBasic(). Returns Cancelled when `cancel` fires.
Result<EnumeratedInstances> enumerateScopeInstances(const QueryContext& ctx, const ResolvedFilter& rf,
                                                    StringIdx scopeId, Cancel cancel);

}  // namespace tachyon::analysis::detail
