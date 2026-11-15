<!-- Orchestrator-owned. Part 3.8 (normative), kept in sync with libs/analysis/thread_states.h, wait_reasons.h, scope_blend.h, scope_pairing.h, fibers.h and locks.h. Changes only via orchestrator. -->

### 3.8 Attribution rules (normative)
1. Sample weight = `sampleIntervalTicks`, clipped so that no sample's weight extends beyond the end of the `Running` interval it falls in. A sample with no enclosing `Running` interval (thread not running per context-switch data) gets weight 0 and is excluded from time totals but kept for stack display and sample counts.
2. Inclusive time of a function in a stack counts once per sample even if the function appears several times (recursion).
3. Exclusive time attributes to the leaf frame; with inline frames expanded, the innermost inline frame is the leaf.
4. Context switch out at time `t` with `old_thread_stack` S: the interval `[t, next switch-in)` is `Waiting` (or `Ready` if `old_state` says ready/standby/deferred-ready) with `blocking_stack = S`. A `ReadyRec` for the thread at `tr` splits the interval into `Waiting [t, tr)` and `Ready [tr, switch-in)` and sets `unblocking_stack` and `woke_by_tid`.
5. Scope blending: for a sample at time `ts` on thread T, the open scopes are those with `begin ≤ ts < end`, ordered by depth. For each open scope (outermost first), find the deepest frame in the sample's stack whose frame address range contains `caller_addr`; insert a synthetic frame with `FunctionKey(kScopeModule, id)` directly above that frame. If none matches, insert directly above the root. Nested scopes maintain relative order.
6. Frame statistics: a frame is `[FrameMark_i, FrameMark_{i+1})` on the marking thread; per-frame totals are computed per thread from Running intervals and from exclusive sample time.

## Thread states (rule 4; `libs/analysis/thread_states.h`, WP-31)
- `buildThreadStates` / `stateAt`: half-open intervals, contiguous over [trace start, trace end or thread exit).
- **Unknown before the first event:** a thread's state is `Unknown` from trace start (`header.start_ts`, regardless of ThreadRec `start_ts`) until its first context-switch event. A thread whose first event is a switch-out is NOT inferred to have been Running before it; that prefix is `Unknown`, and samples in it get weight 0 under rule 1.
- **Rule 4 metadata:** when a `ReadyRec` splits a wait, both pieces (`Waiting [t,tr)` and `Ready [tr,switch-in)`) carry the same `blocking_stack`, `unblocking_stack`, `woke_by_tid` and `ready_ts = tr`. A switch-out whose `old_state` is Ready/Standby (preempted) produces a single `Ready` interval with no unblocking information unless a `ReadyRec` arrives (then it attaches without splitting); a preempted interval without one has `ready_ts` = its start. A Waiting interval with no ReadyRec has `ready_ts` = 0.
- **Event order at equal timestamps** between streams: switch-out, then ReadyRec, then switch-in. Within the switch stream, ties keep file order. A known edge case: a same-timestamp cross-CPU migration recorded as in-before-out degrades to Unknown.
- **Lost-event gaps:** a double switch-in or double switch-out makes the span from the last known state change to the offending event `Unknown` (merged with adjacent Unknown). Pending ReadyRec info is dropped.
- A ReadyRec while Running/Unknown is ignored. A second ReadyRec in one wait is ignored (the first wins). A ReadyRec at or after the thread's last switch-out with no later switch-in leaves the trace ending in Ready.
- **Thread exit:** a switch-out with `old_state` 4 (Terminated), or the ThreadRec `end_ts` when it is later than the thread's last event.

## Wait reasons and `old_state` encoding (`libs/analysis/wait_reasons.h`)
- `ContextSwitchRec::old_state` uses the KTHREAD_STATE numbering on both platforms: Initialized 0, Ready 1, Running 2, Standby 3, Terminated 4, Waiting 5, Transition 6, DeferredReady 7. States 1, 3 and 7 mean the thread left the CPU still runnable (preempted): the off-CPU interval is Ready. State 4 is the thread's exit. Anything else is a wait.
- `ThreadStateInterval::wait_reason` is a copy of `old_wait_reason`, interpreted by `FileHeader::platform` (`waitReasonName`, `waitReasonCategory`; `WaitCategory` = Synchronization, IO, Memory, Preempted, Other, which feeds the timeline band colours):
  - **Windows:** a KWAIT_REASON 0..37 (category mapping in the header: e.g. page-in/pool wait reasons are Memory, `WrLpc*` are IO, quantum-end/dispatch/preempted/yield are Preempted, executive/user-request/queue/mutex/push-lock reasons are Synchronization, the rest Other).
  - **Linux:** `prev_state & 0xFF` of the sched_switch record. Bits: 0x01 S (interruptible), 0x02 D (uninterruptible), 0x04 T (stopped), 0x08 t (traced), 0x10 EXIT_DEAD, 0x20 EXIT_ZOMBIE, 0x40 TASK_PARKED, 0x80 TASK_IDLE (a wait). Categories: D → IO, S → Synchronization, everything else Other.
  - **Linux reason 0 on a wait means "unknown"** (commit 1ecc212, 2026-11-15; previously shown as Preempted). A thread preempted while TASK_RUNNING is encoded as `old_state` Ready, never as a wait with reason 0, so a Waiting interval with reason 0 only occurs in PERF_RECORD_SWITCH captures without the sched tracepoint (the fallback below). It is named "Unknown (no sched tracepoint)", categorised Other (grey) and drawn as a wait, not as Ready; traces converted earlier display correctly without re-conversion.
