# Tachyon: Implementation Plan for a Superluminal-class CPU Profiler

Handoff document for an orchestrating agent (Opus) that delegates work packages to implementation subagents (Sonnet).
Version 1.0, 2026-10-24. Codename `tachyon` is a placeholder; rename consistently if the user prefers another name.

---

## 0. How to use this document (read first, orchestrator)

### 0.1 Roles
- **Orchestrator (you):** owns the schedule, the shared specifications in Part 3, integration, code review, and the user conversation. You do not implement work packages yourself except WP-00 and integration fixes under ~50 lines.
- **Implementation subagents:** each receives exactly one work package (WP) from Part 4, implements it on a branch, and returns a report. They never change a shared interface in Part 3 without your approval.
- **User:** the engine owner. Only they can answer the open questions in Part 7. Do not block on them; proceed under the stated assumptions and flag.

### 0.2 Workflow per work package
1. Check the WP's `Depends on` list is complete and merged.
2. Create the brief using the template in 0.4. Paste the WP section verbatim, plus the Part 3 spec sections it names, plus Part 2.4 conventions. Do not summarize specs; copy them.
3. Subagent implements on branch `wp/<id>-<slug>`, adds tests, runs `ctest`, and reports with the template in 0.5.
4. You run the build and tests yourself (or via a fresh verifier subagent for anything over ~500 lines), review the diff against the acceptance criteria, then merge to `main`.
5. After each milestone gate (Part 5), run the gate checklist before starting the next milestone's WPs.

### 0.3 Rules that are not negotiable
- Decisions in Part 1.4 are made. Subagents do not re-open them. If a subagent finds a decision unworkable, it stops and reports; you decide.
- Shared headers listed in Part 3 are owned by the orchestrator. A WP may propose a diff to them in its report; it may not merge one.
- Every WP ships tests runnable on Linux unless marked `Windows-only test`. Windows-only code must still compile-check behind `#if TACHYON_PLATFORM_WINDOWS` and be covered by fixture-based tests where possible.
- No new third-party dependency outside Part 2.3 without orchestrator approval.
- Public API surface (`sdk/include/tachyon/tachyon.h`) is frozen after WP-50 is merged; later changes bump `TACHYON_API_VERSION`.
- Never commit secrets, symbol server URLs with credentials, or engine source. The engine is not in this repo.

### 0.4 Subagent brief template
```
You are implementing work package <WP-ID> <title> of the Tachyon profiler.
Repository: <path>. Branch: wp/<id>-<slug>. Build: cmake --preset <preset> && cmake --build --preset <preset> && ctest --preset <preset>.

== Context ==
<Part 1.1–1.4 pasted>
<Part 2.4 conventions pasted>

== Specifications you must conform to (do not modify these files) ==
<the Part 3 sections named in the WP, pasted verbatim>
Files you may edit: <list>. Files you may create: <list>. Files you must not touch: <list>.

== Your work package ==
<WP section pasted verbatim>

== Definition of done ==
- All acceptance criteria met and demonstrated by tests under tests/<area>/.
- `ctest` passes on Linux (and on Windows if the WP says so).
- No warnings at the project warning level. clang-format applied.
- Report using the required report format.

== Report format ==
<Part 0.5 pasted>
```

### 0.5 Subagent report format
```
WP: <id>
Status: complete | partial | blocked
Files added/changed: <list>
How to verify: <exact commands>
Test results: <paste ctest summary>
Deviations from spec: <none | list with reasons>
Proposed changes to shared headers: <none | diff>
Open issues / follow-ups: <list>
Measured numbers (if the WP asks for any): <table>
```

### 0.6 Parallelization guide
Lanes that can run concurrently once WP-00 and WP-03 are merged:
- Lane A (Windows capture): WP-01, WP-10, WP-11, WP-12, WP-13
- Lane B (symbols): WP-20, WP-21, WP-22
- Lane C (analysis): WP-04, WP-30, WP-31, WP-32, WP-33, WP-34, WP-35, WP-36
- Lane D (UI shell): WP-40, WP-41 (later UI WPs need Lane C)
- Lane E (SDK): WP-50, WP-51
Keep at most one subagent per lane at a time; they share files within a lane.

---

## 1. Charter

### 1.1 Goal
A CPU profiler for the user's custom AAA game engine with the capabilities that make Superluminal valuable: high-frequency kernel sampling (8 kHz on Windows), context-switch-aware thread states with blocking and unblocking callstacks, optional instrumentation blended into sampled data, and a UI built around a per-thread timeline that remains interactive on multi-gigabyte captures. Because the engine source is available, add first-class support for the engine's job system, fibers, locks, frame markers, and a hitch-triggered flight-recorder mode.

### 1.2 Non-goals for v1
Memory profiling, GPU profiling beyond timestamp correlation, .NET or scripting-language stacks, 32-bit targets, console backends (need NDA SDKs), a commercial installer or licensing.

### 1.3 Assumptions (confirm with user, see Part 7)
- Engine is C++ (C++17 or later), ships on Windows x64 first, Linux second.
- Engine build can be changed: emit PDBs for every configuration, embed build IDs, keep frame pointers, publish symbols to a symbol store.
- Dev machines allow running an elevated capture agent.
- Primary users are the engine team.

### 1.4 Decisions (final; do not re-litigate)
| Area | Decision | Rationale |
|---|---|---|
| Language | C++20 for agent, libraries, viewer, tools. C99-compatible public SDK header. | Matches the engine; one toolchain. |
| Build | CMake 3.25+ with presets; Ninja; MSVC 2022 on Windows, Clang 16+ on Linux. | Standard. |
| UI | Dear ImGui (docking branch) + SDL2 + OpenGL 3.3 backend on all platforms. | Proven for profilers (Tracy), cross-platform, hackable by engine programmers. |
| Windows capture | ETW kernel logger through `advapi32` directly (StartTrace/ControlTrace/TraceSetInformation/OpenTrace/ProcessTrace). File mode (ETL) first; real-time mode is a v1.1 option. | Same mechanism as Superluminal and WPA. |
| Linux capture | `perf_event_open` with frame-pointer callchains plus `sched` tracepoints. No eBPF and no in-kernel `.eh_frame` unwinding. | Engine builds with frame pointers; avoids kernel-side complexity and a patent-encumbered technique. |
| Sampling clock | Windows: QPC (`ClientContext = 1`); instrumentation timestamps also QPC. Linux: `CLOCK_MONOTONIC_RAW` via perf `use_clockid`. TSC fast path is a later optimization with calibration records. | One clock domain per platform, no drift. |
| Trace storage | Own chunked, zstd-compressed, versioned `.tachyon` file (Part 3.1). Callstacks interned into a DAG at conversion time. Raw ETL kept optionally alongside. | Load speed and memory; import path for external ETLs. |
| Symbols | Windows: DIA SDK (`msdia140.dll`, registration-free) for functions, inlines, lines; own HTTP symbol-server client with two-tier local cache. Linux: elfutils `libdw`. Behind one `ISymbolProvider` interface. | DIA is the most complete PDB reader; the interface allows swapping in `raw_pdb` later. |
| Disassembly | Zydis for x86-64. | MIT, small, accurate. |
| Instrumentation transport | Per-thread SPSC ring buffers in process memory, drained by a client background thread to a named pipe (Windows) / Unix socket (Linux) connected to the agent; falls back to a sidecar `.tachyon-inst` file when no agent. | Low overhead; works without the agent. |
| Scope placement in stacks | Begin records capture the caller return address from a `noinline` function; End is tail-call-suppressed. Analysis splices scopes above the matching frame. | Mirrors the Performance API's design. |
| Testing | GoogleTest; fixture `.tachyon` traces from `tracegen`; profiler testbed app with known answers; perf budget tests. Linux CI for the core; Windows CI for ETW and DIA. | Most code testable without Windows or privileges. |
| Licensing of deps | Permissive only (MIT/BSD/Apache/zlib), except libdw (LGPL, dynamically linked) and DIA (Microsoft redistributable). | Internal tool; keep options open. |

---

## 2. Repository

### 2.1 Layout
```
tachyon/
  CMakeLists.txt            CMakePresets.json  .clang-format  .clang-tidy  .gitattributes (LFS for fixtures)
  cmake/                    toolchain + warnings + dependency helpers
  third_party/              vendored or FetchContent (see 2.3)
  sdk/
    include/tachyon/tachyon.h           public C API + C++ RAII wrapper + macros (WP-50)
    include/tachyon/tachyon_loader.h    dynamic loader (WP-50)
    src/                                 client runtime (WP-51)
  libs/
    core/        platform.h, time.h, logging, arena, hash maps, span utilities, error type
    trace/       .tachyon format reader/writer, string table, callstack interner (WP-03)
    control/     agent control protocol (JSON lines over pipe/socket) (WP-10)
    capture/     ICaptureBackend + windows_etw/ + linux_perf/ (WP-11, WP-70)
    etlconvert/  ETL -> .tachyon conversion (WP-12)
    symbols/     ISymbolProvider, range tables, symbol server client, dia/, dwarf/ (WP-20..23)
    model/       in-memory trace model and loader (WP-30)
    analysis/    thread states, attribution, aggregation, filters, statistics, LOD (WP-31..36, WP-53)
    disasm/      Zydis wrapper (WP-34)
    export/      speedscope / chrome-trace exporters (WP-63)
  apps/
    agent/       elevated capture agent (WP-10..13, WP-52, WP-60, WP-61)
    cli/         tachyon-cli capture/attach/stop/flush/report (WP-13, WP-62)
    viewer/      ImGui application (WP-40..48, WP-54)
    testbed/     known-answer workload app (WP-02)
  tools/
    etlstat/     ETW spike tool (WP-01)
    tracegen/    synthetic fixture generator (WP-04)
    report/      CI report (WP-62)
  tests/
    unit/<lib>/  fixtures/ (LFS)  perf/  windows/ (Windows-only)
  docs/
    specs/       copies of Part 3 as individual files, kept in sync by orchestrator
    engine-integration.md (WP-55)
```

