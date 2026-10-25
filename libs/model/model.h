// In-memory trace model (spec: docs/specs/model.md, Part 3.2) and its loader.
#pragma once

#include "core/result.h"
#include "core/strong_id.h"
#include "trace/string_table.h"
#include "trace/trace_format.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tachyon::model {

// PDB GUID+age or GNU build-id (Part 3.6). Lives here because libs/symbols depends on model.
struct ModuleId {
  uint8_t kind = 0;  // trace::kModuleIdPdb / kModuleIdGnuBuildId / kModuleIdPeTimestampSize, 0 = none
  std::array<uint8_t, 32> bytes{};
  uint8_t len = 0;
  uint32_t age = 0;
  bool operator==(const ModuleId&) const = default;
};

enum class SymbolState : uint8_t { Pending, Loading, Resolved, NotFound, Failed };

// Heuristic from the thread name (case-insensitive substring, checked in this order): "main" -> Main,
// "render" -> Render, "worker" or "job" -> Worker; any other non-empty name -> Other; empty name -> Unknown.
enum class ThreadRole : uint8_t { Unknown, Main, Render, Worker, Other };

enum class ThreadState : uint8_t { Running, Ready, Waiting, Unknown };

struct Process {
  uint32_t pid = 0, parent_pid = 0;
  uint64_t start_ts = 0, end_ts = 0;
  std::string name, command_line;
};

struct Module {
  uint32_t pid = 0;
  uint64_t base = 0, size = 0;
  std::string path;
  ModuleId id;
  uint64_t load_ts = 0, unload_ts = 0;
  SymbolState state = SymbolState::Pending;
};

struct Thread {
  uint32_t tid = 0, pid = 0;
  std::string name;
  uint64_t first_ts = 0, last_ts = 0;
  bool is_target = false;
  ThreadRole role = ThreadRole::Unknown;
};

// symbolication attaches FunctionIdx lazily (symbols lib)
struct Frame {
  ModuleIdx module;  // kInvalidIdx: addr_or_rva is an absolute address
  uint64_t addr_or_rva = 0;
};

// nodes[0] = root
struct CallstackNode {
  CallstackIdx parent;
  FrameIdx frame;
};
static_assert(sizeof(CallstackNode) == 8);

// Per thread, sorted by ts.
struct SampleColumns {
  std::vector<uint64_t> ts;
  std::vector<CallstackIdx> callstack;
  std::vector<uint16_t> cpu;
  std::vector<uint16_t> flags;
};

struct ThreadStateInterval {
  uint64_t start = 0, end = 0;
  ThreadState state = ThreadState::Unknown;
  uint8_t wait_reason = 0;
  uint32_t cpu = 0;
  CallstackIdx blocking_stack, unblocking_stack;
  uint32_t woke_by_tid = 0;
  uint64_t ready_ts = 0;
};

struct ScopeInstance {
  uint64_t begin = 0, end = 0;
  StringIdx id, data;
  uint32_t color = 0;
  uint16_t depth = 0;
  uint64_t caller_addr = 0;
};

// Mirrors trace::FiberEventRec with a ThreadIdx.
struct FiberSwitch {
  uint64_t ts = 0;
  ThreadIdx thread;
  uint8_t kind = 0;  // trace::kFiberRegister .. kFiberSwitchEnd
  uint64_t fiber_id = 0, other_fiber_id = 0;
};

struct FrameMark {
  uint64_t ts = 0;
  ThreadIdx thread;
  StringIdx name;
  uint64_t frame_number = 0;
};

struct Marker {
  uint64_t ts = 0;
  ThreadIdx thread;  // invalid for a global marker (tid 0)
  StringIdx name, data;
  uint32_t color = 0;
};

struct PlotSeries {
  StringIdx name;
  std::vector<uint64_t> ts;
  std::vector<double> value;
};

// Parsed fields of the CaptureInfo JSON chunk.
struct CaptureInfo {
  uint64_t sampling_hz = 0;  // 0 if absent
  uint64_t events_lost = 0;
  std::string raw_json;  // empty if the trace has no CaptureInfo chunk
};

struct LoadOptions {
  // Worker threads used for chunk decoding and scattering; 0 means std::thread::hardware_concurrency().
  unsigned threads = 0;
  // Optional cancellation flag, polled between work items; when it becomes true load() returns Cancelled.
  const std::atomic<bool>* cancel = nullptr;
};

struct TraceModel {
  trace::FileHeader header;
  bool isPartial = false;  // header flag bit2 (unfinalized file, or the footer was unusable)
  CaptureInfo captureInfo;

  std::vector<Process> processes;
  std::vector<Thread> threads;
  std::vector<Module> modules;
  std::vector<Frame> frames;
  std::vector<CallstackNode> nodes;
  trace::StringTable strings;
  std::unordered_map<uint32_t, ThreadIdx> tidToThread;           // OS tid -> index into threads
  std::vector<SampleColumns> samplesByThread;                    // index = ThreadIdx
  std::vector<std::vector<ThreadStateInterval>> statesByThread;  // sized to threads, empty; filled by WP-31
  std::vector<std::vector<ScopeInstance>> scopesByThread;        // sized to threads, empty; filled by WP-53
  std::vector<FiberSwitch> fiberSwitches;
  std::vector<FrameMark> frameMarks;
  std::vector<Marker> markers;
  std::vector<PlotSeries> plots;
  uint64_t sampleIntervalTicks = 0;

  // Additions to 3.2: raw event streams as loaded, sorted by ts (stable), for WP-31 / WP-53 to build from.
  std::vector<trace::ContextSwitchRec> contextSwitches;
  std::vector<trace::ReadyRec> readyEvents;
  std::vector<trace::ScopeEventRec> scopeEvents;
  std::vector<trace::ClockCalibrationRec> clockCalibrations;

  // Thread-safe (no shared state): reads the file and builds a new model. Chunks are decoded on
  // options.threads workers; the calling thread is one of them and the only one that calls `progress` (value
  // in [0,1], monotonic, 1.0 on success). Partial or torn files load what is recoverable.
  static Result<TraceModel> load(const std::filesystem::path& path, const LoadOptions& options = {},
                                 std::function<void(float)> progress = {});

  // Thread-safe for concurrent const access. Sum of capacity * element size over all vectors, string storage
  // and an estimate for the hash containers.
  size_t residentBytes() const;
};

// Thread-safe: pure function. Applies the ThreadRole heuristic documented above.
ThreadRole classifyThreadRole(std::string_view name);

}  // namespace tachyon::model
