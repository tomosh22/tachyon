<!-- Orchestrator-owned. Part 3.1, kept in sync with libs/trace/trace_format.h. Changes only via orchestrator. -->

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
struct ThreadNameRec { uint64_t ts; uint32_t tid; StringIdx name; };      // ThreadNames chunk: thread `tid` (re)named at `ts`
// CaptureInfo: one JSON document (UTF-8), record_count 1; keys listed below.
struct FooterRec   { uint32_t type; uint32_t version; uint64_t offset; uint64_t record_count; }; // one per chunk, in file order
```
Rules:
- `FrameRec` is 12 bytes. When `module == kInvalidIdx`, `addr_or_rva` holds the absolute address so unmapped frames survive conversion; otherwise it holds the RVA.
- `FileHeader.footer_offset` and `FooterRec.offset` are absolute file offsets of the respective `ChunkHeader`. The footer is a chunk of type `Footer` (0xFFFF), uncompressed, whose payload is `FooterRec[]` for every preceding chunk (not itself); `FooterRec.version` is that chunk's schema version.
- Chunks of the same type may repeat (streaming writers append). Readers concatenate in order; indices are global across repeats of Frames/CallstackNodes/Strings only when the writer guarantees it (writers must; `tracegen`, `etlconvert`, the perf converter and the instrumentation merge do).
- Samples, ContextSwitches, ReadyEvents, Scope/Fiber/Marker/FrameMark/Plot records within a chunk are sorted by `ts`. Across chunks, order is by chunk sequence; readers merge-sort if needed.
- Callstack node 0 is the root `{parent = kInvalidIdx, frame = kInvalidIdx}` and is written as record 0 of the first CallstackNodes chunk. `internCallstack(leafToRoot)` returns the leaf node index; an empty span returns 0. String 0 is "" and is record 0 of the first Strings chunk. Both are always present.
- Partial scheme: the writer writes the header with flags bit2 set and `footer_offset = 0` at open and rewrites it at `finish()` with bit2 cleared. The reader scans from offset 64 when `footer_offset == 0` or the footer is invalid, stops at the first truncated or invalid chunk, and reports bit2 in `header().flags`. (`TraceWriter::keepPartialFlag()` keeps bit2 set in a rewritten file whose input was partial.)
- Max record_count per chunk: 1,048,576 (keeps zstd frames around tens of MB). Writers must split every chunk type, including Strings/Frames/CallstackNodes, at that count. Readers must reject (not throw on) `record_count > 1,048,576`, `record_count` inconsistent with payload size, payloads over 1 GiB, and zstd frames whose content size differs from `uncompressed_size`.
- Samples SoA = 20 bytes/sample on disk (u64 ts, u32 tid, u32 callstack, u16 cpu, u16 flags). In memory (model.md) the tid column is dropped: 16 bytes/sample.
- Packed sizes: FileHeader 64, ChunkHeader 32, ProcessRec 32, ThreadRec 32, ModuleRec 78, FrameRec 12, CallstackNodeRec 8, ContextSwitchRec 32, ReadyRec 28, ScopeEventRec 36, FiberEventRec 32, FrameMarkRec 24, MarkerRec 24, PlotSampleRec 20, ClockCalibrationRec 20, ThreadNameRec 16, FooterRec 24 (all `static_assert`ed in the header).
- Alignment: records are `#pragma pack(1)`, so in arrays some members are misaligned (ModuleRec is 78 bytes, so its StrongId `path` is misaligned at most indices; 8-byte fields of FrameRec, PlotSampleRec, ReadyRec, etc. may be misaligned too). Never bind references to packed members (`const auto& x = rec.field`, passing to `std::min`, gtest `EXPECT_EQ`) or call StrongId members on array elements of ModuleRec; copy the field or the record to a local first. Readers decode records whose size is not a multiple of 4 without value-initializing in place. Use `ModuleInfo` (`TraceReader::readModuleInfos()`, `toModuleInfo()`), a naturally aligned copy of ModuleRec, for in-memory module tables.
- `ModuleRec.id_kind`: 1 = PDB GUID + age (`kModuleIdPdb`; the GUID is stored in on-disk GUID struct byte order, Data1..3 little-endian, `age` in the age field), 2 = GNU build-id (`kModuleIdGnuBuildId`), 3 = PE timestamp+size (`kModuleIdPeTimestampSize`): `id_len` 8, u32 TimeDateStamp then u32 SizeOfImage, little-endian, age 0; used when the PE has no CodeView RSDS record or the file is missing/different from the loaded image.
- ELF modules: `ModuleRec.base` is the load address of the module's lowest PT_LOAD segment (not the executable segment's mmap address). RVA = address − base. The DWARF provider maps RVA → ELF vaddr as `rva + (lowest PT_LOAD p_vaddr aligned down to 4096)`. The perf backend fills `base` accordingly.
- Converters write ModuleRec `load_ts` = trace start for modules loaded before the trace and `unload_ts` = trace end when still loaded; kernel modules use pid 0.
- `ContextSwitchRec.reserved` / `ReadyRec.reserved` bit0 (`kSwitchFlagStackFromOtherThread`): the attached stack was walked on a thread other than `old_tid` / `readying_tid`. Other bits stay 0. `ContextSwitchRec.old_state` uses the KTHREAD_STATE numbering on both platforms and `old_wait_reason` is platform specific; see attribution-rules.md (rule 4 and "Wait reasons").
- `ThreadRec.flags` bit0 = target process (`kThreadFlagTargetProcess`); the Samples `flags` column bit0 = kernel-mode leaf (`kSampleFlagKernelLeaf`).
- `CallstackInterner` memory budget: ≤ 16 bytes/node measured after `compact()`; growth policy is doubling at load ≤ 0.7, so the as-grown peak is ~22–23 bytes/node (reported, not budgeted). Confirmed by user 2026-10-25.

