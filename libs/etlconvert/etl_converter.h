// ETL to .tachyon converter (WP-12).
//
// EtlConverter::convert() is the Windows entry point (ProcessTrace over an .etl file). The
// platform-independent core is ConversionPipeline: decoded kernel events go in through EventSink, a .tachyon
// file comes out. Linux tests feed it from .hexevents fixtures; a future real-time consumer (v1.1) and the
// WP-11 backend can reuse it.
//
// Two passes over the events give exact module and thread resolution even though the kernel logger emits its
// rundown (DCEnd) events at the end of the file: pass Metadata handles only Process/Thread/Image events, pass
// Main does everything else. A single Main pass also works (metadata is then taken as it arrives, so
// addresses of modules whose rundown comes later stay unmapped).
//
// Stage split: the calling thread decodes, filters, matches stacks and resolves addresses to (module, RVA);
// a worker thread owns the TraceWriter and does the frame / callstack interning and chunk output. A bounded
// queue of batches joins them.
#pragma once

#include "core/result.h"
#include "etlconvert/etl_events.h"
#include "etlconvert/module_table.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tachyon::etlconvert {

enum class CaptureMode : uint8_t {
  Process,  // keep the target processes' samples; context switches / readies touching a target thread
  System,   // keep everything (system-wide samples, all switches); target threads are still flagged
};

// A pid that counts as a target only between two timestamps (raw trace ticks, inclusive). Lets a capture
// follow children without also keeping the samples of an unrelated process that later reuses a pid.
struct TargetInterval {
  uint32_t pid = 0;
  uint64_t start = 0;           // 0: from the beginning of the trace
  uint64_t end = ~uint64_t{0};  // ~0: until the end of the trace
};

struct ConvertOptions {
  // Target processes. Empty (together with empty targetIntervals) means system mode: keep everything,
  // regardless of `mode`. A pid listed here without an interval is a target for the whole trace.
  std::vector<uint32_t> targetPids;
  // Pids that are targets only inside the given intervals (several per pid allowed). A pid that appears in
  // both lists is scoped by its intervals.
  std::vector<TargetInterval> targetIntervals;
  CaptureMode mode = CaptureMode::Process;
  uint32_t samplingHz = 0;  // recorded in CaptureInfo
  uint64_t eventsLost = 0;  // from the recording session (ControlTrace); max'ed with the ETL header value
  std::string
      captureOptionsJson;    // embedded under "capture_options" in CaptureInfo (valid JSON, else a string)
  std::string agentVersion;  // embedded in CaptureInfo
  bool keepUnmappedFrames = true;       // false: frames outside every known module are dropped from stacks
  std::function<void(float)> progress;  // 0..1, called from the converting thread

  // Tuning and test hooks.
  uint32_t stackWindowMs = 250;  // how long a parent waits for its StackWalk
  bool twoPass = true;           // convert() runs a metadata pass before the main pass
  bool threaded = true;          // false: intern on the calling thread (deterministic, for tests)
  size_t queueDepth = 6;         // batches in flight between parse and intern
  size_t batchEvents = 32768;    // events per batch
  PeLookupFn peLookup;           // empty: read real files (after device path conversion)
  const DevicePathMapper* deviceMapper = nullptr;  // null: DevicePathMapper::fromSystem()
};

// Facts about the ETL file (from the ETW logfile header on Windows).
struct EtlFileInfo {
  uint64_t clockFrequencyHz = 10'000'000;  // PerfFreq
  uint32_t numberOfProcessors = 0;
  uint32_t pointerSize = 8;
  uint64_t eventsLost = 0;
  uint64_t buffersLost = 0;
  uint64_t buffersWritten = 0;
};

struct ConvertStats {
  // Input event counts.
  uint64_t eventsTotal = 0;
  uint64_t processEvents = 0;
  uint64_t threadEvents = 0;
  uint64_t imageEvents = 0;
  uint64_t sampledProfileEvents = 0;
  uint64_t cswitchEvents = 0;
  uint64_t readyEvents = 0;
  uint64_t stackWalkEvents = 0;  // opcode 32
  uint64_t stackKeyEvents = 0;   // opcodes 34-37: compressed stack keys, skipped (see warnings)
  uint64_t setNameEvents = 0;
  uint64_t otherEvents = 0;
  uint64_t malformedEvents = 0;

