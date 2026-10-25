// Thread state intervals (WP-31, Part 3.8 rule 4).
//
// buildThreadStates turns the raw ContextSwitchRec / ReadyRec streams of a TraceModel into one sorted,
// contiguous, non-overlapping vector of ThreadStateInterval per thread (TraceModel::statesByThread).
// Intervals are half-open [start, end) in trace-clock ticks and cover [header.start_ts, end) for every
// thread, where end is header.end_ts or the thread's exit.
//
// State machine per thread (events of the thread merged by ts; at equal ts: switch-outs, then ReadyRecs, then
// switch-ins; ties inside one stream keep file order):
//   * Initial state is Unknown from header.start_ts until the first switch event (the thread is NOT inferred
//   to
//     have been Running before a first switch-out). A thread with no switch events is Unknown throughout.
//   * Switch-in at t (record new_tid == tid) starts Running{cpu} at t.
//   * Switch-out at t (record old_tid == tid) with old_thread_stack S starts an off-CPU interval at t that
//   lasts
//     to the next switch-in: Waiting, or Ready when old_state says ready/standby (wait-reason table in
//     wait_reasons.h). It carries wait_reason, blocking_stack = S and the cpu of the switch-out.
//   * ReadyRec (readied_tid == tid) at tr while the thread is off-CPU and waiting splits the interval into
//     Waiting [t, tr) and Ready [tr, switch-in). Both pieces carry the same blocking_stack, unblocking_stack
//     = readying_stack, woke_by_tid = readying_tid and ready_ts = tr. If tr == t the Waiting piece is empty
//     and dropped. Only the first ReadyRec of an off-CPU interval counts.
//   * A ReadyRec while the thread is off-CPU as preempted (old_state ready) does not split; it attaches
//     unblocking_stack, woke_by_tid and ready_ts = tr to the single Ready interval. A preempted interval with
//     no ReadyRec has ready_ts = its start (runnable since the switch-out) and no unblocking information. A
//     Waiting interval with no ReadyRec has ready_ts = 0.
//   * A ReadyRec while the thread is Running or Unknown (before its first switch-out) is ignored: nothing is
//     waiting. A ReadyRec at or after the thread's last switch-out with no later switch-in leaves the trace
//     ending in Ready (the split interval ends at the trace end).
//   * Lost-event gaps. A switch-in while the thread is already Running (two consecutive switch-ins), or a
//     switch-out while the thread is already off-CPU (two consecutive switch-outs), means records were lost.
//     The interval from the last known state change (start of the Running / off-CPU interval) to the
//     offending event becomes one Unknown interval (merged with an adjacent Unknown), and the offending event
//     then starts its normal state. Any pending ReadyRec information of the discarded interval is dropped.
//   * Thread exit. A switch-out whose old_state says Terminated (4, both platforms) ends the
//     thread at that ts: nothing is emitted afterwards and later events of the tid are ignored. The thread's
//     last_ts also ends it when it is non-zero, below header.end_ts and later than every event the model
//     knows for the thread (this is how TraceModel::Thread::last_ts looks when it came from ThreadRec::end_ts
//     rather than from the thread's last event).
//   * Events outside [header.start_ts, end) are clamped into it. Zero-length intervals are never emitted.
//
// Running intervals carry cpu; Waiting/Ready carry wait_reason, blocking_stack, unblocking_stack (invalid
// when none), woke_by_tid (0 when none) and ready_ts; Unknown intervals have all of these zero / invalid.
#pragma once

#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <cstdint>

namespace tachyon::analysis {

struct BuildOptions {
  // Worker threads; 0 means std::thread::hardware_concurrency(). The result does not depend on it.
  unsigned threads = 0;
};

// Thread-safe for distinct models (writes model.statesByThread, reads the event streams). Runs per thread in
// parallel; deterministic for any thread count. Calling it again rebuilds the intervals.
Result<void> buildThreadStates(model::TraceModel& model, const BuildOptions& options = {});

// Thread-safe for concurrent const access. The interval of `thread` containing ts (start <= ts < end) by
// binary search, or nullptr when the thread index is invalid, states are not built, or ts is outside
// coverage.
const model::ThreadStateInterval* stateAt(const model::TraceModel& model, ThreadIdx thread, uint64_t ts);

}  // namespace tachyon::analysis
