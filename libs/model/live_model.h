// Live view model (WP-65a, docs/specs/live.md section 3): the consumer side of the live batch encoding.
//
// The agent's LiveEncoder (libs/control/live_encoder.h) turns the raw-identity events of a running capture
// into batches of records with LIVE indices (strings, processes, threads, modules, frames, callstack nodes);
// LiveModel decodes those batches, keeps the table definitions and the events of the last `windowNs` of trace
// time, and builds ordinary immutable TraceModel snapshots of the window for the existing analysis and UI code.
//
// THE BATCH ENCODING (little-endian; this is what the encoder writes and apply() reads). A batch is a
// sequence of records {uint32 type; uint32 byte_len; payload[byte_len]}; unknown types are skipped by their
// length. Definitions always precede their first use in the stream (a keyframe starts with all of them).
// Records with a u32 `idx` define (or, for a repeated idx, redefine) that table entry.
//
//   type  record           payload
//    1    StringDef        u32 idx; bytes[byte_len-4] (UTF-8)
//    2    ProcessDef       u32 idx; trace::ProcessRec (32 B; name / command_line are string indices)
//    3    ThreadDef        u32 idx; trace::ThreadRec (32 B; name is a string index; flags bit0 = target)
//    4    ThreadName       u32 thread idx; u32 string idx   (a rename of an already defined thread)
//    5    ModuleDef        u32 idx; trace::ModuleRec (78 B; path is a string index; re-sent with unload_ts set
//                          when the module is unloaded)
//    6    FrameDef         u32 idx; trace::FrameRec (12 B; module is a module index or kInvalidIdx)
//    7    NodeDef          u32 idx; trace::CallstackNodeRec (8 B; parent / frame are node / frame indices;
//                          node 0 is the root {kInvalidIdx, kInvalidIdx})
//    8    Samples          u32 count; ts[count] u64; thread[count] u32 (thread idx); node[count] u32;
//                          cpu[count] u16; flags[count] u16
//    9    ContextSwitches  trace::ContextSwitchRec[] (32 B each; old_thread_stack is a node index; tids are
//                          OS thread ids)
//   10    Ready            trace::ReadyRec[] (28 B; readying_stack is a node index)
//   11    Scopes           trace::ScopeEventRec[] (36 B; id / data are string indices; tid is an OS tid)
//   12    FrameMarks       trace::FrameMarkRec[] (24 B; name is a string index)
//   13    Markers          trace::MarkerRec[] (24 B)
//   14    Plots            trace::PlotSampleRec[] (20 B)
//   15    TimeAdvance      u64 ts (the capture's clock is at ts)
//   16    Dropped          u64 events (live events lost since the last Dropped record)
//   17    Info             u64 clock_frequency_hz; u32 platform; u32 sampling_hz; u64 start_ts
//                          (first record of a keyframe; added to the spec by WP-65a)
// Index 0 of the string table is "" and node 0 is the root; the encoder reserves string 1 as "<overflow>".
// Record structs are the packed on-disk ones of trace/trace_format.h.
#pragma once

#include "core/result.h"
#include "model/model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace tachyon::model {

