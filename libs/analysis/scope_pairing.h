// Pairing of instrumentation Begin/End events into scope instances (WP-35; spec 3.1 ScopeEventRec, 3.8 rule
// 5).
//
// Reusable by the scope blending of WP-53, which extends the interface with fiber-aware splitting: keep this
// interface narrow. The input is TraceModel::scopeEvents (sorted by ts, ties in file order). Events are
// paired per thread (by tid), with stack discipline:
//   * Begin pushes; End pops the innermost open scope of the same thread and closes it at the End's ts (the
//     End record carries no id: the pairing is positional);
//   * an End with no open Begin on its thread is an orphan: it is ignored and its ts is recorded;
//   * a Begin still open after the last event is unterminated: it ends at header.end_ts (or at its own begin
//     when that is later) and is flagged;
//   * events whose tid maps to no thread (tid 0 or unknown) are dropped and counted.
// Lock scopes (color sentinel 0x4C4F43FF, id "Tachyon.LockAcquire") are ordinary scopes here.
#pragma once

#include "analysis/query.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tachyon::analysis {

// One paired scope. Note: distinct from model::ScopeInstance (which WP-53 fills and which has data, color).
struct ScopeInstance {
  StringIdx id;  // StringIdx in TraceModel::strings
  uint64_t begin = 0, end = 0;
  uint32_t depth = 0;  // number of scopes open on the thread when this one began (0 = outermost)
  uint64_t callerAddr = 0;
  bool unterminated = false;  // no End event: end = header.end_ts
};

struct PairedScopes {
  // Indexed by ThreadIdx; each vector is sorted by begin (an enclosing scope before the scopes it contains
  // when begins are equal, i.e. by event order).
  std::vector<std::vector<ScopeInstance>> byThread;
  // Indexed by ThreadIdx: timestamps of the orphan End events, ascending.
  std::vector<std::vector<uint64_t>> orphanEndTs;
  uint64_t droppedEvents = 0;      // events of tid 0 / an unknown tid
  uint64_t unterminatedCount = 0;  // total over all threads

  // Thread-safe (const access). Approximate heap bytes, for the query cache.
  size_t heapBytes() const;
};

// Thread-safe for concurrent const access to the model. Pairs every thread's scope events. Cost O(events).
// Returns Cancelled if `cancel` fires (checked every 65536 events).
Result<PairedScopes> pairScopes(const model::TraceModel& model, Cancel cancel = {});

}  // namespace tachyon::analysis
