<!-- Orchestrator-owned. Part 3.7, kept in sync with libs/analysis/query.h, instances.h, scope_pairing.h and fibers.h. Changes only via orchestrator. -->

### 3.7 Analysis query API (`libs/analysis/query.h`, namespace `tachyon::analysis`)
All queries take a `const QueryContext&` and a `Filter`, return plain structs (or `shared_ptr<const T>` for the `*Shared` variants), run on worker threads and are cancellable via a trailing `Cancel cancel = {}` token.
```cpp
struct Filter { uint64_t t0 = 0, t1 = UINT64_MAX;                       // half-open [t0, t1) on sample timestamps
                std::optional<std::vector<ThreadIdx>> threads;          // nullopt = all
                std::optional<CallstackIdx> subtreeRoot;
                std::optional<std::vector<FunctionKey>> subtreePath;    // WP-33b
                std::optional<FunctionIdx> function; };                 // from QueryContext::functionIdx(key)
struct NodeAgg { FunctionKey key; uint64_t inclusiveTicks, exclusiveTicks; uint32_t sampleCount; std::vector<NodeAgg> children; };
struct FunctionRow { FunctionKey key; uint64_t inclusiveTicks, exclusiveTicks; uint32_t instances; };
struct Butterfly { FunctionRow self; std::vector<FunctionRow> callers, callees; };
struct LineAgg { uint32_t line; uint64_t ticks; uint64_t runningTicks, readyTicks, waitingTicks; };
struct InstructionAgg { uint64_t rva; uint64_t ticks; };
struct Stats { uint32_t count; double min, p25, median, p75, max, mean; std::vector<uint32_t> histogram;
               uint64_t totalTicks; uint32_t unterminated, orphanEnds; };
struct ThreadStateTotals { uint64_t runningTicks, readyTicks, waitingTicks, unknownTicks; uint32_t runningIntervals, readyIntervals, waitingIntervals; };
struct AggregationOptions { bool expandInline = false; KernelFrames kernelFrames = KernelFrames::Show; unsigned threads = 0;
                            bool blendScopes = false; bool groupByFiber = false; };            // KernelFrames { Show, Group, Hide }
class QueryContext { public: QueryContext(const TraceModel&, const symbols::SymbolService&, AggregationOptions = {}, QueryCacheConfig = {}); /* clearCache, cacheStats, setCacheBudget, functionIdx, setExecutor ... */ };
Result<NodeAgg>                   topDownTree(const QueryContext&, const Filter&, Cancel = {});
Result<std::vector<FunctionRow>>  bottomUpFunctions(const QueryContext&, const Filter&, Cancel = {});
Result<Butterfly>                 butterfly(const QueryContext&, const Filter&, FunctionKey, Cancel = {});
Result<std::vector<LineAgg>>      linesForFunction(const QueryContext&, const Filter&, FunctionKey, Cancel = {});
Result<std::vector<InstructionAgg>> instructionsForFunction(const QueryContext&, const Filter&, FunctionKey, Cancel = {});
Result<Stats>                     functionStats(const QueryContext&, const Filter&, FunctionKey, Cancel = {});      // per-instance durations (approximated from samples)
Result<Stats>                     scopeStats(const QueryContext&, const Filter&, StringIdx scopeId, Cancel = {});   // exact, from paired Begin/End
Result<ThreadStateTotals>         threadStateTotals(const QueryContext&, const Filter&, ThreadIdx, Cancel = {});
std::vector<FrameSeries>          frameSeries(const QueryContext&);
Result<std::vector<FrameSummary>> frames(const QueryContext&, const Filter&, FrameOptions = {}, Cancel = {});
Result<std::vector<FrameSummary>> slowestFrames(const QueryContext&, const Filter&, uint32_t n, FrameOptions = {}, Cancel = {});
Result<LockSummary>               lockSummary(const QueryContext&, const Filter&, Cancel = {});                     // WP-53
// instances.h (WP-47):  Instance { ThreadIdx thread; uint64_t begin, end; }
Result<std::shared_ptr<const std::vector<Instance>>> functionInstances(const QueryContext&, const Filter&, FunctionKey, Cancel = {});
Result<std::shared_ptr<const std::vector<Instance>>> scopeInstances(const QueryContext&, const Filter&, StringIdx scopeId, Cancel = {});
std::string functionKeyName(const QueryContext&, FunctionKey);
```
Every by-value query has a `*Shared` variant returning `Result<std::shared_ptr<const T>>`: the cache's own immutable copy (a hit costs a lookup and a reference count; it survives eviction and `clearCache()`). `FunctionKey` = `(ModuleIdx, functionStartRva)` for sampled functions or `(kScopeModule, StringIdx)` for instrumentation scopes, so both appear in the same trees (symbol-provider.md).