### 2.2 CMake targets and presets
- Static libraries: `tachyon_core`, `tachyon_trace`, `tachyon_control`, `tachyon_capture`, `tachyon_etlconvert` (Windows), `tachyon_symbols`, `tachyon_model`, `tachyon_analysis`, `tachyon_disasm`, `tachyon_export`, `tachyon_sdk` (static) and `tachyon_sdk_shared` (DLL/.so exporting `Tachyon_GetAPI`).
- Executables: `tachyon-agent`, `tachyon-cli`, `tachyon-viewer`, `tachyon-testbed`, `etlstat`, `tracegen`, `tachyon-report`.
- Presets: `linux-debug`, `linux-release`, `windows-debug`, `windows-release`. Options: `TACHYON_BUILD_VIEWER`, `TACHYON_BUILD_TESTS`, `TACHYON_ENABLE_DIA`, `TACHYON_ENABLE_LIBDW`.
- Warnings: `/W4 /WX` (MSVC), `-Wall -Wextra -Werror` (Clang). Sanitizers preset `linux-asan`.

### 2.3 Third-party dependencies (approved list)
| Dependency | Version | License | Use |
|---|---|---|---|
| Dear ImGui (docking branch) | 1.91.x docking | MIT | viewer UI |
| SDL2 | 2.30.x | zlib | window/input |
| glad or SDL GL loader | any | MIT | OpenGL loader |
| zstd | 1.5.x | BSD | chunk compression |
| Zydis | 4.x | MIT | disassembly |
| GoogleTest | 1.14+ | BSD | tests |
| nlohmann/json | 3.11.x | MIT | control protocol, exports, report |
| libcurl (Linux) / WinHTTP (Windows) | system | MIT / OS | symbol server HTTP |
| elfutils libdw/libelf | system | LGPL (dynamic) | DWARF, Linux only |
| DIA SDK | with MSVC 2022 | Microsoft | PDB, Windows only |
| xxHash | 0.8.x | BSD | hashing for interning |
| fmt | 10.x | MIT | formatting (until std::format is uniform) |
No Boost, no Qt, no Abseil.

### 2.4 Conventions (paste into every brief)
- C++20, no exceptions across library boundaries (use `tachyon::Result<T>` from `libs/core/result.h`); exceptions allowed internally only in third_party.
- Namespaces: `tachyon::<lib>`. Files `snake_case.cpp/.h`. Types `PascalCase`, functions `camelCase`, constants `kPascalCase`, members `m_camelCase`.
- Fixed-width integer types everywhere in formats and protocols. All on-disk structs are `#pragma pack(push,1)` with `static_assert(sizeof)`. Little-endian only.
- Timestamps are `uint64_t` ticks in the trace clock; durations in ticks; convert to ns only at UI edges using `TraceHeader.clock_frequency_hz`.
- Ids: `ThreadIdx`, `ModuleIdx`, `FrameIdx`, `CallstackIdx`, `StringIdx` are strong-typedef'd `uint32_t` (`libs/core/strong_id.h`). `kInvalidIdx = 0xFFFFFFFF`.
- No global mutable state except logging. No singletons in libraries.
- Every public function has a one-line comment stating thread safety.
- Tests: one `tests/unit/<lib>/<topic>_test.cpp` per topic; fixtures referenced via `tests/fixtures/` relative path from `TACHYON_FIXTURE_DIR` compile definition.
- Commit messages: `WP-xx: <imperative summary>`.
- Performance-critical code (interning, loading, queries, timeline LOD) must include a `tests/perf/` benchmark with a budget assertion that fails CI on 2× regression.

---

## 3. Shared specifications (orchestrator-owned)

Each subsection becomes a file under `docs/specs/` and, where applicable, a header under `libs/`. Subagents conform to these; proposals to change them go through the report.

### 3.1 `.tachyon` trace file format (v1)

File = `FileHeader` + sequence of `Chunk` + `Footer`. All little-endian, packed.

```c
struct FileHeader {            // 64 bytes
  char     magic[8];           // "TACHYON\0"
  uint32_t format_version;     // 1
  uint32_t flags;              // bit0: has_instrumentation, bit1: circular_capture, bit2: partial (writer crashed)
  uint64_t clock_frequency_hz; // QPC frequency on Windows, 1e9 on Linux
  uint64_t start_ts, end_ts;   // trace clock ticks
  uint64_t footer_offset;      // 0 if not finalized; reader must then scan chunks
  uint32_t platform;           // 1 Windows, 2 Linux
  uint32_t reserved[3];
};
struct ChunkHeader {           // 32 bytes
  uint32_t type;               // ChunkType
  uint8_t  compression;        // 0 none, 1 zstd
  uint8_t  version;            // chunk schema version, starts at 1
  uint16_t reserved;
  uint64_t uncompressed_size;
  uint64_t compressed_size;    // == uncompressed_size when compression==0
  uint64_t record_count;
};
enum ChunkType : uint32_t {
  Strings=1, Processes=2, Threads=3, Modules=4, Frames=5, CallstackNodes=6,
  Samples=7, ContextSwitches=8, ReadyEvents=9, ScopeEvents=10, FiberEvents=11,
  FrameMarks=12, Markers=13, PlotSamples=14, ThreadNames=15, ClockCalibration=16,
  CaptureInfo=17, Footer=0xFFFF
};
```
Record schemas (one record per `record_count`, SoA chunks noted):
```c
// Strings: concatenated UTF-8, each prefixed by uint32 length; StringIdx is the ordinal. Index 0 is "".
struct ProcessRec  { uint32_t pid; uint32_t parent_pid; uint64_t start_ts, end_ts; StringIdx name; StringIdx command_line; };
struct ThreadRec   { uint32_t tid; uint32_t pid; uint64_t start_ts, end_ts; StringIdx name; uint32_t flags; }; // flags bit0 = is_target_process
struct ModuleRec   { uint32_t pid; uint64_t base; uint64_t size; StringIdx path; uint8_t id_kind; /*1 pdb guid+age, 2 gnu build-id, 3 pe timestamp+size*/ uint8_t id_len; uint8_t id[32]; uint32_t age; uint64_t load_ts, unload_ts; };
struct FrameRec    { ModuleIdx module; uint64_t addr_or_rva; };         // module == kInvalidIdx: absolute address (unmapped); else RVA within module
struct CallstackNodeRec { CallstackIdx parent; FrameIdx frame; };       // index 0 reserved as root (parent=kInvalidIdx)
// Samples: SoA, record_count N: uint64 ts[N]; uint32 tid[N]; uint32 callstack[N]; uint16 cpu[N]; uint16 flags[N] (bit0 kernel-mode leaf)
struct ContextSwitchRec { uint64_t ts; uint32_t cpu; uint32_t old_tid; uint32_t new_tid; uint8_t old_state; uint8_t old_wait_reason; uint8_t old_wait_mode; uint8_t new_priority; CallstackIdx old_thread_stack; uint32_t reserved; };
struct ReadyRec    { uint64_t ts; uint32_t cpu; uint32_t readied_tid; uint32_t readying_tid; CallstackIdx readying_stack; uint8_t adjust_reason; uint8_t flags; uint16_t reserved; };
struct ScopeEventRec { uint64_t ts; uint32_t tid; uint8_t kind; /*0 begin,1 end*/ uint8_t reserved[3]; StringIdx id; StringIdx data; uint32_t color_rgba; uint64_t caller_addr; }; // end: id/data/color/caller unused
struct FiberEventRec { uint64_t ts; uint32_t tid; uint8_t kind; /*0 register,1 unregister,2 switch_begin,3 switch_end*/ uint8_t reserved[3]; uint64_t fiber_id; uint64_t other_fiber_id; };
struct FrameMarkRec { uint64_t ts; uint32_t tid; StringIdx name; uint64_t frame_number; };
struct MarkerRec    { uint64_t ts; uint32_t tid; StringIdx name; StringIdx data; uint32_t color_rgba; };
struct PlotSampleRec{ uint64_t ts; StringIdx name; double value; };
struct ClockCalibrationRec { uint64_t trace_ts; uint64_t other_clock_ts; uint32_t other_clock_kind; }; // 1 TSC, 2 GPU
// CaptureInfo: one JSON document (UTF-8) with capture options, host info, events_lost, sampling_hz, agent version.
struct FooterRec   { uint32_t type; uint32_t version; uint64_t offset; uint64_t record_count; }; // one per chunk, in file order
```
Rules:
- `FrameRec` is 12 bytes. When `module == kInvalidIdx`, `addr_or_rva` holds the absolute address so unmapped frames survive conversion; otherwise it holds the RVA.
- Chunks of the same type may repeat (streaming writers append). Readers concatenate in order; indices are global across repeats of Frames/CallstackNodes/Strings only when the writer guarantees it (writers must; `tracegen` and `etlconvert` do).
- Samples, ContextSwitches, ReadyEvents, Scope/Fiber/Marker/FrameMark/Plot records within a chunk are sorted by `ts`. Across chunks, order is by chunk sequence; readers merge-sort if needed.
- Writers write `FileHeader` with `footer_offset = 0`, then chunks, then rewrite the header at close. Readers handle `footer_offset == 0` by scanning.
- Max record_count per chunk: 1,048,576 (keeps zstd frames around tens of MB).

