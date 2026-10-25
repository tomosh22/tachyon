// On-disk structures of the .tachyon trace file format v1 (spec: docs/specs/trace-format.md, Part 3.1).
// All structs are little-endian and tightly packed.
#pragma once

#include "core/strong_id.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tachyon::trace {

inline constexpr char kMagic[8] = {'T', 'A', 'C', 'H', 'Y', 'O', 'N', '\0'};
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint64_t kMaxRecordsPerChunk = 1'048'576;
inline constexpr uint64_t kFileHeaderSize = 64;

// FileHeader::flags bits.
inline constexpr uint32_t kFlagHasInstrumentation = 1u << 0;
inline constexpr uint32_t kFlagCircularCapture = 1u << 1;
inline constexpr uint32_t kFlagPartial = 1u << 2;

// FileHeader::platform values.
inline constexpr uint32_t kPlatformWindows = 1;
inline constexpr uint32_t kPlatformLinux = 2;

// ThreadRec::flags bits.
inline constexpr uint32_t kThreadFlagTargetProcess = 1u << 0;

// SampleBatch::flags bits.
inline constexpr uint16_t kSampleFlagKernelLeaf = 1u << 0;

// ModuleRec::id_kind values.
inline constexpr uint8_t kModuleIdPdb = 1;
inline constexpr uint8_t kModuleIdGnuBuildId = 2;

// ScopeEventRec::kind values.
inline constexpr uint8_t kScopeBegin = 0;
inline constexpr uint8_t kScopeEnd = 1;

// FiberEventRec::kind values.
inline constexpr uint8_t kFiberRegister = 0;
inline constexpr uint8_t kFiberUnregister = 1;
inline constexpr uint8_t kFiberSwitchBegin = 2;
inline constexpr uint8_t kFiberSwitchEnd = 3;

// ClockCalibrationRec::other_clock_kind values.
inline constexpr uint32_t kClockTsc = 1;
inline constexpr uint32_t kClockGpu = 2;

enum class ChunkType : uint32_t {
  Strings = 1,
  Processes = 2,
  Threads = 3,
  Modules = 4,
  Frames = 5,
  CallstackNodes = 6,
  Samples = 7,
  ContextSwitches = 8,
  ReadyEvents = 9,
  ScopeEvents = 10,
  FiberEvents = 11,
  FrameMarks = 12,
  Markers = 13,
  PlotSamples = 14,
  ThreadNames = 15,
  ClockCalibration = 16,
  CaptureInfo = 17,
  Footer = 0xFFFF,
};

inline constexpr uint8_t kCompressionNone = 0;
inline constexpr uint8_t kCompressionZstd = 1;
inline constexpr uint8_t kChunkSchemaVersion = 1;

#pragma pack(push, 1)

struct FileHeader {
  char magic[8] = {'T', 'A', 'C', 'H', 'Y', 'O', 'N', '\0'};
  uint32_t format_version = kFormatVersion;
  uint32_t flags = 0;
  uint64_t clock_frequency_hz = 0;
  uint64_t start_ts = 0;
  uint64_t end_ts = 0;
  uint64_t footer_offset = 0;  // 0 if not finalized; reader must then scan chunks
  uint32_t platform = 0;
  uint32_t reserved[3] = {0, 0, 0};
};

struct ChunkHeader {
  uint32_t type = 0;  // ChunkType
  uint8_t compression = kCompressionNone;
  uint8_t version = kChunkSchemaVersion;
  uint16_t reserved = 0;
  uint64_t uncompressed_size = 0;
  uint64_t compressed_size = 0;  // == uncompressed_size when compression == 0
  uint64_t record_count = 0;
};

struct ProcessRec {
  uint32_t pid;
  uint32_t parent_pid;
  uint64_t start_ts, end_ts;
  StringIdx name;
  StringIdx command_line;
  bool operator==(const ProcessRec&) const = default;
};

struct ThreadRec {
  uint32_t tid;
  uint32_t pid;
  uint64_t start_ts, end_ts;
  StringIdx name;
  uint32_t flags;  // bit0 = is_target_process
  bool operator==(const ThreadRec&) const = default;
};

struct ModuleRec {
  uint32_t pid;
  uint64_t base;
  uint64_t size;
  StringIdx path;
  uint8_t id_kind;  // 1 pdb guid+age, 2 gnu build-id
  uint8_t id_len;
  uint8_t id[32];
  uint32_t age;
  uint64_t load_ts, unload_ts;
  bool operator==(const ModuleRec&) const = default;
};

struct FrameRec {
  ModuleIdx module;  // kInvalidIdx: addr_or_rva is an absolute address (unmapped frame)
  uint64_t addr_or_rva;
  bool operator==(const FrameRec&) const = default;
};

struct CallstackNodeRec {
  CallstackIdx parent;
  FrameIdx frame;
  bool operator==(const CallstackNodeRec&) const = default;
};