- Linux sched_switch encoding (perf backend): `prev_state` TASK_RUNNING (low byte 0, including the TASK_REPORT_MAX flag 0x100 and older kernels' TASK_STATE_MAX) → `old_state` 1; EXIT_DEAD / EXIT_ZOMBIE (0x10 / 0x20) → 4; otherwise 5. Only those two exit bits map to 4.
- Linux `context_switch` fallback (per-thread plans, or no tracefs): `old_state` 1 if the record says preempted, otherwise 5; wait reason 0 (unknown, see above); no blocking stacks and no ReadyRecs. CaptureInfo `sched.path` records which path was used, and a warning explains what is missing.
- `old_tid`/`new_tid`/`readying_tid` 0 means "other task unknown/unnamed". The kernel's tid −1 (an exiting task that is no longer pid_alive) is attributed from the tracepoint payload, or to the CPU's last running task in the fallback, or dropped (counted in `convert_stats.sched_unattributed_switches`, see capture-backend.md). It never creates a thread. Events a thread emits after PERF_RECORD_EXIT attach to the exited thread's record.

## Scope blending (rule 5; `libs/analysis/scope_blend.h`, WP-53)
- Placement is monotonic: process open scopes outermost first; `slot(S_i) = max(match(S_i), slot(S_{i-1}))`, where `match` is the depth of the deepest stack frame whose function address range (the whole function from symbols) contains `caller_addr`, or the root if none. This keeps nested scopes in order even when an inner scope's caller matches nothing.
- "Above the root" means directly above the first (outermost) stack frame: slot 0, after frame index 0, with the monotonic max. This matches tracegen's reference. A stack with no frames holds only the scopes.
- `caller_addr` is a return address captured inside the user's function. A mapped frame matches it when the frame's whole-function range satisfies `lo < caller_addr <= hi` (WP-53b). An unmapped frame (module = kInvalidIdx) matches only its exact address.
- Inline expansion: a scope is placed between the matched frame's function key and its inline keys. This is a known limitation; placing it above the innermost inline frame containing `caller_addr` is deferred.
- Subtree cut: scopes whose slot is at or above the cut depth are dropped.
- Blending is `AggregationOptions::blendScopes` (default off) and fiber grouping is `groupByFiber`. Both apply in the shared flattener to every query, and the options are part of every cache key.

## Fibers and locks (WP-53)
- A fiber starts running at `switch_end`. Register only creates its lane. Unregister ends the fiber's open scopes, which are flagged unterminated. At an equal ts, End events at the head of the tie group go first, then fiber events, then the other scope events. With no fiber running, scope events go to the thread's own lane. Pieces exclude switched-out time. `scopeStats` and `lockSummary` report wall time (end − begin).
- A lock wait is a scope with colour `0x4C4F43FF` (`TACHYON_LOCK_COLOR_SENTINEL`). Its address is parsed from the scope data as hex; unparsable data is counted in `unparsedWaits`. The name is the scope id unless the id is the reserved `"Tachyon.LockAcquire"`. `contentionCount` counts every acquire, including zero-length ones.

## Changelog
- 2026-10-25 (WP-04 review, WP-31, WP-53): Unknown-before-first-event rule, rule-4 metadata, rule-5 monotonic placement, thread-state edge cases, fiber and lock rules.
- 2026-11-03 (WP-71): Linux exit encoding and the `context_switch` fallback.
- 2026-11-15 (commit 1ecc212, WP-71b): Linux wait reason 0 means unknown (Other), not Preempted; unattributed exiting-task switches.
- 2026-11-15: clarifications folded into the sections above.