Public API (`libs/trace/trace_writer.h`, `trace_reader.h`):
```cpp
class TraceWriter { public: static Result<TraceWriter> open(path, FileHeader); StringIdx internString(std::string_view); FrameIdx internFrame(ModuleIdx, uint64_t); CallstackIdx internCallstack(std::span<const FrameIdx> leafToRoot); void append(const SampleBatch&); void append(std::span<const ContextSwitchRec>); /* ... one per chunk type */ Result<void> finish(); };
class TraceReader { public: static Result<TraceReader> open(path); const FileHeader& header(); std::span<const FooterRec> chunks(); Result<void> readChunk(const FooterRec&, std::vector<std::byte>& out); /* typed helpers per chunk type */ };
class CallstackInterner { /* flat std::vector<CallstackNodeRec> nodes; open-addressing hash of (parent,frame)->idx storing only indices; see Part 3.2 */ };
```

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

### 3.3 Public SDK API (`sdk/include/tachyon/tachyon.h`), version 1
C ABI. Names mirror the Performance API so engine integration guides translate directly.
```c
#define TACHYON_API_VERSION 0x00010000
#ifndef TACHYON_ENABLED  /* default 1 on Windows/Linux, 0 elsewhere */ #endif
#define TACHYON_MAKE_COLOR(r,g,b) ((uint32_t)(r)<<24 | (uint32_t)(g)<<16 | (uint32_t)(b)<<8 | 0xFF)
#define TACHYON_DEFAULT_COLOR 0xFFFFFFFFu
void Tachyon_SetCurrentThreadName(const char* utf8);
void Tachyon_SetCurrentThreadName_N(const char* utf8, uint16_t len);
void Tachyon_BeginEvent(const char* id, const char* data, uint32_t color);           /* id must be a static string; data may be dynamic or NULL */
void Tachyon_BeginEvent_N(const char* id, uint16_t idLen, const char* data, uint16_t dataLen, uint32_t color);
TachyonSuppressTailCall Tachyon_EndEvent(void);                                      /* returns 24-byte struct to defeat tail calls */
void Tachyon_RegisterFiber(uint64_t fiberId);
void Tachyon_UnregisterFiber(uint64_t fiberId);
void Tachyon_BeginFiberSwitch(uint64_t currentFiber, uint64_t nextFiber);
void Tachyon_EndFiberSwitch(uint64_t fiberId);
/* Tachyon-specific additions */
void Tachyon_FrameMark(const char* name, uint64_t frameNumber);
void Tachyon_Marker(const char* name, const char* data, uint32_t color);
void Tachyon_Plot(const char* name, double value);
void Tachyon_LockAcquireBegin(const void* lockAddr, const char* lockName);           /* implemented as BeginEvent with data = hex addr, kind flag */
void Tachyon_LockAcquireEnd(const void* lockAddr);
int  Tachyon_IsConnected(void);                                                      /* 1 when an agent is draining */
int  Tachyon_RequestFlush(const char* reason);                                       /* flight recorder trigger; see control protocol */
typedef struct { /* function pointers in the order above */ } TachyonFunctions;
typedef int (*Tachyon_GetAPI_Func)(int version, TachyonFunctions* out);              /* exported as "Tachyon_GetAPI" from tachyon_sdk_shared */
```
C++ wrapper: `tachyon::Scope` RAII type and macros `TACHYON_SCOPE(id)`, `TACHYON_SCOPE_DATA(id,data)`, `TACHYON_SCOPE_COLOR(id,color)`, `TACHYON_FUNCTION()`, `TACHYON_FRAME(name,num)`, `TACHYON_LOCK_SCOPE(lock,name)`. All compile to nothing when `TACHYON_ENABLED == 0`.
Contracts: Begin/End pairs in the same function; `id` pointer identity is used as the interning key (static string requirement); strings UTF-8; maximum `data` length 4096 bytes (truncated); thread-safe; re-entrant from any thread including fibers; no allocation on the hot path after first call per thread.

### 3.4 Agent control protocol (`libs/control/protocol.h`)
Transport: Windows named pipe `\\.\pipe\tachyon-agent`, Linux Unix socket `/run/user/<uid>/tachyon-agent.sock` (fallback `/tmp`). Framing: one JSON object per line, UTF-8, `\n` terminated. Every request has `"id"` echoed in the response.
Requests (client → agent):
```json
{"id":1,"cmd":"status"}
{"id":2,"cmd":"start","mode":"launch|attach|system","exe":"...","args":["..."],"cwd":"...","env":{"K":"V"},"pid":1234,
  "sampling_hz":8000,"follow_children":true,"child_filter":"*.exe","circular_mb":0,"out_path":"...","start_immediately":true,
  "stackwalk":{"profile":true,"cswitch":true,"readythread":true},"kernel_stacks":true}
{"id":3,"cmd":"stop"}
{"id":4,"cmd":"flush","name":"hitch-frame-12345","seconds":10}
{"id":5,"cmd":"cancel"}
{"id":6,"cmd":"inst_hello","pid":1234,"api_version":65536}   // from SDK client; agent replies with shm/pipe details
```
Responses/events (agent → client): `{"id":N,"ok":true,...}` or `{"id":N,"ok":false,"error":"...","code":"ENOPRIV|EBUSY|ENOTFOUND|EINTERNAL"}`; unsolicited events `{"event":"state","state":"idle|starting|capturing|converting|done","progress":0.0-1.0}`, `{"event":"capture_done","path":"...","events_lost":0,"duration_s":31.2}`, `{"event":"error","message":"..."}`.
Instrumentation data path: after `inst_hello`, the agent opens a second pipe `\\.\pipe\tachyon-inst-<pid>`; the SDK streams binary `ScopeEventRec`/`FiberEventRec`/... frames prefixed with `{uint32 type; uint32 byte_len}`; strings are sent once per process as `{type=StringDef, uint64 ptr_key, uint32 len, bytes}` and referenced by `ptr_key` until the agent remaps to `StringIdx` at conversion.

### 3.5 Capture backend interface (`libs/capture/capture_backend.h`)
```cpp
struct CaptureOptions { CaptureMode mode; std::string exe, cwd; std::vector<std::string> args; std::map<std::string,std::string> env; uint32_t pid; uint32_t samplingHz; bool followChildren; std::string childFilter; uint32_t circularMb; std::filesystem::path outPath; bool startImmediately; bool stackProfile, stackCSwitch, stackReady; bool kernelStacks; };
class ICaptureBackend { public:
  virtual Result<void> checkPrivileges() = 0;            // SeSystemProfilePrivilege / CAP_PERFMON
  virtual Result<void> start(const CaptureOptions&) = 0; // returns once the kernel session is live and the target is running
  virtual Result<CaptureResult> stop() = 0;              // stops session, converts, returns {.tachyon path, events_lost, raw path}
  virtual Result<CaptureResult> flush(std::string name, uint32_t seconds) = 0; // circular mode only
  virtual CaptureState state() const = 0;
  virtual void setProgressCallback(std::function<void(CaptureState,float)>) = 0; };
```
Implementations: `WindowsEtwBackend` (WP-11/12), `LinuxPerfBackend` (WP-70), `MockBackend` (WP-10; copies a fixture to `outPath`).

### 3.6 Symbol provider interface (`libs/symbols/symbol_provider.h`)
```cpp
struct ModuleId { uint8_t kind; std::array<uint8_t,32> bytes; uint8_t len; uint32_t age; };   // PDB GUID+age or GNU build-id
struct InlineFrame { StringIdx function; StringIdx file; uint32_t line; };
struct ResolvedAddress { StringIdx function; uint64_t functionStartRva; StringIdx file; uint32_t line; std::vector<InlineFrame> inlines; /* innermost first */ bool resolved; };
class ISymbolProvider { public:
  virtual Result<void> load(const Module&, const std::filesystem::path& symbolFile) = 0;
  virtual ResolvedAddress resolve(uint64_t rva) = 0;                          // thread-safe after load
  virtual Result<std::vector<LineRange>> linesForFunction(uint64_t functionStartRva) = 0;
  virtual Result<FunctionBounds> functionBounds(uint64_t rva) = 0; };
class SymbolLocator { public: Result<std::filesystem::path> locate(const Module&); /* local paths, symstore cache, HTTP servers */ };
class SymbolService { public: SymbolService(std::vector<std::string> servers, std::filesystem::path cacheDir); void resolveAllAsync(TraceModel&, std::function<void(ModuleIdx,SymbolState)> onModule); const ResolvedAddress& lookup(FrameIdx) const; /* memoized */ };
```
Symbol server HTTP layout: `<server>/<pdbfile>/<GUID as 32 upper hex><age hex>/<pdbfile>` (also `file.ptr` redirection and `index2.txt` two-tier). Local cache uses the same two-tier layout under `cacheDir`.

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
*(Orchestrator change, WP-32: queries take `const QueryContext&` (model + SymbolService + options + memo) instead of `const TraceModel&`, with a trailing defaulted `Cancel`. Non-leaf frames resolve at return address − 1. See docs/specs/analysis-query.md.)*

### 3.8 Attribution rules (normative)
1. Sample weight = `sampleIntervalTicks`, clipped so that no sample's weight extends beyond the end of the `Running` interval it falls in. A sample with no enclosing `Running` interval (thread not running per context-switch data) gets weight 0 and is excluded from time totals but kept for stack display.
2. Inclusive time of a function in a stack counts once per sample even if the function appears several times (recursion).
3. Exclusive time attributes to the leaf frame; with inline frames expanded, the innermost inline frame is the leaf.
4. Context switch out at time `t` with `old_thread_stack` S: the interval `[t, next switch-in)` is `Waiting` (or `Ready` if `old_state` says ready/standby) with `blocking_stack = S`. A `ReadyRec` for the thread at `tr` splits the interval into `Waiting [t, tr)` and `Ready [tr, switch-in)` and sets `unblocking_stack` and `woke_by_tid`.
5. Scope blending: for a sample at time `ts` on thread T, the open scopes are those with `begin ≤ ts < end`, ordered by depth. For each open scope (outermost first), find the deepest frame in the sample's stack whose frame address range contains `caller_addr`; insert a synthetic frame with `FunctionKey(kScopeModule, id)` directly above that frame. If none matches, insert directly above the root. Nested scopes maintain relative order.
6. Frame statistics: a frame is `[FrameMark_i, FrameMark_{i+1})` on the marking thread; per-frame totals are computed per thread from Running intervals and from exclusive sample time.