namespace live_wire {

enum class RecordType : uint32_t {
  StringDef = 1,
  ProcessDef = 2,
  ThreadDef = 3,
  ThreadName = 4,
  ModuleDef = 5,
  FrameDef = 6,
  NodeDef = 7,
  Samples = 8,
  ContextSwitches = 9,
  Ready = 10,
  Scopes = 11,
  FrameMarks = 12,
  Markers = 13,
  Plots = 14,
  TimeAdvance = 15,
  Dropped = 16,
  Info = 17,
};

// Table caps shared by the encoder and the model: an over-cap identity is mapped to an overflow entry by the
// encoder; the model refuses definitions beyond them.
inline constexpr uint32_t kMaxStrings = 1u << 20;
inline constexpr uint64_t kMaxStringBytes = 64ull << 20;
inline constexpr uint32_t kMaxProcesses = 1u << 16;
inline constexpr uint32_t kMaxThreads = 1u << 18;
inline constexpr uint32_t kMaxModules = 1u << 16;
inline constexpr uint32_t kMaxFrames = 1u << 22;
inline constexpr uint32_t kMaxNodes = 1u << 23;
// One event of the protocol carries at most this many raw bytes (base64 on the wire stays under the 1 MiB
// line limit).
inline constexpr size_t kMaxEventBytes = 512 * 1024;
// String index 1 is reserved for identities that did not fit a table.
inline constexpr uint32_t kOverflowString = 1;
inline constexpr const char* kOverflowName = "<overflow>";

#pragma pack(push, 1)
struct RecordHeader {
  uint32_t type;
  uint32_t byte_len;
};
struct InfoPayload {
  uint64_t clock_frequency_hz;
  uint32_t platform;
  uint32_t sampling_hz;
  uint64_t start_ts;
};
struct ThreadNamePayload {
  uint32_t thread;
  uint32_t string;
};
#pragma pack(pop)
static_assert(sizeof(RecordHeader) == 8);
static_assert(sizeof(InfoPayload) == 24);
static_assert(sizeof(ThreadNamePayload) == 8);

// Thread-safe: pure function. Appends one record (header + `size` payload bytes) to `out`.
inline void putRecord(std::string& out, RecordType type, const void* payload, size_t size) {
  RecordHeader h{static_cast<uint32_t>(type), static_cast<uint32_t>(size)};
  out.append(reinterpret_cast<const char*>(&h), sizeof(h));
  if (size != 0) out.append(static_cast<const char*>(payload), size);
}
// Thread-safe: pure function. A record whose payload is `idx` followed by `size` bytes of `body`.
inline void putIndexedRecord(std::string& out, RecordType type, uint32_t idx, const void* body, size_t size) {
  RecordHeader h{static_cast<uint32_t>(type), static_cast<uint32_t>(sizeof(idx) + size)};
  out.append(reinterpret_cast<const char*>(&h), sizeof(h));
  out.append(reinterpret_cast<const char*>(&idx), sizeof(idx));
  if (size != 0) out.append(static_cast<const char*>(body), size);
}

}  // namespace live_wire

// What a LiveModel is allowed to keep.
struct LiveModelOptions {
  // Events older than `now - windowNs` (trace time) are discarded as newer ones arrive.
  uint64_t windowNs = 30ull * 1'000'000'000ull;
};

struct LiveModelStats {
  uint64_t generation = 0;
  uint64_t batches = 0;
  uint64_t keyframes = 0;
  uint64_t nowTs = 0;          // the capture's newest timestamp seen
  uint64_t windowStartTs = 0;  // now - window (0 while the capture is younger than the window)
  uint64_t samplesInWindow = 0;
  uint64_t droppedLive = 0;      // sum of Dropped records
  uint64_t unknownRecords = 0;   // records of an unknown type that were skipped
  uint64_t residentBytes = 0;    // estimate of the heap held by tables and window events
};

// Thread-safe: every method takes an internal lock (apply() is one writer; snapshot() may run concurrently
// from another thread and blocks apply() while it copies, about 100 ms for 4 M samples).
class LiveModel {
 public:
  // Thread-safe.
  explicit LiveModel(LiveModelOptions options = {});
  ~LiveModel();
  LiveModel(const LiveModel&) = delete;
  LiveModel& operator=(const LiveModel&) = delete;

  // Thread-safe. Decodes one batch (the `data` of a `live` event, base64-decoded). With `keyframe` the window
  // and all tables are reset first. A record that is malformed or out of range makes the call fail with
  // CorruptData after applying what precedes it; the caller should resubscribe. generation() increases by one
  // for every call that applied at least one record.
  Result<void> apply(std::span<const std::byte> data, bool keyframe);

  // Thread-safe. An ordinary TraceModel of the window: header start / end = window bounds, isPartial = true,
  // loadStatus.complete = false (generation = generation()), captureInfo.raw_json = {"live":true,...} (also
  // "dropped_live"), tables copied with their live indices (identity mapping: ThreadIdx = live thread index,
  // CallstackIdx = live node, ...), events of the window sorted by time, statesByThread / scopesByThread
  // sized to the thread count and empty (analysis fills them as for a file).
  std::shared_ptr<const TraceModel> snapshot() const;

  // Thread-safe. Increases with every applied batch; 0 before the first.
  uint64_t generation() const;
  // Thread-safe.
  LiveModelStats stats() const;
  // Thread-safe. Clock frequency of the stream (from the Info record), 0 before it arrives.
  uint64_t clockFrequencyHz() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace tachyon::model
