<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.1. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

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
struct ModuleRec   { uint32_t pid; uint64_t base; uint64_t size; StringIdx path; uint8_t id_kind; /*1 pdb guid+age, 2 gnu build-id*/ uint8_t id_len; uint8_t id[32]; uint32_t age; uint64_t load_ts, unload_ts; };
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

## Orchestrator clarifications (binding; merged with WP-03, 2026-10-25)
Header: `libs/trace/trace_format.h` (namespace `tachyon::trace`), now orchestrator-owned.
- `FileHeader.footer_offset` and `FooterRec.offset` are absolute file offsets of the respective `ChunkHeader`. The footer is a chunk of type `Footer` (0xFFFF), uncompressed, whose payload is `FooterRec[]` for every preceding chunk (not itself); `FooterRec.version` is that chunk's schema version.
- Callstack node 0 is the root `{parent = kInvalidIdx, frame = kInvalidIdx}` and is written as record 0 of the first CallstackNodes chunk. `internCallstack(leafToRoot)` returns the leaf node index; an empty span returns 0. String 0 is "" and is record 0 of the first Strings chunk. Both are always present.
- Partial scheme: the writer writes the header with flags bit2 set and `footer_offset = 0` at open and rewrites it at `finish()` with bit2 cleared. The reader scans from offset 64 when `footer_offset == 0` or the footer is invalid, stops at the first truncated or invalid chunk, and reports bit2 in `header().flags`.
- Samples SoA = 20 bytes/sample on disk (u64 ts, u32 tid, u32 callstack, u16 cpu, u16 flags). In memory (3.2) the tid column is dropped: 16 bytes/sample.
- `ThreadNames` chunk record: `ThreadNameRec { uint64_t ts; uint32_t tid; StringIdx name; }` (16 bytes).
- `CaptureInfo` payload is raw UTF-8 JSON, record_count 1.
- Packed sizes: FileHeader 64, ChunkHeader 32, ProcessRec 32, ThreadRec 32, ModuleRec 78, FrameRec 12, CallstackNodeRec 8, ContextSwitchRec 32, ReadyRec 28, ScopeEventRec 36, FiberEventRec 32, FrameMarkRec 24, MarkerRec 24, PlotSampleRec 20, ClockCalibrationRec 20, ThreadNameRec 16, FooterRec 24.
- Readers must reject (not throw on) `record_count > 1,048,576`, `record_count` inconsistent with payload size, payloads over 1 GiB, and zstd frames whose content size differs from `uncompressed_size`.
- Writers must split every chunk type, including Strings/Frames/CallstackNodes, at 1,048,576 records.
- `CallstackInterner` memory budget (WP-03): ≤ 16 bytes/node measured after `compact()`; growth policy is doubling at load ≤ 0.7, so the as-grown peak is ~22–23 bytes/node (reported, not budgeted). Confirmed by user 2026-10-25.