### 3.9 Timeline level-of-detail (`libs/analysis/lod.h`)
For each thread, precompute at load: (a) state bands at levels L0..Lk where level L buckets have width `2^L × 64 ticks`, storing per bucket the fraction of Running/Ready/Waiting and the dominant wait reason; (b) sample "mipmaps": for each level and bucket, the most frequent callstack id and its share, so the collapsed row can draw a representative color; (c) for expanded flame charts, per-depth run-length encoded spans built from consecutive samples sharing the same callstack prefix, merged when narrower than one pixel at the current zoom. Target: render any zoom level of a 10-minute, 64-thread capture in under 4 ms of CPU per frame.

---

## 4. Work packages

Size: S ≈ 1–2 days, M ≈ 3–5 days, L ≈ 1–2 weeks of a focused subagent. "Test platform" says where acceptance tests must run.

### Milestone M0: Foundation

#### WP-00 Repository skeleton and CI (orchestrator)
- Depends on: nothing. Size: S. Test platform: Linux + Windows.
- Deliverables: layout from 2.1; CMake presets; `libs/core` with `result.h`, `strong_id.h`, `platform.h`, `time.h` (QPC / clock_gettime wrappers), `log.h`, `arena.h`, `hash.h` (xxHash wrapper), `span_util.h`; third_party via FetchContent pinned to versions in 2.3; GoogleTest wired; GitHub Actions (or equivalent) for `linux-release`, `linux-asan`, `windows-release`; `.clang-format`; `docs/specs/*.md` generated from Part 3.
- Acceptance: both CI jobs green on an empty test; `cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release` passes.

#### WP-01 ETW spike tool `etlstat` (Windows-only test)
- Depends on: WP-00. Size: M.
- Goal: prove the capture recipe and learn the real event layouts before the agent exists.
- Deliverables: `tools/etlstat`: (1) `etlstat record --out x.etl --seconds N --hz 8000 [--exe path]`: starts a kernel session (`EVENT_TRACE_SYSTEM_LOGGER_MODE`, `ClientContext=1` QPC, flags `PROCESS|THREAD|IMAGE_LOAD|PROFILE|CSWITCH|DISPATCHER`), sets `TraceSampledProfileIntervalInfo` to 1221 (document whether the call needs the session handle or 0), enables stack walking via `TraceStackTracingInfo` for PerfInfo/SampledProfile (type 46), Thread/CSwitch (36), Thread/ReadyThread (50); buffer settings `BufferSize=1024 KB, MinimumBuffers=max(64, 4×cores), MaximumBuffers=…`; stops and reports `EventsLost`, `BuffersLost`. (2) `etlstat stats x.etl`: `OpenTrace`/`ProcessTrace` with `PROCESS_TRACE_MODE_EVENT_RECORD|RAW_TIMESTAMP`, counts per (provider GUID, opcode, version), counts StackWalk events (GUID `def2fe46-7bd6-4b80-bd94-f57fe20d0ce3`, type 32) matched to their parent event by `(EventTimeStamp, StackThread)`, reports unmatched stacks, max stack depth, and whether Thread/SetName (type 72) events appear. (3) `docs/etw-notes.md` recording the exact payload layouts observed for each event/version, the privilege needed (`SeSystemProfilePrivilege`), and the measured lost-event rate on the dev machine at 8 kHz.
- Acceptance: a 30 s recording of `tachyon-testbed` at 8 kHz on a ≥8-core machine shows ≥ 7,500 samples/s per busy core, > 95% of SampledProfile events have matched stacks, and the notes document is complete. Report the numbers.
- Pitfalls: requires admin; only one NT Kernel Logger but up to 8 system loggers on Win8+; `DisablePagingExecutive=1` may be needed for complete kernel stacks; 64-bit user stacks truncate when stack pages are paged out.

