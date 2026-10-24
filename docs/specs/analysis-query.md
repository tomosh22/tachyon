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
