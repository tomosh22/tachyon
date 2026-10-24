<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.8. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.8 Attribution rules (normative)
1. Sample weight = `sampleIntervalTicks`, clipped so that no sample's weight extends beyond the end of the `Running` interval it falls in. A sample with no enclosing `Running` interval (thread not running per context-switch data) gets weight 0 and is excluded from time totals but kept for stack display.
2. Inclusive time of a function in a stack counts once per sample even if the function appears several times (recursion).
3. Exclusive time attributes to the leaf frame; with inline frames expanded, the innermost inline frame is the leaf.
4. Context switch out at time `t` with `old_thread_stack` S: the interval `[t, next switch-in)` is `Waiting` (or `Ready` if `old_state` says ready/standby) with `blocking_stack = S`. A `ReadyRec` for the thread at `tr` splits the interval into `Waiting [t, tr)` and `Ready [tr, switch-in)` and sets `unblocking_stack` and `woke_by_tid`.
5. Scope blending: for a sample at time `ts` on thread T, the open scopes are those with `begin ≤ ts < end`, ordered by depth. For each open scope (outermost first), find the deepest frame in the sample's stack whose frame address range contains `caller_addr`; insert a synthetic frame with `FunctionKey(kScopeModule, id)` directly above that frame. If none matches, insert directly above the root. Nested scopes maintain relative order.
6. Frame statistics: a frame is `[FrameMark_i, FrameMark_{i+1})` on the marking thread; per-frame totals are computed per thread from Running intervals and from exclusive sample time.
