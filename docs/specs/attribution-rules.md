<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.8. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.8 Attribution rules (normative)
1. Sample weight = `sampleIntervalTicks`, clipped so that no sample's weight extends beyond the end of the `Running` interval it falls in. A sample with no enclosing `Running` interval (thread not running per context-switch data) gets weight 0 and is excluded from time totals but kept for stack display.
2. Inclusive time of a function in a stack counts once per sample even if the function appears several times (recursion).
3. Exclusive time attributes to the leaf frame; with inline frames expanded, the innermost inline frame is the leaf.
4. Context switch out at time `t` with `old_thread_stack` S: the interval `[t, next switch-in)` is `Waiting` (or `Ready` if `old_state` says ready/standby) with `blocking_stack = S`. A `ReadyRec` for the thread at `tr` splits the interval into `Waiting [t, tr)` and `Ready [tr, switch-in)` and sets `unblocking_stack` and `woke_by_tid`.
5. Scope blending: for a sample at time `ts` on thread T, the open scopes are those with `begin ≤ ts < end`, ordered by depth. For each open scope (outermost first), find the deepest frame in the sample's stack whose frame address range contains `caller_addr`; insert a synthetic frame with `FunctionKey(kScopeModule, id)` directly above that frame. If none matches, insert directly above the root. Nested scopes maintain relative order.
6. Frame statistics: a frame is `[FrameMark_i, FrameMark_{i+1})` on the marking thread; per-frame totals are computed per thread from Running intervals and from exclusive sample time.

## Orchestrator clarifications (binding; from the WP-04 review, 2026-10-25)
- Unknown before first event: a thread's state is `Unknown` from trace start (`header.start_ts`, regardless of ThreadRec start_ts; corrected with WP-31) until its first context-switch event. A thread whose first event is a switch-out is NOT inferred to have been Running before it; that prefix is `Unknown`, and samples in it get weight 0 under rule 1. (Matches WP-31.)
- Rule 4 metadata: when a `ReadyRec` splits a wait, both pieces (`Waiting [t,tr)` and `Ready [tr,switch-in)`) carry the same `blocking_stack`, `unblocking_stack`, `woke_by_tid` and `ready_ts = tr`. A switch-out whose `old_state` is Ready/Standby (preempted) produces a single `Ready` interval with no unblocking information unless a `ReadyRec` arrives.
- Rule 5 placement is monotonic: process open scopes outermost first; slot(S_i) = max(match(S_i), slot(S_{i-1})), where match is the depth of the deepest stack frame whose function address range (the whole function from symbols) contains `caller_addr`, or the root if none. An unmapped frame (module = kInvalidIdx) matches only its exact address. This keeps nested scopes in order even when an inner scope's caller matches nothing.

## Clarifications added with WP-31 (2026-10-25)
- Implementation: `libs/analysis/thread_states.h` (`buildThreadStates`, `stateAt`; half-open intervals, contiguous over [trace start, trace end or thread exit)).
- Event order at equal timestamps between streams: switch-out, then ReadyRec, then switch-in. Within the switch stream, ties keep file order. A known edge case: a same-timestamp cross-CPU migration recorded as in-before-out degrades to Unknown.
- Lost-event gaps: a double switch-in or double switch-out makes the span from the last known state change to the offending event `Unknown` (merged with adjacent Unknown). Pending ReadyRec info is dropped.
- ReadyRec while Running/Unknown is ignored. A second ReadyRec in one wait is ignored (the first wins). A ReadyRec on a preempted (Ready) interval attaches unblocking info without splitting.
- Thread exit: a switch-out with old_state 4 (Terminated), or the ThreadRec end_ts when it is later than the thread's last event.
- Wait reasons (`libs/analysis/wait_reasons.h`): `old_state` uses KTHREAD_STATE numbering on both platforms (1/3/7 ready/preempted, 4 exited, else waiting). `old_wait_reason` is the KWAIT_REASON on Windows and `prev_state & 0xFF` on Linux. WP-71 must write sched_switch records in this encoding (prev_state TASK_RUNNING → old_state 1, exiting → 4, else 5).

## Clarifications added with WP-53 (2026-10-25, binding)
- **Rule 5 "above the root"** means directly above the first (outermost) stack frame: slot 0, after frame index 0, with the monotonic max. This matches tracegen's reference. A stack with no frames holds only the scopes.
- **caller_addr** is a return address captured inside the user's function. A mapped frame matches it when the frame's whole-function range satisfies `lo < caller_addr <= hi` (WP-53b). An unmapped frame matches only its exact address.
- **Inline expansion:** a scope is placed between the matched frame's function key and its inline keys. This is a known limitation; placing it above the innermost inline frame containing caller_addr is deferred.
- **Subtree cut:** scopes whose slot is at or above the cut depth are dropped.
- Blending is `AggregationOptions::blendScopes` (default off) and fiber grouping is `groupByFiber`. Both apply in the shared flattener to every query, and the options are part of every cache key.
- **Fibers:**
  - A fiber starts running at switch_end. Register only creates its lane.
  - Unregister ends the fiber's open scopes, which are flagged unterminated.
  - At an equal ts, End events at the head of the tie group go first, then fiber events, then the other scope events.
  - With no fiber running, scope events go to the thread's own lane.
  - Pieces exclude switched-out time.
  - scopeStats and lockSummary report wall time (end − begin).
- **Locks:** a lock wait is a scope with colour 0x4C4F43FF. Its address is parsed from the scope data as hex; unparsable data is counted in `unparsedWaits`. The name is the scope id unless the id is the reserved "Tachyon.LockAcquire". contentionCount counts every acquire, including zero-length ones.
