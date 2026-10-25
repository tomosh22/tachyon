// Fiber-aware scope pairing (WP-53; docs/specs/sdk-api.md, inst-wire.md "Fibers", trace-format
// FiberEventRec).
//
// Which fiber runs on a thread over time comes from TraceModel::fiberSwitches (sorted by ts):
//   * switch_end(fiber F) on thread T: F starts running on T. If T was running another fiber G (the
//     switch_begin was lost) G stops at that instant; if F was running on another thread it stops there.
//   * switch_begin(F, next): F stops running (on the thread it was running on). Until the next switch_end
//     on that thread the thread runs no fiber ("thread context").
//   * unregister(F): like switch_begin when F is running; open scopes of F end here (unterminated).
//   * register(F) only makes the fiber known (a lane exists for it); it does not make it run.
// A thread with no fiber events runs the thread context for the whole trace.
//
// Scope events are attributed to the execution context current on their thread: the running fiber, or the
// thread context. Each context has its own scope stack (Begin pushes, End pops the innermost scope of the
// context, positional as in scope_pairing.h), so a scope begun on thread A by a fiber that is resumed on
// thread B is closed by the End that B emits for that fiber. Tie order at equal timestamps between the event
// streams: the End events that open the scope stream's tie group, then the fiber events (file order), then
// the remaining scope events. So a scope that ends when its fiber is switched out still belongs to that
// fiber, and one that begins when a fiber is entered belongs to the entering fiber. Within one stream ties
// keep file order.
//
// Each scope instance is split into pieces: the stretches of [begin, end) during which its context really ran
// (a fiber's switched-out time is excluded). A fiber's pieces on one thread never overlap another fiber's.
// The instance itself keeps wall-clock begin/end (what scopeStats and lockSummary report).
#pragma once

#include "analysis/query.h"
#include "analysis/scope_pairing.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tachyon::analysis {

// A stretch during which a context ran on a thread. [begin, end), sorted and disjoint within a lane. The last
// run of a fiber that is still running ends at header.end_ts; the thread context's last run ends at
// UINT64_MAX (it also covers a trace without FiberEvents).
struct FiberRun {
  ThreadIdx thread;
  uint64_t begin = 0, end = 0;
};

// One piece of a scope instance of a lane: the intersection of the instance with a run.
struct ScopePiece {
  uint32_t scope = 0;  // index into FiberLane::scopes
  ThreadIdx thread;
  uint64_t begin = 0, end = 0;  // [begin, end), non-empty
};

struct FiberLane {
  // The fiber id; 0 for the thread-context lane of `thread` (scopes begun while no fiber ran on it).
  uint64_t fiberId = 0;
  ThreadIdx thread;  // valid only when fiberId == 0
  std::vector<FiberRun> runs;
  // Logical instances (wall-clock begin/end, depth within the lane's own stack), sorted by begin (an
  // enclosing scope before the scopes it contains when begins are equal).
  std::vector<ScopeInstance> scopes;
  // Sorted by (begin, depth, scope). Zero-length instances have no pieces.
  std::vector<ScopePiece> pieces;
};

// Result of the fiber-aware pairing. `scopes` has the layout of the plain pairing: instances grouped by the
// thread of their Begin, sorted by begin, with depth within their own context's stack (a fiber lane's, or the
// thread's) and orphan Ends per thread; unterminated scopes end at header.end_ts, except those still open
// when their fiber is unregistered, which end at the unregister (both flagged).
struct PairedFibers {
  PairedScopes scopes;
  bool fiberAware =
      false;  // false: the trace has no FiberEvents, `lanes` is empty, `scopes` is the plain pairing
  // Sorted by (fiberId, thread). Real fibers (fiberId != 0) include registered fibers without scopes; the
  // thread-context lanes are present only for threads that have scopes in them.
  std::vector<FiberLane> lanes;

  // Thread-safe (const access). Approximate heap bytes, for the query cache.
  size_t heapBytes() const;
};

// Thread-safe for concurrent const access to the model. Pairs the scope events per execution context (see
// above). Without FiberEvents it equals pairScopes(). Cost O(events + pieces). Cancelled if `cancel` fires.
Result<PairedFibers> pairScopesByFiber(const model::TraceModel& model, Cancel cancel = {});

// Thread-safe (the cache locks internally). The fiber-aware pairing, cached (QueryKind::ScopePairs, arg 1).
Result<std::shared_ptr<const PairedFibers>> pairedFibersFor(const QueryContext& ctx, Cancel cancel = {});

// Thread-safe. The pairing the queries of `ctx` use: the fiber-aware one when ctx.options().groupByFiber and
// the trace has FiberEvents, else the plain per-thread one (cached, QueryKind::ScopePairs, arg 0). The
// pointer keeps the underlying result alive.
Result<std::shared_ptr<const PairedScopes>> pairedScopesFor(const QueryContext& ctx, Cancel cancel = {});

// Thread-safe: pure function. Whether the model has any FiberEvents.
inline bool hasFiberEvents(const model::TraceModel& m) {
  return !m.fiberSwitches.empty();
}

}  // namespace tachyon::analysis
