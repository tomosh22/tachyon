# Development Plan: a Superluminal-class CPU Profiler for a Custom AAA Engine

Date: 2026-10-24

Assumptions (adjust if wrong):
- The engine is C++ and ships first on Windows x64. Linux is secondary. Consoles may follow if you have SDK access (Superluminal's console backends use first-party APIs under NDA, so they are out of scope for this public plan).
- You control the engine build pipeline, so you can emit PDBs, build IDs, frame pointers, and instrumentation macros in every build configuration.
- Primary users are engine and gameplay programmers on the same team, not external customers. That changes priorities: capture quality and analysis depth over installers and licensing.

---

## Part 1. What Superluminal is and how it works (research summary)

### 1.1 Positioning
Superluminal is a sampling profiler first, with instrumentation layered on top. Its pitch against engine-side instrumentation profilers (Tracy, Optick, Unreal Insights) is:
- Very high sampling rate: 8 kHz default on Windows, up to 40 kHz on PlayStation, versus ~1 kHz for most sampling tools.
- No code changes required to get useful data. Instrumentation via the Performance API is optional and is "seamlessly blended" into every view.
- It captures from process launch (DLL loads, static init), and it captures third-party and system code, not just your own.
- A UI built around thread interaction: thread states, blocking and unblocking callstacks, and a per-thread timeline that stays responsive on multi-GB captures.

### 1.2 Capture mechanism per platform
**Windows (primary):** Event Tracing for Windows (ETW) kernel logger.
- Sampled profile events (`EVENT_TRACE_FLAG_PROFILE`) with kernel stack walking enabled for the `PerfInfo/SampledProfile` event.
- The sampling interval is set with `TraceSetInformation(TraceSampledProfileIntervalInfo)`. The kernel's minimum interval is 1221 units of 100 ns, which is 122.1 µs, i.e. ~8.19 kHz. That is where Superluminal's "8 kHz on Windows" comes from; it is the ETW ceiling, not a design choice.
- Context switch events (`CSWITCH`) and ready-thread events (`DISPATCHER`/`ReadyThread`) with stacks. Together they give: when each thread ran, on which CPU, why it stopped (wait reason), which thread made it runnable (the "unblocking" stack), and the gap between "ready" and "actually scheduled".
- Process, thread, and image-load rundown events so addresses can be mapped to modules.
- Superluminal writes a standard ETL-style capture and can also import ETL files recorded externally (its support page recommends UIForETW with "Context switch callstacks", "Compress trace", and "Fast sampling" enabled). Its changelog mentions handling circular-buffer traces and stack-compressed traces.
- Requires administrator privileges, as all kernel ETW consumers do.

**Linux (beta):** perf events plus eBPF.
- eBPF programs attached to the perf sampling event and to scheduler context-switch events, emitting to userspace through a BPF ring buffer map.
- A custom in-kernel stack unwinder that uses `.eh_frame` unwind tables uploaded per binary, so frame pointers are not required. The founder's blog posts document two kernel bugs they hit doing this (eBPF resilient spinlocks causing system freezes, and slow map-in-map updates), which is a useful warning about the cost of that approach.
- The Linux beta did not yet implement the instrumentation API at the time of the page I could see.

**Consoles:** Xbox and PlayStation 4/5 via first-party profiling APIs. Not reproducible without the SDKs.

### 1.3 The Performance API (instrumentation), version 3.0
The headers are BSD-licensed and self-contained (the copy I read is embedded in Embark's `superluminal-perf-rs` repo). Key design points worth copying:
- C ABI, with a thin C++ RAII wrapper (`InstrumentationScope`) and macros (`PERFORMANCEAPI_INSTRUMENT`, `_DATA`, `_COLOR`, `_FUNCTION`, …).
- `BeginEvent(id, data, color)` / `EndEvent()`. The `id` must be a stable static string per scope (it is the aggregation key). `data` is an optional per-invocation string (frame number, file path, asset name). `color` is RGB packed into a uint32 and must be stable per id.
- `_N` variants that take explicit lengths, and `_Wide` variants for UTF-16.
- `SetCurrentThreadName`.
- Fiber support: `RegisterFiber`, `UnregisterFiber`, `BeginFiberSwitch(current, next)`, `EndFiberSwitch(current)`. This is how the tool keeps timelines coherent for job systems that run fibers across OS threads.
- `EndEvent` returns a 24-byte struct purely to defeat tail-call optimization, so the EndEvent call site stays inside the function that called BeginEvent. Begin/End must be in the same function. This strongly suggests Superluminal records the caller's return address and uses it to splice the instrumentation scope into the sampled callstack at the right depth.
- Deployment: either link a static lib, or `LoadLibrary("PerformanceAPI.dll")`, `GetProcAddress("PerformanceAPI_GetAPI")`, and request a function table for an exact header version. If the DLL is absent, all calls become no-ops. A one-header loader is provided. Everything compiles out when `PERFORMANCEAPI_ENABLED` is 0.

### 1.4 Analysis and UI feature set
- **Session flow:** Run (launch, optionally "start profiling immediately" so static init is captured), Attach, or Session Explorer (stored captures). Capture options: sampling rate, child-process profiling with filter. A command-line capture tool exists for remote machines and automation.
- **Symbols:** resolved after capture, with symbol-server support. Rust demangling, .NET mixed stacks.
- **Threads view (timeline):** one row per thread with a summary band at the top of each row; green = executing, other colors = wait states (synchronization, I/O, memory management/paging, etc.), tooltips explain the state. Rows expand to show the sampled callstack over time (a flame chart driven by samples, not instrumentation). Hovering shows thread interaction arrows; hovering or clicking shows the blocking and unblocking callstacks, and the docs describe the gap between "set ready" and "scheduled in".
- **Time filter:** drag a range, press F; every other view filters to it. Selecting a function or a call-graph node also acts as a filter.
- **Call Graph:** top-down hierarchical tree with inclusive/exclusive time and a pie chart of child distribution; "follow the hot path".
- **Function List:** bottom-up, sortable by exclusive/inclusive time regardless of call site; can be restricted to a call-graph subtree.
- **Butterfly (callers/callees)** for the selected function.
- **Function Statistics:** per-function or per-instrumentation-event instance statistics with histogram and box plot; usable as a search (Ctrl+F) and populated from almost any view; selecting an entry highlights it in the timeline.
- **Source & Disassembly:** per-line timing and thread-state attribution; mixed-mode disassembly with per-instruction timing when source is missing or when you drill down.
- **Instrumentation:** shown in the timeline (colored lanes with data strings), in the call graph (blended with sampled frames), and in Function Statistics.
- **Scale:** The engineering post "Compressing callstacks" describes a 2.64 GiB ETL from Unity with 21 million callstacks (4.2 GiB raw). Deduplication removed 96%; callstacks are stored as a static DAG where each node is a 16-byte (parent index, frame) record interned through a keyless hash map, and a stack is reconstructed by walking leaf to root. This is the core data structure that keeps the UI fast and is worth adopting directly.

### 1.5 What this means for a clone
The hard parts, in order: (1) a correct, lossless kernel capture at 8 kHz with context switches; (2) a compact in-memory model that handles tens of millions of samples with interactive time filtering; (3) a timeline renderer that stays at 60 fps on that data; (4) symbolication with inline frames and line tables; (5) blending instrumentation into sampled stacks convincingly. The UI widgets themselves (trees, lists, pie charts) are comparatively cheap.

---

## Part 2. What owning the engine source buys you

Superluminal is designed for people who cannot change the target. You can, and that changes several decisions:

1. **Deterministic symbols.** Emit PDBs for every configuration including shipping-with-symbols, embed a build ID, and run `symstore` on CI so the profiler can resolve any build by GUID/age. This removes the whole "symbol path" support burden.
2. **Frame pointers everywhere.** Build with `/Oy-` on MSVC and `-fno-omit-frame-pointer` on Clang/GCC. On Windows x64 the kernel stack walker uses unwind tables so this is not strictly required, but it makes Linux trivial: `perf_event_open` with `PERF_SAMPLE_CALLCHAIN` just works, and you never need the eBPF `.eh_frame` unwinder (which is also the subject of a patent referenced in the OpenTelemetry eBPF profiler README).
3. **Instrumentation at the right seams, built in from day one.** Frame boundaries, job system (job begin/end, dependencies, worker wake-ups), fibers (switch events), named locks with the lock address, allocator tags, streaming/I/O with asset path as the data string, render thread submit points, GPU timestamp queries. Superluminal users hand-sprinkle these; you can generate them from the job system and lock wrappers once.
4. **Fiber-aware timelines.** OS context switches cannot see fiber switches. Because you own the scheduler, you can emit fiber switch events so the timeline can present per-fiber (per-job) lanes, not only per-OS-thread lanes.
5. **Thread identity.** Name every thread with both the OS mechanism (`SetThreadDescription`) and the profiler API, and tag thread roles (main, render, worker N, audio, streaming) so the UI can group and sort them sensibly by default.
6. **Trigger-driven capture ("flight recorder").** Keep the kernel logger in circular-buffer mode permanently on dev machines. When the engine detects a hitch (frame > budget), it asks the agent to flush the last N seconds to disk. This catches the one-off 80 ms spike that nobody can reproduce, which is the most valuable thing a AAA profiler does.
7. **Automation.** A headless CLI capture plus a report generator lets CI profile a fixed camera path per commit and flag regressions by function and by frame-time percentile.
8. **Correlated GPU data.** Convert GPU timestamp queries to the CPU clock and show a GPU lane under the render thread so CPU-bound vs GPU-bound is visible in one view.
9. **Engine-side data channels.** Frame number, level/area, camera position, memory budget, and streaming state as markers, so a slow region in the timeline can be tied back to what the game was doing.

---

## Part 3. Target architecture

### 3.1 Components
```
+------------------+     +-------------------+     +------------------+
| Instrumentation  |     | Capture Agent     |     | Trace file       |
| client (in-proc) | --> | (privileged proc) | --> | .trace (chunked, |
| header + tiny lib|     | ETW / perf        |     |  zstd, versioned)|
+------------------+     +-------------------+     +--------+---------+
                                                            |
                                                            v
+------------------+     +-------------------+     +------------------+
| UI (ImGui app)   | <-- | Analysis engine   | <-- | Loader +         |
| timeline, graphs |     | DAG, intervals,   |     | Symbolicator     |
| source/disasm    |     | aggregation,filter|     | (PDB/DWARF, async)|
+------------------+     +-------------------+     +------------------+
```
- **Capture Agent:** one executable, runs elevated, hosts the kernel trace session, launches or attaches to the target, receives instrumentation data from the client over shared memory, writes the raw trace. Also exposes a small TCP/named-pipe control protocol so the UI, the engine (hitch trigger), and CI can start/stop/flush.
- **Instrumentation client:** a single public header modeled on the Performance API (same function set, same no-op-when-absent behavior), backed by per-thread lock-free ring buffers in shared memory. Timestamps from `QueryPerformanceCounter` (ETW uses QPC when configured with `EVENT_TRACE_CLOCK_PERFORMANCE_COUNTER`, so no clock conversion is needed) or `rdtsc` with a calibration record.
- **Trace file:** append-only chunks (header, module table, thread table, sample chunk, cswitch chunk, instrumentation chunk, string table), each zstd-compressed, with a footer index. Versioned from day one.
- **Symbolicator:** background threads; Windows via DIA SDK (inline frames, line tables, public symbols) with DbgHelp `SymSrv` for symbol-server fetch; Linux via LLVM DebugInfo/DWARF. Results cached per (module GUID/age, RVA).
- **Analysis engine:** builds the in-memory model (section 3.2), answers queries for the UI under a time filter, all on worker threads so the UI never blocks.
- **UI:** Dear ImGui (docking branch) on D3D11/Vulkan with custom-drawn timeline, tree, and source widgets. Tracy and Optick prove this stack can render profiler timelines at scale; it also lets engine programmers modify the tool without learning a UI framework.

### 3.2 In-memory data model
- `Module { path, base, size, guid/age or build-id, symbol state }`
- `Thread { tid, pid, name, role, first/last seen }`
- `Frame = (module index, rva)` interned to a 32-bit id. Symbolication attaches `{function, file, line, inline chain}` lazily.
- `CallstackNode { parent: u32, frame: u32 }` in one flat array (the Superluminal DAG); `CallstackId` = node index. Interning through an open-addressing hash of (parent, frame) that stores only node indices, not keys.
- `Sample { ts: u64, tid: u32, cpu: u8, callstack: CallstackId }` stored in structure-of-arrays, sorted by thread then time, so a time filter is two binary searches per thread.
- `ContextSwitch { ts, cpu, old_tid, new_tid, old_state, wait_reason, old_stack, readying_tid, readying_stack, ready_ts }` folded into per-thread `ThreadStateInterval { start, end, state, cause, blocking_stack, unblocking_stack, woke_by }`.
- `ScopeEvent { begin_ts, end_ts, tid (or fiber id), id_string, data_string, color, depth, caller_rva }` per thread, with a per-thread depth stack to pair begin/end and to emit a "scope tree" per frame.
- `FiberSwitch`, `FrameMarker`, `Marker` arrays.
- Derived per-frame aggregates (frame time, per-thread busy time, top N functions) for the frame graph and for "jump to slowest frame".

### 3.3 Attribution rules
- Each sample has weight = sampling interval, clipped to the thread's running interval from context-switch data (this is why 8 kHz plus context switches makes attribution "straightforward", as the docs put it). A sample that lands in a thread that was not running is discarded.
- Inclusive time of a function = sum of sample weights whose stack contains it (counted once per stack, even under recursion). Exclusive = samples whose leaf frame is it.
- Instrumentation blending: when a sample falls inside an open scope on that thread, insert a synthetic frame for the scope directly above the frame whose RVA matches the scope's recorded caller return address (fall back to directly above the root when no match). This is the trick that makes scopes appear in the call graph in the right place.
- Thread state per source line: a line's time breaks down into running/ready/waiting by looking at the thread state interval that covers each sample or context switch stack whose leaf is that line.

---

## Part 4. Phased roadmap

Estimates assume one strong systems programmer full time, plus occasional help from engine owners for integration. Each phase ends with a demo on the real engine.

### Phase 0: Foundations and de-risking (1–2 weeks)
- Repo, CMake, CI, coding standards. Decide C++20 for all components.
- Spike: record an ETL of the engine with `xperf -on PROC_THREAD+LOADER+PROFILE+CSWITCH+DISPATCHER -stackwalk Profile+CSwitch+ReadyThread -SetProfInt 1221`, parse it with `OpenTrace`/`ProcessTrace`, and dump counts of samples, stacks, and context switches. This validates privileges, buffer sizing, and the event layouts before any product code exists.
- Spike: load that ETL in Windows Performance Analyzer to have a reference for validating your own attribution later.
- Write the "profiler testbed": a small app with known behavior (spin loop in function A for 40%, sleep 30%, mutex contention between two threads, file I/O, a fiber switch). Every later phase is validated against this.
- Exit criteria: numbers from the spike match WPA within a few percent; testbed exists.

### Phase 1: Windows capture agent (3–4 weeks)
- Kernel session via `StartTrace` (private system logger or NT Kernel Logger), flags for process/thread/image/profile/cswitch/dispatcher, `TraceSetInformation(TraceStackTracingInfo)` for Profile, CSwitch, ReadyThread; `TraceSampledProfileIntervalInfo` to 1221; QPC clock; large buffers (e.g. 1 MB × 64 minimum) tuned until lost-event counters stay at zero at 8 kHz on a 32-core machine.
- Launch mode: create the target `CREATE_SUSPENDED`, start the session, resume, so static init is captured. Attach mode: filter by PID at analysis time (ETW is system-wide anyway). Optional child-process following.
- File mode first (write ETL, consume after stop). Real-time mode later for live view.
- Own `.trace` writer: convert ETL events to the chunked format; keep the raw ETL as an optional import path so externally recorded traces still load.
- CLI: `prof capture --launch game.exe --args ... --duration 30 --out run.trace`, `prof attach --pid`, `prof stop`, `prof flush` (circular mode).
- Exit criteria: 60 s capture of the engine at 8 kHz with zero lost events; CLI usable by CI.

### Phase 2: Symbolication (3–4 weeks)
- Module table from image-load events with PDB GUID/age read from the PE debug directory.
- DIA-based resolver: RVA to function, inline frames, file/line, plus a function-range table for fast lookups. DbgHelp `SymSrv` for fetching from your symbol store; local cache.
- Demangling, deduplicated function/file string tables, "unknown module" and "kernel" placeholders.
- Fully asynchronous: the UI opens the trace immediately with addresses and fills in names as modules resolve (this is Superluminal's "might take a few minutes" behavior; make it progressive rather than blocking).
- Exit criteria: testbed resolves 100% of frames including inlined ones; engine capture resolves all first-party modules within a minute from the symbol store.

### Phase 3: Analysis engine (4–6 weeks)
- Callstack DAG interning, SoA sample storage, per-thread sorting, memory budget targets (aim for < 4× the ETL size resident).
- Thread state intervals from CSwitch + ReadyThread, with wait reasons, readying thread, blocking and unblocking stacks, and ready-to-run latency.
- Sample weight clipping to running intervals.
- Query layer under a time filter and optional thread filter: top-down tree, bottom-up list, butterfly for a function, per-line and per-instruction histograms for a function, per-thread state totals, frame statistics.
- Function statistics (instances, histogram, box plot) for both sampled functions and scopes.
- Golden tests against the testbed: attribution within tolerance; locks show the correct blocker.
- Exit criteria: a 2 GB capture loads in under a minute and answers filtered queries in well under 100 ms.

### Phase 4: UI MVP (6–8 weeks)
- Session Explorer, Run/Attach dialogs, capture options.
- Threads view: rows with summary band (state colors), expandable to a sample-driven flame chart; LOD rendering by pre-bucketing samples at multiple zoom levels; pan/zoom; thread sort by busy time; pin and hide threads.
- Time selection, F to filter, Esc to clear; all views subscribe to the filter.
- Call Graph with inclusive/exclusive columns and child pie chart; hot path expand.
- Function List (bottom-up) with sorting and "restrict to subtree".
- Butterfly panel.
- Source view with per-line time and per-line thread-state bars; falls back to disassembly (Zydis) with per-instruction time.
- Thread interaction: hover arrows, click for blocking/unblocking stacks in a bottom panel, Ctrl-hover compact tooltip.
- Search (Ctrl+F) into Function Statistics; selection highlights in timeline.
- Exit criteria: an engine programmer can find a real hitch end to end without the author's help.

### Phase 5: Instrumentation API and engine integration (3–4 weeks)
- Public header mirroring the Performance API surface (Begin/End, `_N`, thread name, fibers, color, data string) plus additions you want: `FrameMark`, `Marker`, `Plot(name, value)`, `LockBegin/End(address)`, `JobBegin/End(job id, deps)`.
- Implementation: per-thread SPSC ring in shared memory, 16- to 32-byte records, string ids interned once per static string (pointer identity is enough because ids must be static), data strings copied, caller return address captured via `_ReturnAddress()`, tail-call suppression on End.
- Dynamic loading with a version-checked function table so shipping builds carry only the header and a no-op path.
- Engine hooks: job system, fibers, named locks, frame boundaries, render submit, streaming, allocator tags; console command and hotkey for start/stop/flush; hitch trigger to the agent.
- Analysis and UI: instrumentation lanes in the timeline with data strings, blended scopes in call graph and function list, frame-time graph with click-to-zoom, per-frame top functions.
- Exit criteria: job lanes and fiber-coherent timelines visible; frame markers drive a frame graph; no measurable overhead with the agent absent, under 1% with it present.

### Phase 6: Scale, robustness, automation (4–6 weeks)
- Circular-buffer flight recorder mode with trigger-to-disk; long-session streaming load; trace format versioning and migration tests.
- Real-time attach view (optional): live timeline during capture.
- Remote capture: agent on the target machine, UI connects over TCP and pulls the trace.
- CI mode: headless capture of a scripted run, JSON report of frame percentiles and top-N functions, diff against baseline with thresholds.
- Export to speedscope / Chrome trace / Perfetto JSON for sharing with people who do not have the tool.
- Polish: keyboard navigation, persistent layouts, theming, crash reporting of the tool itself.

### Phase 7: Platform expansion (ongoing)
- Linux: `perf_event_open` (cpu-clock or hardware cycles at 8–10 kHz, `PERF_SAMPLE_CALLCHAIN` with frame pointers, `PERF_SAMPLE_TID/TIME/CPU`), `sched:sched_switch` and `sched:sched_wakeup` tracepoints via perf or a small eBPF program, DWARF symbolication, build-id matching. Skip in-kernel `.eh_frame` unwinding; you own the build flags.
- Consoles: if you have SDK access, each has a native sampling API; map it onto the same trace format so the analysis and UI are shared.
- GPU timeline correlation, memory allocation sampling with stacks, network/streaming lanes.

### Timeline summary
| Milestone | Cumulative time |
|---|---|
| Windows sampling MVP (phases 0–4) | ~4–5 months |
| With engine instrumentation (phase 5) | ~5–6 months |
| v1 with flight recorder, CI, remote (phase 6) | ~7–8 months |
| Linux backend | +1–2 months |

---

## Part 5. Technology recommendations
- **Language:** C++20 throughout. The engine team can maintain it, and the instrumentation header is shared with the engine.
- **UI:** Dear ImGui docking branch, custom draw lists for timeline/source/trees, D3D11 backend on Windows and Vulkan elsewhere.
- **Windows capture:** ETW APIs directly (`advapi32`/`tdh`); do not take a dependency on the .NET TraceProcessing library in the product, but do use it or WPA for validation.
- **Symbols:** DIA SDK + DbgHelp (symbol server) on Windows; LLVM DebugInfo on Linux. Keep a resolver interface so a pure-C++ PDB reader (e.g. raw_pdb) can replace DIA later if its COM dependency becomes painful.
- **Disassembly:** Zydis (MIT).
- **Compression:** zstd.
- **Hash maps / containers:** your engine's, or absl/ankerl for the interning tables.
- **Testing:** GoogleTest or the engine's framework; golden traces from the testbed checked into LFS; a perf test that loads a large trace and asserts query latency.

## Part 6. Risks and known gotchas
- **ETW lost events.** At 8 kHz with stacks on many cores, default buffers drop events silently. Monitor `EventsLost`, size buffers generously, and prefer file mode over real-time mode until real-time consumption is proven.
- **Admin requirement.** The agent must run elevated; design the UI so only the agent is elevated, not the viewer.
- **Kernel-mode frames and DPC/ISR samples.** Decide early how to present kernel time (Superluminal shows it); you need kernel symbols from the Microsoft symbol server for that.
- **32-bit targets and WoW64 stacks** are much harder in ETW; scope them out unless the engine ships 32-bit.
- **Timestamps.** Force the session clock to QPC and timestamp instrumentation with QPC; mixing rdtsc without calibration causes drift that misaligns scopes and samples.
- **Tail calls and inlining.** The caller-address trick for scope placement needs End calls that are not tail-call optimized and Begin/End in the same function; document and enforce via the macro wrapper.
- **Identical code folding and inlining** make sample attribution surprising; surface inline frames explicitly in the call graph so users trust the numbers.
- **UI scale.** The timeline must never iterate raw samples at low zoom; precompute multi-level buckets per thread when loading.
- **Scope creep.** Memory profiling, .NET support, and GPU profiling are each their own product. Keep them behind the trace format's extensibility, not on the v1 critical path.
- **eBPF on Linux.** Superluminal's own write-ups show in-kernel unwinding uncovered real kernel bugs. Frame pointers plus `perf_event_open` avoid that whole class of problems for a first-party engine.

## Part 7. Immediate next steps
1. Confirm the assumptions at the top (platforms, language, team size).
2. Run the Phase 0 xperf spike on a real engine build and record sample/cswitch counts and lost-event numbers.
3. Build the profiler testbed with the known-answer workloads.
4. Add build-ID embedding, PDB retention, and `symstore` publishing to engine CI now, since every later phase depends on it.
5. Pick the UI stack by porting one Tracy-style timeline widget over a synthetic 10-million-sample dataset and measuring frame time.

---

## Sources
- Superluminal product and docs: https://superluminal.eu/ , https://superluminal.eu/cplusplus/ , https://superluminal.eu/workflow/ , https://superluminal.eu/applications/windows/ , https://superluminal.eu/applications/linux/ , https://superluminal.eu/?p=3156 (PlayStation), https://www.superluminal.eu/docs/documentation.html , https://superluminal.eu/version-history/ , https://superluminal.eu/support/
- Superluminal engineering posts: https://superluminal.eu/compressing-callstacks/ , https://rovarma.com/articles/from-profiling-to-kernel-patch-the-journey-to-an-ebpf-performance-fix/ , https://rovarma.com/articles/a-tale-about-fixing-ebpf-spinlock-issues-in-the-linux-kernel/
- Performance API headers (BSD) as embedded in https://github.com/EmbarkStudios/superluminal-perf-rs (sys/external/include/Superluminal/PerformanceAPI_capi.h, PerformanceAPI.h, PerformanceAPI_loader.h)
- Third-party usage write-ups: https://dev.to/attiliohimeki/unity-profiling-using-superluminal-2oc8 , https://discussions.unity.com/t/profiling-in-unity-using-superluminal/1614358 , https://blog.s-schoener.com/2025-01-21-unity-vfx-graph-multithread/ , https://blog.s-schoener.com/2025-02-13-superluminal-compute-memory/ , https://github.com/TomerAberbach/profiler-md/issues/228
- ETW background: https://davekilian.com/windows-profiling.html , https://learn.microsoft.com/en-us/previous-versions/windows/desktop/xperf/stack-walking , https://learn.microsoft.com/ko-kr/archive/blogs/sajay/how-to-collect-stacks-during-context-switches , https://github.com/PascalBeyer/Simple-ETW-based-Profiler
- Tracy design notes (sampling rates, context-switch matching): https://github.com/wolfpld/tracy
- eBPF unwinding without frame pointers: https://www.elastic.co/blog/universal-profiling-frame-pointers-symbols-ebpf , https://pkg.go.dev/go.opentelemetry.io/ebpf-profiler