## Context, filter and cache
- **QueryContext** binds the model and `symbols::SymbolService` and owns the per-filter memo. It needs `statesByThread` to be built. Create it after `SymbolService::waitAll()`, and rebuild it (or call `clearCache()`) whenever module resolution changes, because FunctionKeys change on resolve. Model, symbols and module resolution must stay unchanged and alive while the context is used. Queries may run concurrently on one context.
- `Cancel { const std::atomic<bool>* flag; }`; the default is never cancelled.
- `Filter` ranges are half-open `[t0, t1)` on sample timestamps, and weights are not re-clipped at t1. `Filter::subtreeRoot` 0 means no subtree. `Filter::function` is a `FunctionIdx` issued by `QueryContext::functionIdx(key)` from a per-context append-only `FunctionRegistry`; it keeps only samples whose stack, as the query sees it (after the subtree cut, with the context's inline/kernel options), contains that function. A handle the context never issued gives `InvalidArgument`; a handle that happens to be issued by two contexts is not detected as foreign.
- Results are cached in a per-context `QueryCache` (LRU within a byte budget, default 256 MiB, `QueryCacheConfig::budgetBytes`; keyed by query, extra arguments, filter and options). `QueryExecutor` (caller-owned via `setExecutor`, Foreground/Background priorities, `GenerationToken` stale-result guard) runs queries and lets a query's scan fan out through it; queries reuse `QueryKey` and the reserved `QueryKind` values.
- `AggregationOptions::blendScopes` (default off) splices instrumentation scopes into sampled stacks (attribution-rules.md rule 5); `groupByFiber` attributes and pairs scopes per fiber. Both apply in the shared flattener to every query, and both are part of every cache key. Without them results are identical to a build without scope support.

## Semantics
- `kRootKey = {invalid, 0}` is the top-down root and the caller of a stack-base function. `kKernelKey = {invalid, ~0}` is the merged kernel frame for `KernelFrames::Group`. Under `Hide`, a stack left with no frame at all (empty, or only kernel frames) is attributed to the single frame `kKernelKey` ("<kernel>") in every view, so `bottomUpFunctions` gets a `<kernel>` row (inclusive = exclusive) and `topDownTree` a `<kernel>` child of the root.
- `FunctionRow::instances` counts samples containing the function, with recursion counted once. `NodeAgg::sampleCount` includes zero-weight samples.
- Butterfly callers and callees are taken relative to the outermost occurrence of the function, so caller inclusive, exclusive and instances each sum to self.
- Unresolved but mapped frames give one row per `{module, rva}`, named `module!0x<rva>` (`functionKeyName`: `module!function`, `<unmapped>!0x<addr>`, `<kernel>`, `<root>`, `scope:<id>`).
- `bottomUpFunctions`: kept samples with an empty stack form one row keyed `kRootKey` (inclusive = exclusive); its total equals `topDownTree`'s root under KernelFrames Show and Group. `butterfly(kRootKey)` returns that row as self with no callers or callees. The viewer names the row "<no stack>".
- `NodeAgg` destruction is iterative (a 300,000-deep tree is safe to destroy and move); copying is still recursive and must not be used on deep trees, and consumers must traverse iteratively. Stacks deeper than about 200k frames are unsupported.
- **Return-address rule (binding; WP-32b).** Every frame of a sampled or blocking stack other than the leaf is a return address. Symbol lookups for function, inline chain and line attribution of such a frame use `rva − 1` (for an unmapped frame, `addr − 1`), so a call that is the last instruction of a function is attributed to that function. The leaf frame uses its exact address. Exception: for a sample whose leaf is in kernel mode (`kSampleFlagKernelLeaf`), the first user-mode frame below the kernel frames is the interrupted instruction pointer, so it uses its exact address too. Instruction-level attribution (`instructionsForFunction`) keeps the exact return address and reports it as the call site's following instruction. Implemented by `SymbolService::lookupCaller(FrameIdx)` (memoized separately from `lookup`) and the `frameKeyAt`/`usesExactAddress` helpers in `libs/analysis/attribution.h`; aggregation, LOD and the timeline choose between the two by position.

## Subtree filters (WP-33b)
- `Filter::subtreePath`: keys from the top-down root's child down to the selected node (no `kRootKey`). A sample is kept only if its flattened root-first key sequence (the one `topDownTree` merges: return-address rule, inline expansion, kernel Show/Group/Hide with the Hide fallback `<kernel>` key counted as a frame, scope blending, then the `subtreeRoot` cut if set) starts with exactly the path; the sequence then starts at the path's last key (earlier keys are dropped, mirroring the `subtreeRoot` cut). A function reached through several call sites is kept whole, which a single `subtreeRoot` node cannot do. An empty path is `InvalidArgument` for every query that resolves a Filter. When both are set both apply (`subtreeRoot` first). Part of Filter equality and of the cache key. Matching is memoized per distinct weight entry (node, kernel-leaf flag, scope stack), never per sample.
- Per query under a path: `topDownTree`'s root is the path's last function with exactly the inclusive ticks and subtree it had unfiltered (no match: plain root with 0); `bottomUpFunctions`, `butterfly`, `functionStats`, `functionInstances` and `frames` (topFunctions, sampleTicks) work on the cut stacks; `linesForFunction`/`instructionsForFunction` only count frames at or below the physical frame holding the path's last key (Ready/Waiting time tests the blocking stack the same way); `scopeStats`, `threadStateTotals`, `lockSummary` validate the path and otherwise ignore it, like `subtreeRoot`. Exporters reject it as `Unsupported`. Subtree paths are only meaningful under the frame options they were taken with (the viewer carries them in `ViewFilter::subtreeOptions`).

## Statistics and frames (WP-35)
- `Stats`: count, min, p25, median, p75, max, mean, `totalTicks` (wraps modulo 2^64), `unterminated`, `orphanEnds`, histogram. Quartiles are lower order statistics `d[floor(q·(n−1))]` (the median of an even count is the lower middle value; matches tracegen's `median_ticks`); mean = totalTicks / count; all zero when count is 0. Histogram bucket 0 = {0}; bucket b ≥ 1 = [2^(b−1), 2^b), up to bucket 64; see `statsBucketOf/Lower/Upper`. The vector runs from bucket 0 to the highest non-empty bucket.
- Sampled-function instance (`functionStats`, `functionInstances`): a maximal per-thread run of weighted samples containing the function at the same (outermost-occurrence) depth, with gaps ≤ 2 sample intervals. Duration = last − first + the last sample's weight. Zero-weight samples are skipped before runs form. This is an approximation. A key with module `kScopeModule` is answered as `scopeStats`.
- Scope instances (`scopeStats`, `scopeInstances`): exact per-thread stack pairing (`libs/analysis/scope_pairing.h`; fiber-aware pairing in `fibers.h`). An unterminated Begin ends at `header.end_ts` and is counted. An orphan End is counted and ignored. A scope is selected by its Begin in [t0, t1) and by its thread; `subtreeRoot` and `function` are ignored for scopes. `functionInstances` and `scopeInstances` return `Instance{thread, begin, end}` sorted by (begin, thread, end), using exactly the rules of the stats (one shared enumeration), cached under `QueryKind::Instances`, honouring `blendScopes` and `groupByFiber`.
- `threadStateTotals` clips intervals to the range (and the range to the thread's coverage, so `t1 = UINT64_MAX` is fine); `Filter::threads`, `subtreeRoot` and `function` are ignored.
- Frames (attribution rule 6): per (thread, name) series (`frameSeries`, sorted by mark count descending); the default series is the one with the most marks (ties: earliest first mark, then lowest thread, then lowest name). n marks give n−1 frames. Frames overlapping the range are reported whole, not clipped. Per-thread Running comes from thread states and is not affected by `subtreeRoot`/`function`; `topFunctions` are by exclusive time, using return-address-rule keys. A trace without frame marks gives an empty list; an explicit series with no marks gives `InvalidArgument`.

## Lines and instructions (WP-34)
- `linesForFunction(ctx, filter, key)` covers the function's own lines, sorted by line, only lines with ticks > 0. `runningTicks` is the rule-1 weight of samples whose frame in this function resolves to the line (non-leaf frames at rva − 1; recursion counted once, at the outermost occurrence). `readyTicks` and `waitingTicks` are Ready and Waiting interval durations whose blocking stack contains the function, attributed to the blocking frame's line and clipped to the filter. `ticks` = running + ready + waiting. Lines inside inlined callees go to the call-site line in the function's own file (`ResolvedAddress::callSiteLine`; an approximation when the provider has none). Line 0 = no line information. Scope keys, `kRootKey` and `kKernelKey` match nothing.
- `instructionsForFunction` counts running samples only, sorted by rva. The leaf frame uses its exact address. A non-leaf frame uses the return address itself, which the disassembly view shows on the instruction after the call.
- `libs/disasm`: `disassemble(bytes, baseRva, bitness)` (Zydis, Intel syntax, call targets symbolized through a callback). `readImageBytes`/`readModuleBytes` map PE sections and ELF PT_LOAD per the ELF base rule.

## Locks (WP-53)
`lockSummary` summarizes lock-wait scopes (colour sentinel `0x4C4F43FF`): `LockRow { address, name, contentionCount, totalWaitTicks, maxWaitTicks, unterminated, topThreads (top kMaxLockWaiters = 5 by waitTicks) }`, `LockSummary { locks sorted by totalWaitTicks descending then address, unparsedWaits }`. Details in attribution-rules.md.

## Changelog
- 2026-10-25 (WP-32, WP-32b, WP-33, WP-35): `QueryContext` signature change, `Cancel`, `FunctionRegistry`, cache and executor, semantics, return-address rule, statistics and frames.
- 2026-11-02 (WP-34, WP-47): lines, instructions, disassembly, instances.
- 2026-11-03 (WP-33b): `subtreePath`, frameless consistency of bottomUp/topDown.
- 2026-11-15: clarifications folded into the sections above; main listing brought up to date (`QueryCacheConfig`, `lockSummary`, instances, `AggregationOptions`, `*Shared` variants).
