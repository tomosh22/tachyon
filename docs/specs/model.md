<!-- Orchestrator-owned. Part 3.2, kept in sync with libs/model/model.h. Changes only via orchestrator. -->

### 3.2 In-memory model (`libs/model/model.h`, namespace `tachyon::model`)
```cpp
struct Process { uint32_t pid, parent_pid; uint64_t start_ts, end_ts; std::string name, command_line; };
struct Module { uint32_t pid; uint64_t base, size; std::string path; ModuleId id; uint64_t load_ts, unload_ts; SymbolState state; };
struct Thread { uint32_t tid, pid; std::string name; uint64_t first_ts, last_ts; bool is_target; ThreadRole role; };
struct Frame  { ModuleIdx module; uint64_t addr_or_rva; };               // module == kInvalidIdx: absolute address; symbolication is lazy (symbols lib)
struct CallstackNode { CallstackIdx parent; FrameIdx frame; };            // nodes[0] = root; sizeof == 8
struct SampleColumns { std::vector<uint64_t> ts; std::vector<CallstackIdx> callstack; std::vector<uint16_t> cpu; std::vector<uint16_t> flags; }; // per thread, sorted by ts
struct ThreadStateInterval { uint64_t start, end; ThreadState state; uint8_t wait_reason; uint32_t cpu; CallstackIdx blocking_stack; CallstackIdx unblocking_stack; uint32_t woke_by_tid; uint64_t ready_ts; };
enum class ThreadState : uint8_t { Running, Ready, Waiting, Unknown };
struct ScopeInstance { uint64_t begin, end; StringIdx id; StringIdx data; uint32_t color; uint16_t depth; uint64_t caller_addr; };
struct TraceModel {
  trace::FileHeader header; bool isPartial; LoadStatus loadStatus; CaptureInfo captureInfo;
  std::vector<Process> processes; std::vector<Thread> threads; std::vector<Module> modules; std::vector<Frame> frames; std::vector<CallstackNode> nodes;
  trace::StringTable strings;
  std::unordered_map<uint32_t, ThreadIdx> tidToThread;
  std::vector<SampleColumns> samplesByThread;                 // index = ThreadIdx
  std::vector<std::vector<ThreadStateInterval>> statesByThread;  // sized to threads, EMPTY after load; filled by analysis::buildThreadStates
  std::vector<std::vector<ScopeInstance>> scopesByThread;     // sized to threads, EMPTY after load; filled by the scope pairing (analysis/scope_pairing.h)
  std::vector<FiberSwitch> fiberSwitches; std::vector<FrameMark> frameMarks; std::vector<Marker> markers; std::vector<PlotSeries> plots;
  uint64_t sampleIntervalTicks;                               // from CaptureInfo sampling_hz
  std::vector<trace::ContextSwitchRec> contextSwitches; std::vector<trace::ReadyRec> readyEvents; std::vector<trace::ScopeEventRec> scopeEvents;
  std::vector<trace::ClockCalibrationRec> clockCalibrations;  // raw event streams as loaded, sorted by ts (stable)
  static Result<TraceModel> load(const std::filesystem::path&, const LoadOptions& = {}, std::function<void(float)> progress = {});
  static Result<TraceModel> loadProgressive(path, options, const std::vector<double>& stages, onSnapshot, progress = {});
  size_t residentBytes() const;
};
```
Memory budget: resident size ≤ 3× the compressed `.tachyon` size for a 10-minute 8 kHz capture (under user review: synthetic fixtures compress 8:1 and measure 6.95–7.33×). Callstack node = 8 bytes. Sample = 16 bytes in memory (per-thread `SampleColumns`; tid implicit) and 20 bytes on disk (trace-format.md SoA columns). [Corrected 2026-10-25 by orchestrator; was "18 bytes", which matched neither layout.]

