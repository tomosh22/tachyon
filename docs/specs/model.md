<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.2. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.2 In-memory model (`libs/model/model.h`)
```cpp
struct Module { uint32_t pid; uint64_t base, size; std::string path; ModuleId id; uint64_t load_ts, unload_ts; SymbolState state; };
struct Thread { uint32_t tid, pid; std::string name; uint64_t first_ts, last_ts; bool is_target; ThreadRole role; };
struct Frame  { ModuleIdx module; uint64_t addr_or_rva; };               // symbolication attaches FunctionIdx lazily (symbols lib)
struct CallstackNode { CallstackIdx parent; FrameIdx frame; };            // nodes[0] = root
struct SampleColumns { std::vector<uint64_t> ts; std::vector<CallstackIdx> callstack; std::vector<uint16_t> cpu; std::vector<uint16_t> flags; }; // per thread, sorted by ts
struct ThreadStateInterval { uint64_t start, end; ThreadState state; uint8_t wait_reason; uint32_t cpu; CallstackIdx blocking_stack; CallstackIdx unblocking_stack; uint32_t woke_by_tid; uint64_t ready_ts; };
enum class ThreadState : uint8_t { Running, Ready, Waiting, Unknown };
struct ScopeInstance { uint64_t begin, end; StringIdx id; StringIdx data; uint32_t color; uint16_t depth; uint64_t caller_addr; };
struct TraceModel {
  FileHeader header; std::vector<Process>; std::vector<Thread>; std::vector<Module>; std::vector<Frame> frames; std::vector<CallstackNode> nodes;
  StringTable strings;
  std::unordered_map<uint32_t, ThreadIdx> tidToThread;
  std::vector<SampleColumns> samplesByThread;                 // index = ThreadIdx
  std::vector<std::vector<ThreadStateInterval>> statesByThread;
  std::vector<std::vector<ScopeInstance>> scopesByThread;     // sorted by begin, with depth
  std::vector<FiberSwitch> fiberSwitches; std::vector<FrameMark> frameMarks; std::vector<Marker> markers; std::vector<PlotSeries> plots;
  uint64_t sampleIntervalTicks;                               // from CaptureInfo sampling_hz
};
```
Memory budget: resident size ≤ 3× the compressed `.tachyon` size for a 10-minute 8 kHz capture. Callstack node = 8 bytes. Sample = 16 bytes in memory (per-thread `SampleColumns`; tid implicit) and 20 bytes on disk (3.1 SoA columns). [Corrected 2026-10-25 by orchestrator; was "18 bytes", which matched neither layout.]

## Orchestrator clarifications (binding; merged with WP-30, 2026-10-25)
Header: `libs/model/model.h` (`tachyon::model`), now orchestrator-owned.
- Additional types: `ModuleId` (layout from 3.6; lives here because libs/symbols depends on model), `SymbolState {Pending, Loading, Resolved, NotFound, Failed}`, `ThreadRole {Unknown, Main, Render, Worker, Other}` (name heuristic), `Process`, `FiberSwitch`, `FrameMark`, `Marker`, `PlotSeries` (SoA), `CaptureInfo`, `LoadOptions {threads, const std::atomic<bool>* cancel}`.
- Additional TraceModel members: `isPartial`, `captureInfo`, raw `contextSwitches`, `readyEvents`, `scopeEvents`, `clockCalibrations` (as loaded, sorted by ts), `residentBytes()`.
- `statesByThread` and `scopesByThread` are sized to the thread count and filled by WP-31 and WP-53.
- `TraceModel::load(path, LoadOptions, progress)`. Progress is invoked only on the calling thread. Loading is deterministic for any worker count.
- tid 0 on events means "no thread" (invalid ThreadIdx). The latest ThreadNames record by ts sets `Thread::name`. Duplicate ThreadRecs are kept, and `tidToThread` maps to the first.
- Memory: 16 B/sample for sample columns; the "≤ 3× compressed" budget is under user review (synthetic fixtures compress 8:1 and measure 6.95–7.33×).

## Clarifications added with WP-64 (2026-11-03, binding)

- Streaming load: `LoadOptions::prefixFraction` (default 1.0) loads only that fraction of the sample chunks as a consistent time prefix: tables complete, `header.end_ts` clamped to the loaded samples, events past it dropped, `loadStatus.complete == false`. Thread indices of threads with a ThreadRec equal the full load's (threads synthesised from samples alone may differ; real traces have ThreadRecs). `TraceModel::loadStatus` = {complete, generation, sampleChunksLoaded/Total, fullEndTs, samplesLoaded}.
- `TraceModel::loadProgressive(path, options, stages, onSnapshot, progress)` publishes a preview per stage, then the complete model; cancellation returns Cancelled.
- Viewer: files of 64 MiB or more open as a 5% preview, then the complete document replaces it, keeping the timeline view, row flags and scroll. Closing the preview abandons the load; a newer open supersedes it.
- Memory ceiling (viewer): `Settings::memoryCeilingPercent` (default 75 % of physical memory, 0 = none; View > Memory limit). The timeline index is budgeted up front (ceiling minus the model); over the ceiling the governor degrades, in order, the LOD fine level, the finest LOD levels, the analysis cache budget (toast and banner at each stage), then shows "model alone exceeds the ceiling". Nothing is restored automatically.
- `tachyon-viewer --bench <trace> [--bench-frames N]` runs a headless steady/pan/zoom frame-time benchmark (dogfooding entry point; capture it with `tachyon-agent --backend perf`).