#### WP-02 Profiler testbed `tachyon-testbed` (Linux + Windows)
- Depends on: WP-00. Size: M.
- Goal: a workload with known answers used to validate attribution end to end.
- Deliverables: `apps/testbed` with scenarios selectable by flag and a `--duration`: `spin` (function `hot_a` 40%, `hot_b` 20%, `hot_c` 10% of one thread's time, enforced by calibrated loop counts and checked via `rdtsc`), `sleep` (thread sleeping 30%), `lock` (two threads contending one mutex, holder identifiable), `io` (file read loop), `recursion` (depth 50), `inline` (functions marked force-inline inside a hot loop), `fibers` (Windows fibers / ucontext on Linux, 4 fibers round-robin on one thread), `jobs` (8 worker threads with a condition variable), `frames` (simulated 60 Hz frame loop with occasional 50 ms hitch every 2 s). Threads named via `SetThreadDescription` / `pthread_setname_np`. Every scenario prints its expected split as JSON to stdout at exit for the test harness.
- Later (after WP-50) the testbed gains `--instrument` to emit Tachyon scopes and frame marks around each scenario.
- Acceptance: builds on both platforms; `--scenario all --duration 5` runs to completion; expected-split JSON validated by a unit test on the calibration logic.

#### WP-03 Trace format library `libs/trace` (Linux)
- Depends on: WP-00. Spec: 3.1. Size: L.
- Deliverables: `TraceWriter`, `TraceReader`, `StringTable`, `CallstackInterner` (flat node vector; open-addressing hash storing node indices only, keyed by hashing `(parent, frame)` and verified against the node array; load factor ≤ 0.7; grows by doubling), zstd chunk compression, footer, header rewrite on finish, partial-file recovery (flag bit2), per-type typed append/read helpers, record `static_assert`s, `docs/specs/trace-format.md` cross-checked.
- Tests: round-trip every chunk type; 10 M random callstacks intern/dedup correctness vs. a `std::map` oracle on 100 k; partial-file read; chunk splitting at 1,048,576 records; perf test: intern 20 M stacks of depth 30 with 4% unique in < 10 s and ≤ 16 bytes/node resident (nodes + hash).
- Acceptance: all tests pass; perf budget met; API matches 3.1 exactly.

#### WP-04 Synthetic trace generator `tracegen` (Linux)
- Depends on: WP-03. Size: M.
- Goal: deterministic fixtures with known answers for analysis and UI tests, no Windows needed.
- Deliverables: `tools/tracegen` driven by a JSON scenario: threads, modules with fake function tables (a sidecar `.symbols.json` giving function ranges, lines, inlines, used by a `JsonSymbolProvider` in WP-20), synthetic callstacks with specified time shares, running/waiting intervals, ready events with wakers, scopes with caller addresses, fibers, frame marks, plots; seeds for reproducibility. Emits `fixture.tachyon` + `fixture.expected.json` (expected inclusive/exclusive per function, state totals per thread, scope stats). Generate and commit (LFS) fixtures: `tiny` (2 threads, 1 s), `medium` (16 threads, 60 s, 8 kHz ≈ 7.7 M samples), `large` (64 threads, 600 s; generated in CI, not committed), `scopes`, `fibers`, `frames`, `recursion`, `partial`.
- Acceptance: fixtures reproducible byte-for-byte from seed; expected JSON agrees with a brute-force reference computed inside tracegen.

### Milestone M1: Windows capture

#### WP-10 Agent skeleton and control protocol (Linux for protocol tests; Windows for pipe)
- Depends on: WP-00, WP-03. Spec: 3.4, 3.5. Size: M.
- Deliverables: `libs/control` (framing, JSON schema validation, client and server, pipe/socket transports), `apps/agent` main loop (single capture at a time, state machine idle→starting→capturing→converting→done, unsolicited events), `MockBackend`, `--backend mock|etw|perf`, logging, graceful shutdown, elevation check on startup (Windows: `IsUserAnAdmin`/token elevation; Linux: `CAP_PERFMON` or root). `tachyon-cli status|start|stop|flush` thin client (full CLI in WP-13).
- Tests: protocol round-trips, malformed input, concurrent clients, state machine, mock capture produces the fixture at `out_path`.
- Acceptance: `tachyon-cli start --mode system --out x.tachyon` against `--backend mock` yields the fixture and a `capture_done` event.

#### WP-11 ETW session controller (Windows-only test)
- Depends on: WP-01, WP-10. Spec: 3.5. Size: L.
- Deliverables: `libs/capture/windows_etw/etw_session.{h,cpp}`: start/stop/query, privilege enabling (`AdjustTokenPrivileges` for `SeSystemProfilePrivilege`), orphan session cleanup on startup (stop any prior `Tachyon*` logger), flag set from `CaptureOptions`, stack-walk configuration, sampling interval, buffer sizing formula with override, sequential file mode and circular mode (`EVENT_TRACE_FILE_MODE_CIRCULAR` with `MaximumFileSize = circular_mb`), `flush` implemented as `ControlTrace(EVENT_TRACE_CONTROL_FLUSH)` + stop + restart with a new file (document the gap), `EventsLost` reporting. `WindowsEtwBackend` wiring start/stop/flush, calling WP-12's converter in `stop`.
- Tests (Windows): start/stop produces an ETL; `EventsLost == 0` for a 30 s testbed run; circular mode file size bounded; restart after a crashed previous agent works.
- Pitfalls: `EVENT_TRACE_PROPERTIES` must be followed by space for logger and file names; use `EVENT_TRACE_PROPERTIES_V2` if filtering is needed; ETW requires the session name to be unique.

#### WP-12 ETL to `.tachyon` converter `libs/etlconvert` (Windows-only test, with a Linux-compilable core)
- Depends on: WP-01, WP-03. Spec: 3.1. Size: L.
- Deliverables: `EtlConverter::convert(etlPath, outPath, targetPids, options)`: `ProcessTrace` consumer; parsers for Process (1/2/3/4), Thread (1/2/3/4, 72 SetName), Image (10/2/3/4), PerfInfo SampledProfile (46), Thread CSwitch (36), ReadyThread (50), StackWalk (32) and stack-key compressed variants (34–37, for externally recorded ETLs; may be `partial` with a clear error); stack matching by `(timestamp, thread)` with a bounded pending queue; kernel/user stack concatenation; module identification by reading the PE debug directory (`IMAGE_DEBUG_TYPE_CODEVIEW` RSDS → GUID, age, pdb path) from the image path at conversion time, with fallback to `TimeDateStamp+SizeOfImage` when the file is missing; address→module mapping per process using an interval map; callstack interning (leaf to root) via WP-03; per-chunk sorted output; `CaptureInfo` JSON with `sampling_hz`, `events_lost`, host info; progress callback; target-process filter (keep all CSwitch/Ready events that touch a target thread plus the readying thread's stack even if in another process; keep all samples for target threads; optionally keep system-wide samples when `mode == system`).
- Separate the event-record decoding into header-only decoders (`etl_events.h`) with unit tests on byte arrays so the Linux build compiles and tests them; only the `ProcessTrace` driver is Windows-only.
- Tests: decoder tests from captured byte arrays (collected in WP-01 and committed as hex fixtures); Windows: convert the WP-01 ETL of the testbed and check sample counts match `etlstat`, every target thread has Running intervals, and `hot_a/hot_b/hot_c` RVAs rank in that order by sample count.
- Perf budget: convert a 2 GB ETL in < 90 s on 8 cores (parallelize parse vs. intern with a bounded queue).

#### WP-13 Launch, attach, process tree, and CLI (Windows-only test for launch)
- Depends on: WP-11, WP-12. Size: M.
- Deliverables: in `WindowsEtwBackend`: launch with `CREATE_SUSPENDED`, start session, `ResumeThread`, so static initialization is captured; `start_immediately=false` path launches first and starts the session on `start`; attach by PID; child-process following by parsing Process start events whose parent is a target and matching `child_filter` glob; environment injection (`TACHYON_AGENT_PIPE`, so the SDK can connect); `apps/cli` full command set with exit codes, `--json` output, `capture` convenience command = start + wait for duration or process exit + stop.
- Acceptance (Windows): `tachyon-cli capture --launch tachyon-testbed.exe --args "--scenario spin --duration 20" --out spin.tachyon` produces a trace whose first sample is before the testbed's `main` (verify a sample in `ntdll` loader code exists for the target PID).

#### WP-14 Capture validation on the engine (Windows, user-assisted)
- Depends on: WP-13. Size: S.
- Deliverables: `docs/capture-tuning.md` with measured `EventsLost` vs. buffer settings on the engine at 8 kHz across 1, 5, and 10 minute captures; chosen defaults baked into WP-11; a `tachyon-cli doctor` command checking privileges, orphan sessions, disk space, and symbol-store reachability.
- Acceptance: zero lost events for a 5-minute engine capture with default settings, or a documented reason and the UI warning path (WP-48) exercised.

### Milestone M2: Symbols

#### WP-20 Symbol core: interface, range tables, JSON provider, cache (Linux)
- Depends on: WP-03, WP-04. Spec: 3.6. Size: M.
- Deliverables: `ISymbolProvider`, `SymbolService` with a per-module worker pool, memoized `FrameIdx → ResolvedAddress`, function range table (`std::vector<FunctionBounds>` sorted, binary search), `JsonSymbolProvider` reading tracegen's `.symbols.json`, `FunctionKey` derivation, demangler interface (`demangleMsvc`, `demangleItanium`) with a trivial pass-through default, progress reporting per module, symbol state enum (Pending/Loading/Resolved/NotFound/Failed).
- Tests: resolve all frames of the `medium` fixture; inline chains; unknown module → `resolved=false`; concurrency test with 8 threads resolving while loading.

#### WP-21 Symbol locator and HTTP symbol server client (Linux + Windows)
- Depends on: WP-20. Size: M.
- Deliverables: `SymbolLocator`: search order = explicit paths → next to the module → local two-tier cache → HTTP servers in order; PDB key `GUID(32 upper hex)+age(hex)`; ELF key `build-id` (`buildid/<hex>/debuginfo` debuginfod layout as a second scheme); `file.ptr` and `index2.txt` handling; `_` compressed files (`.pd_`) decompression via `expand`-style LZX/MSZIP is out of scope: report NotFound with reason; WinHTTP on Windows, libcurl on Linux behind `IHttpClient`; retries, timeouts, concurrent downloads bounded; cache eviction by size.
- Tests: mock HTTP server in tests (tiny embedded server or recorded responses) covering hit, miss, `file.ptr`, two-tier; Windows: fetch a Microsoft symbol (`ntdll.pdb`) from `https://msdl.microsoft.com/download/symbols` when network is available (skip otherwise).

#### WP-22 DIA symbol provider (Windows-only test)
- Depends on: WP-20. Size: L.
- Deliverables: `libs/symbols/dia/dia_provider.cpp`: load `msdia140.dll` without COM registration (`DllGetClassObject` → `IClassFactory` → `IDiaDataSource`), `loadAndValidateDataFromPdb` with GUID/age, functions via `findChildren(SymTagFunction)` plus public symbols fallback, inline frames via `findInlineFramesByRVA`, lines via `findLinesByRVA`, function bounds, MSVC demangling via `UnDecorateSymbolName`, thread safety via one session per thread or a mutex (document choice), error mapping (mismatched PDB, missing file).
- Tests (Windows): resolve testbed frames: `hot_a` etc. resolve to correct file/line; the `inline` scenario yields inline chains; perf: resolve 1 M distinct RVAs across the testbed and system DLLs in < 20 s with the cache warm.

### Milestone M3: Analysis engine

#### WP-30 Model loader `libs/model` (Linux)
- Depends on: WP-03, WP-04. Spec: 3.2. Size: M.
- Deliverables: `TraceModel::load(path, progress)`: read chunks in parallel, build `tidToThread`, split samples per thread (counting sort), verify/sort by ts, build frame and node arrays, strings, `sampleIntervalTicks` from CaptureInfo, memory accounting, partial-file tolerance.
- Tests: load all fixtures; counts match expected JSON; perf: load `medium` in < 2 s, `large` in < 30 s on 8 cores, resident ≤ 3× compressed size.

#### WP-31 Thread state intervals (Linux)
- Depends on: WP-30. Spec: 3.8 rules 4. Size: M.
- Deliverables: `buildThreadStates(TraceModel&)`: from ContextSwitch and Ready records produce per-thread sorted `ThreadStateInterval`s with Running (cpu), Ready, Waiting (wait reason, blocking stack), unblocking stack and waker, ready latency; handle missing start (thread already running at trace start → `Unknown` until first event), trace end, and lost-event gaps (mark `Unknown` when a switch-in has no matching switch-out); wait-reason name table for Windows `KWAIT_REASON` values and Linux `prev_state` bits.
- Tests: tracegen fixtures with exact expected state totals; edge cases hand-built.

#### WP-32 Attribution and aggregation (Linux)
- Depends on: WP-31, WP-20. Spec: 3.7, 3.8 rules 1–3. Size: L.
- Deliverables: sample weight clipping; `topDownTree` (merge by `FunctionKey` per depth, counting recursion once), `bottomUpFunctions`, `butterfly`, inline expansion toggle, kernel frame handling, unresolved frames grouped per module as `<module>!0x<rva>`.
- Tests: fixtures' expected inclusive/exclusive within 0.1%; recursion fixture counts once; butterfly callers sum to self inclusive.
- Perf: full-range `bottomUpFunctions` on `medium` < 300 ms; on `large` < 3 s; cached per filter.

#### WP-33 Filters, cancellation, and query cache (Linux)
- Depends on: WP-32. Size: M.
- Deliverables: time-range filtering via binary search per thread, thread filter, subtree filter (stack must contain node prefix), function filter; `QueryCache` keyed by `(query, filter hash)` with LRU; cooperative cancellation checked every 64 k samples; a `QueryExecutor` thread pool with priorities (UI foreground > background prefetch).
- Tests: filtered results equal brute-force on `tiny`; cancellation returns within 50 ms; cache hit/miss accounting.

#### WP-34 Line and instruction attribution plus disassembly (Linux for logic; Windows for real PDBs)
- Depends on: WP-32, WP-22 (Windows). Size: M.
- Deliverables: `linesForFunction` with running/ready/waiting split (via thread state at sample time), `instructionsForFunction`, `libs/disasm` Zydis wrapper (decode a byte range from the module file on disk at `rva`, formatted Intel syntax, with symbolized call targets), source file locator (path remapping rules in settings, `srcsrv`-style not required).
- Tests: JSON provider fixtures with lines; Zydis decode of a known byte sequence; Windows: disassemble `hot_a` from the testbed and ensure sample hits land inside its bounds.

#### WP-35 Function and scope statistics, frames (Linux)
- Depends on: WP-32. Size: M.
- Deliverables: per-instance durations for sampled functions (an instance = maximal run of consecutive samples on a thread containing the function at the same depth with no gap > 2 sample intervals; documented as an approximation) and exact instance durations for scopes; `Stats` with histogram (log-scaled buckets) and box-plot quartiles; `frames()` from FrameMarks with per-frame per-thread Running time and top-N exclusive functions; slowest-frame lookup.
- Tests: scope stats exact on `scopes` fixture; frame list exact on `frames` fixture.

#### WP-36 Timeline LOD (Linux)
- Depends on: WP-31. Spec: 3.9. Size: L.
- Deliverables: `lod::build(TraceModel&)` producing state bands, sample mipmaps, and per-depth span RLE per thread; a `TimelineQuery` returning spans for `(thread, depth range, t0, t1, pixelWidth)` with merging below one pixel; incremental build on a worker with progress so the viewer opens before LOD completes.
- Tests: spans cover `[t0,t1)` without overlap; mipmap dominant stack equals brute-force on `tiny`; perf: `large` query at any zoom < 4 ms; build < 15 s.

### Milestone M4: Viewer

#### WP-40 Viewer shell (Linux + Windows)
- Depends on: WP-30. Size: L.
- Deliverables: `apps/viewer`: SDL2 + OpenGL 3.3 + ImGui docking; main loop capped at 60 fps and idle-throttled; `App` with `Document` (one open trace: model, symbol service, query executor, LOD), `SelectionBus` (current time filter, selected thread, selected FunctionKey, hovered item; views subscribe), `Settings` (JSON in user config dir: symbol servers, cache dir, source path mappings, layout), async job display (spinner/progress in status bar), file open dialog (native via SDL or a minimal ImGui browser), command palette stub, theme (dark default, light option), DPI scaling, layout persistence via `imgui.ini`.
- Tests: headless mode `--selftest` that opens a fixture, runs 100 frames with a null renderer, and exits 0; unit tests for `SelectionBus` and settings.

#### WP-41 Session explorer and capture dialogs (Linux with mock backend; Windows real)
- Depends on: WP-40, WP-10. Size: M.
- Deliverables: Session Explorer (recent traces with metadata from CaptureInfo, open/delete/reveal), Run dialog (exe, args, cwd, env, start immediately, sampling rate, circular MB, follow children), Attach dialog (process list with filter), agent connection status and auto-launch of the agent with elevation prompt (`ShellExecuteEx runas` on Windows; `pkexec` on Linux), live capture state and stop button, auto-open on `capture_done`.
- Acceptance: with `--backend mock` the full Run→capture→open flow works on Linux.

#### WP-42 Threads timeline (Linux)
- Depends on: WP-40, WP-36. Size: L.
- Deliverables: a custom ImGui widget: time ruler with adaptive units; per-thread rows sorted by Running time (configurable), collapsed row = state band (green Running, red synchronization wait, dark blue I/O, light blue paging/memory, grey other, amber Ready) with sample-mipmap colored strip; expand to flame chart of sampled stacks (colors hashed by module or function), with inline frames optional; pan (drag/middle), zoom (wheel at cursor), fit, keyboard navigation; drag-select a time range and `F` to apply as filter, `Esc` to clear; hover tooltip with time, state, wait reason, top function; click to set selected function; frame marks as vertical ticks with frame numbers at the top (data from WP-35 when present); overview strip across the whole trace with viewport indicator; thread pin/hide; minimum render work bounded by WP-36 queries.
- Acceptance: 60 fps on `large` on a mid-range GPU with 64 rows at every zoom level; selection and filter propagate via `SelectionBus`.

#### WP-43 Call graph panel (Linux)
- Depends on: WP-40, WP-33. Size: M.
- Deliverables: tree table with columns Function, Module, Inclusive, Exclusive, %, Samples; expand hot path (expand the heaviest child chain), expand all/collapse, pie chart of child time for the selected node (ImGui draw list), right-click: filter to subtree, show in Function List, show source, copy name; double-click navigates source; follows `SelectionBus` filter; cancellable background queries with stale-result guard.

#### WP-44 Function list and butterfly (Linux)
- Depends on: WP-43. Size: M.
- Deliverables: bottom-up table sortable by inclusive/exclusive/instances with text filter, "restrict to call graph subtree" toggle; Butterfly panel showing callers above and callees below the selected function with percentages and click-to-navigate; module grouping toggle.

#### WP-45 Source and disassembly panel (Linux + Windows)
- Depends on: WP-34, WP-43. Size: L.
- Deliverables: source view with per-line time bar (total) and a stacked running/ready/waiting mini-bar, heat coloring, jump to hottest line, line gutter with instruction expansion (mixed mode) showing per-instruction time, pure disassembly fallback when the source file is missing with a "locate file" action that updates path mappings, monospace font, horizontal scroll, search within file.

#### WP-46 Thread interaction (Linux)
- Depends on: WP-42, WP-31. Size: M.
- Deliverables: when hovering a Waiting/Ready interval, draw an arrow from the readying thread's `ReadyRec` position to the switch-in; click opens the Thread Interaction panel with blocking stack (left), unblocking stack (right), waker thread, wait reason, wait duration, ready latency; Ctrl-hover compact tooltip with both stacks (top 8 frames); navigation buttons to jump to the waker's timeline position; list of the N longest waits in the filter range with their wakers.

#### WP-47 Search and Function Statistics (Linux)
- Depends on: WP-35, WP-42. Size: M.
- Deliverables: Ctrl+F opens the Function Statistics panel with a fuzzy search over functions and scope ids; selecting an entry shows count/min/median/p75/max, histogram, box plot, and highlights every instance in the timeline (overlay) with next/previous instance navigation; "populate from Call Graph/Function List selection" action.

#### WP-48 Status, warnings, and symbol progress (Linux)
- Depends on: WP-40, WP-20. Size: S.
- Deliverables: status bar with symbol resolution progress per module (hover for list), lost-events warning banner from CaptureInfo, unresolved-module list with "add symbol path" action, error toasts, keyboard shortcut cheat sheet (`?`).

### Milestone M5: Instrumentation SDK and engine integration

#### WP-50 Public SDK header and loader (Linux + Windows)
- Depends on: WP-00. Spec: 3.3. Size: M.
- Deliverables: `sdk/include/tachyon/tachyon.h` (C API, C++ RAII wrapper, macros, `TACHYON_ENABLED` compile-out), `tachyon_loader.h` (`Tachyon_LoadFrom(path, &functions)` via `LoadLibrary`/`dlopen`, version check, zeroed table on failure), `tachyon_sdk_shared` exporting `Tachyon_GetAPI`, `tachyon_sdk` static with the same symbols, a `TACHYON_NOINLINE` begin implementation that records `_ReturnAddress()`/`__builtin_return_address(0)`, End returning the 24-byte struct, header compiles as C99 and C++17, doc comments mirroring the Performance API's contracts.
- Tests: compile the header in C and C++ with `TACHYON_ENABLED` 0 and 1; loader against the shared lib; unit test that the recorded caller address lies within the test function's bounds (compare against a label address range).

#### WP-51 Client runtime (Linux + Windows)
- Depends on: WP-50, WP-10. Spec: 3.3, 3.4. Size: L.
- Deliverables: `sdk/src`: per-thread SPSC ring (64 KB default, configurable) allocated lazily with TLS registration in a global thread list; record layouts matching 3.1 ScopeEvent/Fiber/FrameMark/Marker/Plot with `id` encoded as the string pointer (`ptr_key`) and a once-per-process `StringDef` emission guarded by a lock-free set; `data` copied into the ring with length prefix (truncate at 4096); timestamps via QPC/`clock_gettime(CLOCK_MONOTONIC_RAW)`; a flush thread draining rings at 1 ms cadence to the agent pipe (connect via `TACHYON_AGENT_PIPE` env or default name; `inst_hello` handshake) or to the sidecar file; overflow policy: drop with a per-thread counter and emit a `Marker("tachyon.dropped", count)`; shutdown flush via `atexit`; thread name capture; `Tachyon_RequestFlush` forwarding to the agent `flush` command.
- Tests: multi-thread stress (16 threads × 1 M scopes) with no data corruption (sequence numbers verified), overhead benchmark: Begin+End < 40 ns median on the CI machine when connected, < 10 ns when disabled at runtime; sidecar file parses into valid records.

#### WP-52 Agent-side ingestion and merge (Linux with mock; Windows real)
- Depends on: WP-51, WP-12. Size: M.
- Deliverables: agent accepts `inst_hello`, opens the per-process data pipe, spools binary records to a temp file during capture, and at conversion remaps `ptr_key → StringIdx`, converts timestamps (same clock, so identity on Windows/Linux; keep a `ClockCalibration` hook), and appends Scope/Fiber/FrameMark/Marker/Plot chunks to the `.tachyon`; sidecar merge command `tachyon-cli merge trace.tachyon inst.tachyon-inst`.
- Tests: mock backend + testbed `--instrument` on Linux produces a trace with scopes; ordering and pairing validated.

#### WP-53 Analysis: scope pairing, blending, fibers (Linux)
- Depends on: WP-52 (fixtures from WP-04 `scopes`/`fibers` suffice), WP-32. Spec: 3.8 rule 5. Size: L.
- Deliverables: pair Begin/End per thread into `ScopeInstance`s with depth (unterminated scopes end at trace end, flagged); blending into `topDownTree`/`bottomUpFunctions`/`butterfly` via synthetic frames placed by `caller_addr` lookup against function bounds (requires symbols; fallback above root); fiber-aware thread view: build per-fiber scope lanes by mapping scope events to the fiber active on that thread (from FiberEvents) and offer "group by fiber" in queries; lock scopes produce a `Lock` summary (contention count, total wait per lock address).
- Tests: `scopes` fixture: blended tree places scope above the correct frame; `fibers` fixture: scopes spanning a fiber switch split correctly.

#### WP-54 Viewer: instrumentation lanes, frame graph, plots (Linux)
- Depends on: WP-53, WP-42. Size: L.
- Deliverables: scope lanes under each thread row (colored by scope color, label with id and data, nested depth), toggle between sampled flame chart and scope lanes or both; frame-time graph strip (bar per frame, budget line, click to zoom to frame, "jump to slowest frame in filter"); plot lanes (line graphs per plot name); scopes appear in Call Graph/Function List with a distinct icon; Function Statistics works on scope ids; fiber grouping toggle.

#### WP-55 Engine integration guide and samples (docs)
- Depends on: WP-50, WP-51. Size: M.
- Deliverables: `docs/engine-integration.md` and `sdk/examples/`: how to vendor the header, link or dynamically load, build flags (`/Zi /Oy- /DEBUG:FULL`, `-g -fno-omit-frame-pointer`), PDB retention and `symstore add` in CI, build-id embedding, thread naming at thread start (both OS API and Tachyon), job system hooks (scope per job with job name as id and job instance data, worker wake-up markers), fiber scheduler hooks (register at creation, Begin/EndFiberSwitch around the switch), lock wrapper (`TACHYON_LOCK_SCOPE`), frame marks on the main and render threads, streaming/I/O scopes with asset path data, hitch trigger (`if frameMs > budget: Tachyon_RequestFlush("hitch")`), console commands to start/stop/flush via the control protocol, GPU timestamp calibration record, recommended default colors per subsystem, overhead expectations, and a checklist. Example code must compile against the SDK in CI as `sdk/examples/engine_hooks_example.cpp` with a fake job system.

### Milestone M6: Scale, robustness, automation

#### WP-60 Flight recorder mode end to end (Windows)
- Depends on: WP-11, WP-52, WP-41. Size: M.
- Deliverables: circular session kept running across many flushes; `flush` produces `<name>.tachyon` containing the last `seconds` (post-filter by timestamp during conversion), including instrumentation from the client's spool (bounded ring on disk); viewer "Flight recorder" panel showing the agent is armed and listing produced captures; engine trigger documented in WP-55 exercised with the testbed `frames` scenario.
- Acceptance: testbed hitch triggers produce a trace where the hitch frame is visible within the last 10 s.

#### WP-61 Remote capture (Linux + Windows)
- Depends on: WP-10, WP-41. Size: M.
- Deliverables: TCP transport for the control protocol (`--listen 0.0.0.0:7331`, token auth from a config file), file transfer command `fetch` streaming the `.tachyon` with resume, viewer "Connect to remote agent" dialog, symbol resolution happens on the viewer side (modules identified by GUID/age so remote paths are irrelevant).

#### WP-62 CI report tool (Linux)
- Depends on: WP-35, WP-53. Size: M.
- Deliverables: `tachyon-report trace.tachyon --baseline base.json --out report.json|md`: frame-time percentiles (p50/p95/p99/max), per-thread Running totals, top-N functions by exclusive, per-scope stats, lost events; baseline diff with configurable thresholds and non-zero exit on regression; Markdown summary suitable for a PR comment.

#### WP-63 Exporters (Linux)
- Depends on: WP-30, WP-53. Size: S.
- Deliverables: speedscope JSON (sampled profile per thread), Chrome trace event JSON (scopes, frame marks, thread names, context switches as `ph:"X"` on a per-CPU track) loadable in Perfetto; `tachyon-cli export --format speedscope|chrome`.

#### WP-64 Large-trace performance pass (Linux)
- Depends on: all M3/M4. Size: L.
- Deliverables: profile the viewer with its own agent (dogfooding) on `large` and a real 10-minute engine capture; fix the top offenders until budgets in 3.2, 3.9, WP-30, WP-32 hold; streaming load (open the viewer with partial data while later chunks load); memory ceiling setting with graceful degradation (drop sample mipmap levels first).

#### WP-65 Real-time live view (optional, Windows)
- Depends on: WP-11, WP-42. Size: L.
- Deliverables: ETW real-time mode consumer feeding an incremental converter and model; viewer "Live" mode showing the last 30 s scrolling. Only start if M6 core items are done.

### Milestone M7: Linux backend

#### WP-70 `perf_event_open` sampling backend (Linux, privileged runner)
- Depends on: WP-10, WP-03. Spec: 3.5. Size: L.
- Deliverables: `LinuxPerfBackend`: per-CPU `perf_event_open` (system-wide when `CAP_PERFMON`, else per-pid with `inherit`) with `PERF_TYPE_SOFTWARE/PERF_COUNT_SW_CPU_CLOCK`, `freq=1, sample_freq=samplingHz`, `sample_type = IP|TID|TIME|CALLCHAIN|CPU`, `use_clockid=1, clockid=CLOCK_MONOTONIC_RAW`, `mmap=1, comm=1, task=1, mmap2=1, exclude_callchain_kernel` configurable; mmap ring readers per CPU with a drain thread; `PERF_RECORD_THROTTLE` detection and `kernel.perf_event_max_sample_rate` / `perf_cpu_time_max_percent` guidance in errors; conversion to `.tachyon` with module identification by GNU build-id (`.note.gnu.build-id` from the mapped file) and address→module via MMAP2 records; launch via `fork/exec` with a stop-on-start pipe so the first instruction is captured.
- Tests: on a privileged runner, capture the testbed `spin` scenario and verify the `hot_a>hot_b>hot_c` ordering; unit tests for record parsing from byte fixtures.

#### WP-71 Scheduler events (Linux, privileged runner)
- Depends on: WP-70. Size: M.
- Deliverables: tracepoint events `sched:sched_switch` (prev_state → wait reason mapping: S interruptible, D uninterruptible/I/O, R preempted → Ready) and `sched:sched_wakeup`/`sched_waking` (waker = current task, callchain of waker = unblocking stack) via `PERF_TYPE_TRACEPOINT` with `PERF_SAMPLE_RAW|CALLCHAIN|TIME|TID|CPU`; alternatively `attr.context_switch=1` records as a fallback without wait reasons when tracepoints are unavailable; conversion into `ContextSwitchRec`/`ReadyRec`.
- Tests: testbed `lock` scenario shows the holder as waker of the waiter.

#### WP-72 DWARF symbol provider (Linux)
- Depends on: WP-20. Size: L.
- Deliverables: `libs/symbols/dwarf/dwarf_provider.cpp` with elfutils `libdw`: functions (`dwfl_module_addrinfo` or DIE walk), inlines (`dwarf_getscopes` / `DW_TAG_inlined_subroutine`), lines (`dwarf_getsrc_die`), `.gnu_debuglink`/debuginfod lookup via WP-21, Itanium demangling via `abi::__cxa_demangle`, split DWARF not required.
- Tests: resolve the testbed built with `-g`; inline scenario produces chains.

#### WP-73 Linux SDK transport and privileges (Linux)
- Depends on: WP-51, WP-70. Size: S.
- Deliverables: Unix socket transport verified against the Linux agent; documentation of `CAP_PERFMON` setup (`setcap cap_perfmon,cap_sys_ptrace+ep tachyon-agent`) and `perf_event_paranoid` values; `tachyon-cli doctor` for Linux.

---

## 5. Milestones, gates, and schedule

| Milestone | Work packages | Gate checklist (orchestrator runs before next milestone) |
|---|---|---|
| M0 Foundation | 00, 01, 02, 03, 04 | CI green on both OSes; `etlstat` numbers reported; fixtures committed; trace round-trip perf budget met. |
| M1 Windows capture | 10, 11, 12, 13, 14 | `tachyon-cli capture` of the testbed and of the engine yields `.tachyon` with 0 lost events; conversion within budget; a trace opens in `TraceReader`. |
| M2 Symbols | 20, 21, 22 | Testbed trace fully symbolized including inlines; symbol-server fetch works against the team's store. |
| M3 Analysis | 30–36 | All fixture expectations met; perf budgets met on `large`; API in 3.7 unchanged or spec updated. |
| M4 Viewer | 40–48 | An engine programmer finds a planted hitch in a testbed `frames` capture unaided in under 10 minutes; 60 fps on `large`. |
| M5 SDK | 50–55 | Engine integrated (user-side) with job/fiber/lock/frame hooks; overhead under 1% at 100 k scopes/frame with agent connected; scopes blended correctly in Call Graph. |
| M6 Scale | 60–64 (65 optional) | Flight recorder catches a planted hitch; CI report runs in the engine's pipeline; 10-minute engine capture loads under budget. |
| M7 Linux | 70–73 | Testbed capture on Linux shows correct attribution and lock waker. |

Approximate calendar with one orchestrator and up to three concurrent subagent lanes: M0 2 weeks, M1 3 weeks, M2 2 weeks (parallel with M1), M3 4 weeks (parallel with M1/M2 after fixtures), M4 6 weeks, M5 3 weeks (header and runtime can start during M3), M6 4 weeks, M7 4 weeks. Critical path: WP-03 → WP-04 → WP-30 → WP-31 → WP-36 → WP-42 → WP-46 → WP-54 → WP-64.

Dependency graph (arrows = depends on):
```
00 → 01,02,03,10,50
03 → 04,12,30,70        04 → 20,30
01 → 11,12              10 → 11,13,41,51,61,70     11,12 → 13 → 14
20 → 21,22,32,48,72     22 → 34(win)
30 → 31,40 → 32 → 33,34,35,53 → 43 → 44,45 ; 31 → 36 → 42 → 46,47,54
50 → 51 → 52,55 ; 52 → 53 (fixtures suffice) ; 53 → 54,62,63
11,52,41 → 60 ; 10,41 → 61 ; M3+M4 → 64
70 → 71,73 ; 20 → 72
```

---

## 6. Testing strategy
- **Unit tests** per library on Linux; Windows-only tests under `tests/windows/` run on the Windows CI runner (self-hosted or a Windows Server GitHub runner; verify ETW works there in WP-01, otherwise a dedicated dev box).
- **Fixture tests** from `tracegen` with expected JSON: the primary vehicle for analysis and UI logic. Fixtures in LFS; `large` generated on demand in CI.
- **Known-answer tests** from the testbed on real captures (Windows now, Linux in M7) compare measured splits to the testbed's printed expectations within tolerance (±5% for 20 s runs).
- **Perf budget tests** in `tests/perf/` with hard thresholds (fail at 2× budget) for interning, load, queries, LOD, SDK overhead.
- **Viewer self-test** (`--selftest`) renders fixtures headless to catch crashes; manual UX gate at M4.
- **Dogfooding** from M4 onward: profile the viewer and the agent with Tachyon itself; file findings as WPs.

---

## 7. Open questions for the user (proceed under assumptions until answered)
1. Platform order: Windows first, Linux second, consoles only with SDK access. Correct?
2. Engine language and compiler: C++ with MSVC 2022 on Windows and Clang on Linux?
3. Does the engine already have a profiling macro layer (e.g. its own `PROFILE_SCOPE`)? If so, WP-55 should map to it rather than introduce new macros.
4. Job system and fiber details: fiber IDs available as 64-bit values at switch time? Job names static strings?
5. Symbol store: do you have one (symstore share or HTTP)? URL and auth model.
6. Where does the repo live, and should the SDK header be mirrored into the engine repo or consumed as a submodule?
7. Is a 60 fps viewer on integrated graphics required, or are dev machines all discrete-GPU?
8. Codename and binary names.

---

## 8. Appendix A: Superluminal research digest (context only)
- Sampling profiler; 8 kHz default on Windows (ETW minimum interval 122.1 µs), up to 40 kHz on PlayStation; Linux beta via eBPF with `.eh_frame` unwinding and BPF ring buffers (founder blog posts document kernel bugs hit); Xbox/PlayStation via first-party APIs.
- Captures from launch (static init, DLL loads), system-wide including kernel and third-party code; symbol resolution after capture with symbol servers; import of external ETLs (UIForETW recommended with context-switch callstacks, compression, fast sampling).
- Performance API v3: C ABI, Begin/End with stable id + dynamic data + stable color, `_N` and `_Wide` variants, thread naming, fiber register/switch, tail-call-suppressing EndEvent, DLL function table with exact version check, compiles out when disabled. BSD-licensed headers.
- UI: Threads timeline with state-colored summary bands (green executing, other colors waits) and sample-driven flame charts; thread interaction arrows and blocking/unblocking stacks with ready latency; time filter (`F`) driving every view; Call Graph with pie chart and hot path; Function List bottom-up; butterfly; Function Statistics with histogram and box plot doubling as search; source and mixed-mode disassembly with per-line/per-instruction timing and thread state; Session Explorer; CLI capture for remote machines.
- Scale: "Compressing callstacks" post: 21 M callstacks from a 2.64 GiB ETL deduplicated 96%, stored as a flat parent-index DAG with 16-byte nodes interned via a keyless hash map. Adopted here as `CallstackInterner`.

## Appendix B: ETW reference for WP-01/11/12
- Kernel provider GUIDs: Process `3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c`; Thread `3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c`; Image `2cb15d1d-5fc1-11d2-abe1-00a0c911f518`; PerfInfo `ce1dbfb4-137e-4da6-87b0-3f59aa102cbc`; StackWalk `def2fe46-7bd6-4b80-bd94-f57fe20d0ce3`.
- Opcodes: Process Start 1/End 2/DCStart 3/DCEnd 4; Thread Start 1/End 2/DCStart 3/DCEnd 4/CSwitch 36/ReadyThread 50/SetName 72; Image Load 10/Unload 2/DCStart 3/DCEnd 4; PerfInfo SampledProfile 46; StackWalk Stack 32, StackKeyKernel 34, StackKeyUser 35, StackKeyRundown 36, StackKeyDelete 37 (verify in WP-01).
- Payloads (x64, verify versions in WP-01): SampledProfile `{ptr InstructionPointer; u32 ThreadId; u16 Count; u16 Reserved}`; CSwitch `{u32 NewThreadId; u32 OldThreadId; i8 NewThreadPriority; i8 OldThreadPriority; u8 PreviousCState; i8 SpareByte; i8 OldThreadWaitReason; i8 OldThreadWaitMode; i8 OldThreadState; i8 OldThreadWaitIdealProcessor; u32 NewThreadWaitTime; u32 Reserved}`; ReadyThread `{u32 TThreadId; i8 AdjustReason; i8 AdjustIncrement; i8 Flag; i8 Reserved}`; StackWalk `{u64 EventTimeStamp; u32 StackProcess; u32 StackThread; ptr Stack[]}`; Image `{ptr ImageBase; ptr ImageSize; u32 ProcessId; u32 ImageChecksum; u32 TimeDateStamp; u32 Reserved0; ptr DefaultBase; u32 Reserved1..4; wstr FileName}`.
- CPU number: `EVENT_RECORD.BufferContext.ProcessorNumber` (or `ProcessorIndex` on newer SDKs).
- Flags: `EVENT_TRACE_FLAG_PROCESS | THREAD | IMAGE_LOAD | PROFILE | CSWITCH | DISPATCHER`. Stack walking via `TraceSetInformation(session, TraceStackTracingInfo, CLASSIC_EVENT_ID[3], ...)`. Interval via `TraceSetInformation(..., TraceSampledProfileIntervalInfo, &TRACE_PROFILE_INTERVAL{Source=0, Interval=1221}, ...)`. Privilege `SeSystemProfilePrivilege`. Clock `Wnode.ClientContext = 1` (QPC). Consumer: `OpenTrace` + `ProcessTrace` with `PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP`; `PerfFreq` from the logfile header.
- Wait reasons: Windows `KWAIT_REASON` (Executive 0, FreePage 1, PageIn 2, …, WrQueue 15, WrLpcReceive 16, WrLpcReply 17, WrVirtualMemory 18, WrPageOut 19, WrRendezvous 20, …, WrPreempted 32 etc.); map to UI categories Synchronization / I/O / Memory / Preempted / Other.

## Appendix C: Linux reference for WP-70/71
- `perf_event_attr`: `type=PERF_TYPE_SOFTWARE, config=PERF_COUNT_SW_CPU_CLOCK, freq=1, sample_freq=8000, sample_type=PERF_SAMPLE_IP|TID|TIME|CALLCHAIN|CPU, mmap=1, mmap2=1, comm=1, task=1, use_clockid=1, clockid=CLOCK_MONOTONIC_RAW, exclude_kernel (option), sample_max_stack=192`.
- Open per CPU with `pid=-1` (needs `CAP_PERFMON` or `perf_event_paranoid<=0`) or per-pid with `inherit=1`. Ring: `mmap` of `1 + 2^n` pages; read `perf_event_mmap_page.data_head`, process records, write `data_tail`.
- Records: `PERF_RECORD_SAMPLE`, `MMAP2` (module mapping, `ino`/build-id with `PERF_RECORD_MISC_MMAP_BUILD_ID` on 5.12+), `COMM` (thread names), `FORK`/`EXIT`, `THROTTLE`/`UNTHROTTLE`, `LOST`.
- Scheduler: tracepoint ids from `/sys/kernel/tracing/events/sched/sched_switch/id` and `sched_wakeup/id`; `PERF_TYPE_TRACEPOINT`, `config=id`, `sample_type |= PERF_SAMPLE_RAW`; parse `prev_pid, prev_state, next_pid` from the raw payload using the `format` file offsets.
- Sysctls: `kernel.perf_event_max_sample_rate`, `kernel.perf_cpu_time_max_percent`, `kernel.perf_event_paranoid`, `kernel.kptr_restrict`.

## Appendix D: Sources
Superluminal product, docs, workflow, platform pages, version history, support: https://superluminal.eu/ , https://www.superluminal.eu/docs/documentation.html , https://superluminal.eu/workflow/ , https://superluminal.eu/applications/windows/ , https://superluminal.eu/applications/linux/ , https://superluminal.eu/version-history/ , https://superluminal.eu/support/ , https://superluminal.eu/?p=3156 . Engineering posts: https://superluminal.eu/compressing-callstacks/ , https://rovarma.com/articles/from-profiling-to-kernel-patch-the-journey-to-an-ebpf-performance-fix/ , https://rovarma.com/articles/a-tale-about-fixing-ebpf-spinlock-issues-in-the-linux-kernel/ . Performance API headers (BSD) via https://github.com/EmbarkStudios/superluminal-perf-rs . Usage write-ups: https://dev.to/attiliohimeki/unity-profiling-using-superluminal-2oc8 , https://discussions.unity.com/t/profiling-in-unity-using-superluminal/1614358 , https://blog.s-schoener.com/2025-01-21-unity-vfx-graph-multithread/ , https://blog.s-schoener.com/2025-02-13-superluminal-compute-memory/ , https://github.com/TomerAberbach/profiler-md/issues/228 . ETW: https://davekilian.com/windows-profiling.html , https://learn.microsoft.com/en-us/previous-versions/windows/desktop/xperf/stack-walking , https://learn.microsoft.com/ko-kr/archive/blogs/sajay/how-to-collect-stacks-during-context-switches , https://github.com/PascalBeyer/Simple-ETW-based-Profiler . Tracy: https://github.com/wolfpld/tracy . eBPF unwinding background: https://www.elastic.co/blog/universal-profiling-frame-pointers-symbols-ebpf , https://pkg.go.dev/go.opentelemetry.io/ebpf-profiler .