Additional types: `ModuleId { kind, bytes[32], len, age }` (layout from symbol-provider.md; it lives here because `libs/symbols` depends on `libs/model`), `SymbolState {Pending, Loading, Resolved, NotFound, Failed}`, `FiberSwitch`, `FrameMark`, `Marker`, `PlotSeries` (SoA), `CaptureInfo { sampling_hz, events_lost, raw_json }` (the parsed fields of the CaptureInfo chunk; `raw_json` is empty when the trace has none), `LoadOptions`, `LoadStatus`.

`ThreadRole {Unknown, Main, Render, Worker, Other}` is a name heuristic (`classifyThreadRole`): case-insensitive substring, checked in this order, "main" → Main, "render" → Render, "worker" or "job" → Worker; any other non-empty name → Other; an empty name → Unknown.

## Loading
- `TraceModel::load(path, LoadOptions, progress)`: chunks are decoded on `options.threads` workers (0 = hardware concurrency; the calling thread is one of them). `progress` (monotonic, 1.0 on success) is invoked only on the calling thread. Loading is deterministic for any worker count. `LoadOptions::cancel` (an `const std::atomic<bool>*`) is polled between work items; a cancelled load returns `Cancelled`. Partial or torn files load what is recoverable (`isPartial`).
- tid 0 on events means "no thread" (invalid ThreadIdx). The latest ThreadNames record by ts sets `Thread::name`. Duplicate ThreadRecs are kept, and `tidToThread` maps to the first.
- `statesByThread` and `scopesByThread` are sized to the thread count and empty; consumers build them (`buildThreadStates`, scope pairing). `QueryContext` requires `statesByThread` to be built.
- Streaming load (WP-64): `LoadOptions::prefixFraction` (default 1.0) loads only that fraction of the Samples chunks as a consistent time prefix: tables complete, `header.end_ts` clamped to the loaded samples, events past it dropped, `loadStatus.complete == false`. Thread indices of threads with a ThreadRec equal the full load's (threads synthesised from samples alone may differ; real traces have ThreadRecs). `LoadOptions::generation` is copied into `loadStatus.generation`. `TraceModel::loadStatus` = {complete, generation, sampleChunksLoaded/Total, fullEndTs, samplesLoaded}. A file with fewer than 2 Samples chunks always loads completely.
- `TraceModel::loadProgressive(path, options, stages, onSnapshot, progress)` publishes a preview per stage (ascending fractions in (0, 1), e.g. `{0.05}`), then returns the complete model; cancellation returns Cancelled. Each preview is an independent model (it pays its own load of the table chunks), so peak memory is the preview plus the final model while the consumer still holds the preview.
- Live models (WP-65): `model::LiveModel` (`libs/model/live_model.h`) builds ordinary `TraceModel` snapshots of a sliding window with `isPartial = true`, `loadStatus.complete = false` and `captureInfo.raw_json = {"live":true,...}`; see live.md.

## Viewer behaviour tied to the model
- Files of 64 MiB or more open as a 5 % preview, then the complete document replaces it, keeping the timeline view, row flags and scroll. Closing the preview abandons the load; a newer open supersedes it.
- Memory ceiling: `Settings::memoryCeilingPercent` (JSON key `memoryCeilingPercent`, default 75 % of physical memory, 0 = none; View > Memory limit). The timeline index is budgeted up front (ceiling minus the model); over the ceiling the governor degrades, in order, the LOD fine level, the finest LOD levels, the analysis cache budget (toast and banner at each stage), then shows "model alone exceeds the ceiling". Nothing is restored automatically.
- `tachyon-viewer --bench <trace> [--bench-frames N]` runs a headless steady/pan/zoom frame-time benchmark (dogfooding entry point; capture it with `tachyon-agent --backend perf`).

## Changelog
- 2026-10-25 (WP-30): additional types and members, `load()` contract, tid 0 rule, memory note.
- 2026-11-03 (WP-64): streaming load, progressive load, viewer preview, memory ceiling, `--bench`.
- 2026-11-15: clarifications folded into the main text; member list brought up to date (`isPartial`, `loadStatus`, raw event streams, `loadProgressive`).