  // Output counts.
  uint64_t samplesWritten = 0;
  uint64_t samplesDropped = 0;  // filtered out (not a target thread / idle thread)
  uint64_t cswitchesWritten = 0;
  uint64_t cswitchesDropped = 0;
  uint64_t readiesWritten = 0;
  uint64_t readiesDropped = 0;
  uint64_t processesWritten = 0;
  uint64_t threadsWritten = 0;
  uint64_t modulesWritten = 0;

  // Stack matching.
  uint64_t stacksMatched = 0;    // StackWalk events attached to a parent (both halves of a pair count)
  uint64_t stacksUnmatched = 0;  // no pending parent with the same (timestamp, thread)
  uint64_t stacksUnmatchedNonTarget = 0;  // subset of the above whose thread is not in a target process
  uint64_t stacksTwoPart = 0;             // parents whose kernel and user stacks arrived separately
  uint64_t parentsWithStack[3] = {};      // by ParentKind, among parents that were kept
  uint64_t parentsWithoutStack[3] = {};   // written with callstack 0
  uint64_t matchRole[3][3] = {};          // [ParentKind][MatchRole]: which thread id the StackThread matched

  // Frames.
  uint64_t framesTotal = 0;
  uint64_t framesUnmapped = 0;  // written as absolute addresses (or dropped, see keepUnmappedFrames)

  // Modules.
  uint32_t modulesRsds = 0;         // identified by PDB GUID + age (id_kind 1)
  uint32_t modulesFallback = 0;     // id_kind 0: TimeDateStamp + SizeOfImage
  uint32_t modulesFileMissing = 0;  // subset of fallback
  uint32_t modulesNoCodeView = 0;   // subset of fallback
  uint32_t modulesMismatch = 0;     // subset of fallback

  // Output.
  uint64_t startTs = 0;
  uint64_t endTs = 0;
  uint64_t clockFrequencyHz = 0;
  uint64_t eventsLost = 0;
  double seconds = 0;
  std::vector<std::string> warnings;
};

enum class PipelinePass : uint8_t { Metadata, Main };

// Consumer of decoded kernel events.
class EventSink {
 public:
  virtual ~EventSink() = default;
  // Not thread-safe. Events must be delivered in trace order. `event.payload` need only live for the call.
  virtual void onEvent(const EtwEvent& event) = 0;
};

class ConversionPipeline : public EventSink {
 public:
  // Thread-safe: creates the output file and (when threaded) the intern worker. `info.clockFrequencyHz` is
  // written into the file header, so it must be known here (the ETW logfile header has it after OpenTrace).
  static Result<std::unique_ptr<ConversionPipeline>> create(const std::filesystem::path& out,
                                                            ConvertOptions options, const EtlFileInfo& info);
  ~ConversionPipeline() override;

  // Not thread-safe. Updates the counters (events lost, buffers lost) once the ETL has been consumed; the
  // clock frequency given to create() stays.
  void setFileInfo(const EtlFileInfo& info);
  // Not thread-safe. Selects what onEvent() does. Starts in Main.
  void beginPass(PipelinePass pass);
  // Not thread-safe.
  void onEvent(const EtwEvent& event) override;
  // Not thread-safe. Releases queued events, writes processes / threads / modules / CaptureInfo, finalises
  // the file. Call once.
  Result<ConvertStats> finish();

  // Thread-safe for const access from the calling thread only. Counters so far.
  const ConvertStats& stats() const;

 private:
  struct Impl;
  explicit ConversionPipeline(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

class EtlConverter {
 public:
  // Not thread-safe (one ProcessTrace consumer per call; concurrent calls on different files are fine).
  // Windows: converts the ETL at `etl` to a .tachyon file at `out`. Elsewhere: ErrorCode::Unsupported.
  static Result<ConvertStats> convert(const std::filesystem::path& etl, const std::filesystem::path& out,
                                      const ConvertOptions& options);

  // Thread-safe. Converts a .hexevents fixture (the `etlstat stats --dump-hex` text format) on any platform.
  static Result<ConvertStats> convertHexEvents(const std::filesystem::path& hexEvents,
                                               const std::filesystem::path& out,
                                               const ConvertOptions& options, const EtlFileInfo& info = {});
};

}  // namespace tachyon::etlconvert