### CaptureInfo (chunk 17)
Raw UTF-8 JSON object, record_count 1. The model parses only `sampling_hz` and `events_lost` (`model::CaptureInfo`); everything else is informational and consumers (the viewer's Trace Info panel, the report tool) must tolerate absent keys. Keys written by the producers:
- Common to the ETW and perf converters: `source` (`"etl"` / `"perf"`), `converter` `{name, version}`, `agent_version`, `sampling_hz`, `events_lost`, `clock_frequency_hz`, `mode` (`"system"` / `"process"`), `target_pids`, `capture_options` (the request's options as JSON), `host`, and `convert_stats` (always includes `frames_total`, `frames_unmapped`, `samples_written`, `warnings`).
- ETW: `host` is `{platform:"windows", logical_cores, pointer_size, etl_buffers_lost, etl_buffers_written}`; `convert_stats` also has `events_total`, `samples_dropped`, `context_switches_written`, `ready_events_written`, `stacks_matched`, `stacks_unmatched`, `stack_match_roles`, `parents_with_stack`, `parents_without_stack`, `modules_rsds`, `modules_fallback`.
- Perf: `host` is `{platform:"linux", logical_cores, kernel_release, hostname, pointer_size}`. It adds `perf` (`plan`, `rings`, `kernel_stacks`, `exclude_kernel`, `kernel_build_id`, `paranoid`, `max_sample_rate`, `cpu_time_max_percent`, `clock`), `sched` (`path`: tracepoint or fallback, `switches_requested`, `wakeups_requested`, `wake_event`, `fallback_reason`, `wait_reasons`, `blocking_stacks`, `wakers`, `switch_events`, `wake_events`, `context_switches_written`, `ready_events_written`, `dropped_not_target`, `dropped_window`, `malformed`) and `throttle` (`events`, `throttled_ms`, `estimated_missed_samples`). Its `convert_stats` has `records_read`, `samples_read`, `samples_dropped_not_target`, `samples_dropped_idle`, `samples_dropped_window`, `records_unsupported`, `records_malformed`, `sched_unattributed_switches`, `frames_kernel`, `modules`, `modules_with_build_id`, `modules_from_kernel_id`. `sched_unattributed_switches` counts switch records of an exiting task that could not be attributed and were dropped (WP-71b): informational below 100 records and 0.1 % of all switch records, otherwise it is also a warning.
- A flushed trace (flight recorder, WP-60) adds `flush_reason`, `flush_name`, `flush_window_s` and `flush_trigger` (true for SDK triggers).
- The instrumentation merge (WP-52) keeps the previous CaptureInfo and adds `instrumentation` `{merge_count, merges:[{sources, streams, scope_events, fiber_events, frame_marks, markers, plot_samples, thread_names, dropped_records, clipped_records, unmatched_scope_ends, unclosed_scopes, warnings}]}`.
- `tracegen` writes `generator`, `agent_version`, `sampling_hz`, `events_lost`, `host` `{os, synthetic}`, `capture_options` `{scenario, seed}` and `tracegen`.

### Public API (`libs/trace/trace_writer.h`, `trace_reader.h`; namespace `tachyon::trace`)
```cpp
class TraceWriter { public:
  static Result<TraceWriter> open(const std::filesystem::path&, FileHeader);
  static Result<ReservedFile> reserveNew(const std::filesystem::path&);   // create-exclusive, no-follow; returns {volume, index}
  static Result<TraceWriter> open(const std::filesystem::path&, FileHeader, const ReservedFile&);  // write into exactly that file
  StringIdx internString(std::string_view); FrameIdx internFrame(ModuleIdx, uint64_t);
  CallstackIdx internCallstack(std::span<const FrameIdx> leafToRoot); CallstackIdx internCallstackNode(CallstackIdx parent, FrameIdx);
  void append(const SampleBatch&);
  void append(std::span<const ProcessRec>);  /* ... Thread, Module, ContextSwitch, Ready, ScopeEvent, FiberEvent, FrameMark, Marker, PlotSample, ThreadName, ClockCalibration */
  void appendCaptureInfo(std::string_view json);
  void keepPartialFlag(); void setTimeRange(uint64_t startTs, uint64_t endTs); void addFlags(uint32_t);
  Result<void> finish(); const CallstackInterner& callstacks() const; size_t chunkCount() const; };
class TraceReader { public:
  static Result<TraceReader> open(const std::filesystem::path&);
  const FileHeader& header() const; std::span<const FooterRec> chunks() const; bool isPartial() const;
  Result<void> readChunk(const FooterRec&, std::vector<std::byte>& out);
  Result<StringTable> readStrings(); Result<std::vector<ProcessRec>> readProcesses();
  /* one typed helper per chunk type: readThreads, readModules, readModuleInfos, readFrames, readCallstackNodes, readSamples (SampleBatch),
     readContextSwitches, readReadyEvents, readScopeEvents, readFiberEvents, readFrameMarks, readMarkers, readPlotSamples, readThreadNames,
     readClockCalibrations */
  Result<std::string> readCaptureInfo(); };
class CallstackInterner { /* flat std::vector<CallstackNodeRec> nodes; open-addressing hash of (parent,frame)->idx storing only indices; load <= 0.7, doubling; compact() */ };
```
Output safety (WP-66b): `TraceWriter::open` never follows a link at the final path (O_NOFOLLOW / FILE_FLAG_OPEN_REPARSE_POINT), refuses a non-regular file and an existing file with more than one hard link before truncating. With a `ReservedFile` it opens the existing file with that device/inode (nlink 1) and fails if the path holds another file.

## Changelog
- 2026-10-25 (WP-03, WP-04, WP-12): header ownership; footer, partial and node-0 rules; ThreadNames record; size list; alignment rule; `id_kind` 3; `kSwitchFlagStackFromOtherThread`; module load/unload convention.
- 2026-10-25 (WP-72): ELF `ModuleRec.base` rule.
- 2026-11-03 (WP-60): flushed-trace CaptureInfo keys.
- 2026-11-08 (WP-66b): output safety rules of `TraceWriter`.
- 2026-11-15: clarifications folded into the rules above; CaptureInfo keys documented (WP-52, WP-70, WP-71b, WP-12); public API listing brought up to date.
