<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.7. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.7 Analysis query API (`libs/analysis/query.h`)
All queries take a `Filter { uint64_t t0, t1; std::optional<std::vector<ThreadIdx>> threads; std::optional<CallstackIdx> subtreeRoot; std::optional<FunctionIdx> function; }` and return plain structs; they run on worker threads and are cancellable via a token.
```cpp
struct NodeAgg { FunctionKey key; uint64_t inclusiveTicks, exclusiveTicks; uint32_t sampleCount; std::vector<NodeAgg> children; };
struct FunctionRow { FunctionKey key; uint64_t inclusiveTicks, exclusiveTicks; uint32_t instances; };
struct Butterfly { FunctionRow self; std::vector<FunctionRow> callers, callees; };
struct LineAgg { uint32_t line; uint64_t ticks; uint64_t runningTicks, readyTicks, waitingTicks; };
struct InstructionAgg { uint64_t rva; uint64_t ticks; };
struct Stats { uint32_t count; double min, p25, median, p75, max, mean; std::vector<uint32_t> histogram; };
Result<NodeAgg>              topDownTree(const TraceModel&, const Filter&, Cancel);
Result<std::vector<FunctionRow>> bottomUpFunctions(const TraceModel&, const Filter&, Cancel);
Result<Butterfly>            butterfly(const TraceModel&, const Filter&, FunctionKey, Cancel);
Result<std::vector<LineAgg>> linesForFunction(const TraceModel&, const Filter&, FunctionKey, Cancel);
Result<std::vector<InstructionAgg>> instructionsForFunction(...);
Result<Stats>                functionStats(const TraceModel&, const Filter&, FunctionKey);   // per-instance durations
Result<Stats>                scopeStats(const TraceModel&, const Filter&, StringIdx scopeId);
Result<ThreadStateTotals>    threadStateTotals(const TraceModel&, const Filter&, ThreadIdx);
Result<std::vector<FrameSummary>> frames(const TraceModel&, const Filter&);
```
`FunctionKey` = `(ModuleIdx, functionStartRva)` for sampled functions or `(kScopeModule, StringIdx)` for instrumentation scopes, so both appear in the same trees.

## Orchestrator clarifications (binding; merged with WP-32, 2026-10-25)
`libs/analysis/query.h` is now orchestrator-owned. Changes go through the orchestrator.
- **Signature change (normative):** every 3.7 query takes `const QueryContext&` as its first parameter instead of `const TraceModel&`, and `Cancel cancel = {}` as a trailing defaulted parameter. `QueryContext(const TraceModel&, const symbols::SymbolService&, AggregationOptions = {})` binds the model and symbols and owns the per-filter memo. It needs `statesByThread` to be built. Create it after `SymbolService::waitAll()`, and rebuild it (or call `clearCache()`) whenever module resolution changes, because FunctionKeys change on resolve. WP-33, WP-34 and WP-35 add their queries to this header with the same convention.
- `Cancel { const std::atomic<bool>* flag; }`; the default is never cancelled. `Filter` ranges are half-open `[t0, t1)` on sample timestamps, and weights are not re-clipped at t1. `Filter::function` (since WP-33) is a `FunctionIdx` issued by `QueryContext::functionIdx(key)` from a per-context append-only `FunctionRegistry`. It keeps only samples whose stack, as the query sees it (after the subtree cut, with the context's inline/kernel options), contains that function. A handle the context never issued gives InvalidArgument. Results are cached in a per-context `QueryCache` (LRU, byte budget, default 256 MiB). `QueryExecutor` (caller-owned, Foreground/Background priorities, GenerationToken stale-result guard) runs queries; WP-34 and WP-35 reuse `QueryKey` and the reserved `QueryKind` values.
- Semantics:
  - `kRootKey = {invalid, 0}` is the top-down root and the caller of a stack-base function.
  - `kKernelKey = {invalid, ~0}` is the merged kernel frame for `KernelFrames::Group`.
  - `FunctionRow::instances` counts samples containing the function, with recursion counted once.
  - Butterfly callers and callees are taken relative to the outermost occurrence of the function, so caller inclusive, exclusive and instances each sum to self.
  - Unresolved but mapped frames give one row per `{module, rva}`, named `module!0x<rva>`.
- `NodeAgg` destruction is recursive. Consumers (WP-43 and WP-44) must traverse it iteratively. Stacks deeper than about 200k frames are unsupported.

### Return-address rule (binding; added after the WP-32 review)
Every frame of a sampled or blocking stack other than the leaf is a return address. Symbol lookups for function, inline chain and line attribution of such a frame use `rva - 1` (for an unmapped frame, `addr - 1`). This way a call that is the last instruction of a function is attributed to that function. The leaf frame uses its exact address. There is one exception. For a sample whose leaf is in kernel mode (`kSampleFlagKernelLeaf`), the first user-mode frame below the kernel frames is the interrupted instruction pointer, so it uses its exact address too. Instruction-level attribution (WP-34) keeps the exact return address and reports it as the call site's following instruction. Implemented by WP-32b: `SymbolService::lookupCaller(FrameIdx)` is memoized separately from `lookup`, and aggregation, LOD and the timeline choose between the two by position.
