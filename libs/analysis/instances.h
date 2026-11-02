// Instance lists (WP-47): the instances behind functionStats / scopeStats, not only their statistics. The
// timeline overlay and the next / previous navigation of the Function Statistics panel need to know where
// each instance is. The lists are computed with exactly the rules of the statistics (one shared enumeration
// in detail/instance_enum.h), so count, durations (end - begin), min, max and sum equal the Stats of the same
// query.
//
// Instance semantics: see query.h (functionStats: sampled-function runs; scopeStats: exact Begin/End pairs).
// A sampled-function instance spans [first sample ts, last sample ts + the last sample's weight); a scope
// instance spans [Begin ts, End ts] (an unterminated scope ends at header.end_ts).
#pragma once

#include "analysis/query.h"
#include "core/result.h"
#include "core/strong_id.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace tachyon::analysis {

struct Instance {
  ThreadIdx thread;
  uint64_t begin = 0, end = 0;  // ticks; the duration is end - begin

  // Thread-safe (value type).
  bool operator==(const Instance&) const = default;
};

// Thread-safe. Instances of a sampled function (a key with module kScopeModule is answered as
// scopeInstances), selected and delimited exactly as functionStats(ctx, filter, key) counts them. Sorted by
// begin, then by thread, then by end. Cached in the context's QueryCache (QueryKind::Instances); the result
// is immutable and survives eviction.
Result<std::shared_ptr<const std::vector<Instance>>> functionInstances(const QueryContext& ctx,
                                                                       const Filter& filter, FunctionKey key,
                                                                       Cancel cancel = {});

// Thread-safe. Instances of an instrumentation scope id, selected exactly as scopeStats (the Begin lies in
// [t0, t1) and its thread is selected; subtreeRoot and function are ignored). Same order and caching as
// functionInstances.
Result<std::shared_ptr<const std::vector<Instance>>> scopeInstances(const QueryContext& ctx,
                                                                    const Filter& filter, StringIdx scopeId,
                                                                    Cancel cancel = {});

}  // namespace tachyon::analysis