struct ContextSwitchRec {
  uint64_t ts;
  uint32_t cpu;
  uint32_t old_tid;
  uint32_t new_tid;
  uint8_t old_state;
  uint8_t old_wait_reason;
  uint8_t old_wait_mode;
  uint8_t new_priority;
  CallstackIdx old_thread_stack;
  uint32_t reserved;
  bool operator==(const ContextSwitchRec&) const = default;
};

struct ReadyRec {
  uint64_t ts;
  uint32_t cpu;
  uint32_t readied_tid;
  uint32_t readying_tid;
  CallstackIdx readying_stack;
  uint8_t adjust_reason;
  uint8_t flags;
  uint16_t reserved;
  bool operator==(const ReadyRec&) const = default;
};

struct ScopeEventRec {
  uint64_t ts;
  uint32_t tid;
  uint8_t kind;  // 0 begin, 1 end
  uint8_t reserved[3];
  StringIdx id;
  StringIdx data;
  uint32_t color_rgba;
  uint64_t caller_addr;
  bool operator==(const ScopeEventRec&) const = default;
};

struct FiberEventRec {
  uint64_t ts;
  uint32_t tid;
  uint8_t kind;  // 0 register, 1 unregister, 2 switch_begin, 3 switch_end
  uint8_t reserved[3];
  uint64_t fiber_id;
  uint64_t other_fiber_id;
  bool operator==(const FiberEventRec&) const = default;
};

struct FrameMarkRec {
  uint64_t ts;
  uint32_t tid;
  StringIdx name;
  uint64_t frame_number;
  bool operator==(const FrameMarkRec&) const = default;
};

struct MarkerRec {
  uint64_t ts;
  uint32_t tid;
  StringIdx name;
  StringIdx data;
  uint32_t color_rgba;
  bool operator==(const MarkerRec&) const = default;
};

struct PlotSampleRec {
  uint64_t ts;
  StringIdx name;
  double value;
  bool operator==(const PlotSampleRec&) const = default;
};

// Addition (3.1 does not specify the ThreadNames layout): thread `tid` was (re)named at `ts`.
struct ThreadNameRec {
  uint64_t ts;
  uint32_t tid;
  StringIdx name;
  bool operator==(const ThreadNameRec&) const = default;
};

struct ClockCalibrationRec {
  uint64_t trace_ts;
  uint64_t other_clock_ts;
  uint32_t other_clock_kind;  // 1 TSC, 2 GPU
  bool operator==(const ClockCalibrationRec&) const = default;
};

struct FooterRec {
  uint32_t type;
  uint32_t version;
  uint64_t offset;  // absolute file offset of the chunk's ChunkHeader
  uint64_t record_count;
  bool operator==(const FooterRec&) const = default;
};

#pragma pack(pop)

static_assert(sizeof(FileHeader) == 64);
static_assert(sizeof(ChunkHeader) == 32);
static_assert(sizeof(ProcessRec) == 32);
static_assert(sizeof(ThreadRec) == 32);
static_assert(sizeof(ModuleRec) == 78);
static_assert(sizeof(FrameRec) == 12);
static_assert(sizeof(CallstackNodeRec) == 8);
static_assert(sizeof(ContextSwitchRec) == 32);
static_assert(sizeof(ReadyRec) == 28);
static_assert(sizeof(ScopeEventRec) == 36);
static_assert(sizeof(FiberEventRec) == 32);
static_assert(sizeof(FrameMarkRec) == 24);
static_assert(sizeof(MarkerRec) == 24);
static_assert(sizeof(PlotSampleRec) == 20);
static_assert(sizeof(ThreadNameRec) == 16);
static_assert(sizeof(ClockCalibrationRec) == 20);
static_assert(sizeof(FooterRec) == 24);

// Bytes per sample in the SoA Samples chunk: ts(8) + tid(4) + callstack(4) + cpu(2) + flags(2) = 20 (the "18
// bytes" in the Part 3.2 memory budget does not match the 3.1 column list; see the WP-03 report).
inline constexpr size_t kSampleBytes = 20;

// Structure-of-arrays batch of samples; all vectors must have equal length.
struct SampleBatch {
  std::vector<uint64_t> ts;
  std::vector<uint32_t> tid;
  std::vector<CallstackIdx> callstack;
  std::vector<uint16_t> cpu;
  std::vector<uint16_t> flags;  // bit0 kernel-mode leaf

  // Thread-safe for const access.
  size_t size() const { return ts.size(); }
  // Thread-safe for const access.
  bool consistent() const {
    return tid.size() == ts.size() && callstack.size() == ts.size() && cpu.size() == ts.size() &&
           flags.size() == ts.size();
  }
  // Not thread-safe.
  void push(uint64_t t, uint32_t thread, CallstackIdx stack, uint16_t cpuIdx, uint16_t flag = 0) {
    ts.push_back(t);
    tid.push_back(thread);
    callstack.push_back(stack);
    cpu.push_back(cpuIdx);
    flags.push_back(flag);
  }
  bool operator==(const SampleBatch&) const = default;
};

}  // namespace tachyon::trace
