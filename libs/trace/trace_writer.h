// Streaming writer for .tachyon trace files.
#pragma once

#include "core/result.h"
#include "core/strong_id.h"
#include "trace/callstack_interner.h"
#include "trace/trace_format.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace tachyon::trace {

// Intern tables (strings, frames, callstack nodes) are global across the file. Pending table entries are
// flushed as Strings/Frames/CallstackNodes chunks before every chunk written by an append(), and by finish().
// Appends of more than kMaxRecordsPerChunk records are split into several chunks.
// I/O errors are sticky: they are reported by finish().
class TraceWriter {
 public:
  // Thread-safe: creates the file; the returned writer is single-threaded. The header is written with
  // footer_offset = 0 and the partial flag set; finish() rewrites it. magic/format_version are filled in.
  static Result<TraceWriter> open(const std::filesystem::path& path, FileHeader header);

  TraceWriter(TraceWriter&&) noexcept;
  TraceWriter& operator=(TraceWriter&&) noexcept;
  TraceWriter(const TraceWriter&) = delete;
  TraceWriter& operator=(const TraceWriter&) = delete;
  // Not thread-safe. An unfinished writer leaves a partial (recoverable) file.
  ~TraceWriter();

  // Not thread-safe. Index 0 is "" (pre-interned).
  StringIdx internString(std::string_view s);
  // Not thread-safe. module == kInvalidIdx means addr is an absolute address.
  FrameIdx internFrame(ModuleIdx module, uint64_t addrOrRva);
  // Not thread-safe. Returns the leaf node; an empty span returns CallstackIdx(0).
  CallstackIdx internCallstack(std::span<const FrameIdx> leafToRoot);
  // Not thread-safe. Extends an existing callstack by one frame (parent is the caller side).
  CallstackIdx internCallstackNode(CallstackIdx parent, FrameIdx frame);

  // All append() overloads: not thread-safe. Records must be sorted by ts within one call.
  void append(const SampleBatch& samples);
  void append(std::span<const ProcessRec> records);
  void append(std::span<const ThreadRec> records);
  void append(std::span<const ModuleRec> records);
  void append(std::span<const ContextSwitchRec> records);
  void append(std::span<const ReadyRec> records);
  void append(std::span<const ScopeEventRec> records);
  void append(std::span<const FiberEventRec> records);
  void append(std::span<const FrameMarkRec> records);
  void append(std::span<const MarkerRec> records);
  void append(std::span<const PlotSampleRec> records);
  void append(std::span<const ThreadNameRec> records);
  void append(std::span<const ClockCalibrationRec> records);
  // Not thread-safe. `json` is the raw UTF-8 JSON document (record_count 1).
  void appendCaptureInfo(std::string_view json);

  // Not thread-safe. Updates header fields that are written by finish().
  void setTimeRange(uint64_t startTs, uint64_t endTs);
  // Not thread-safe. ORs flag bits into the header flags written by finish().
  void addFlags(uint32_t flags);

  // Not thread-safe. Flushes pending tables, writes the footer chunk, rewrites the header (partial flag
  // cleared, footer_offset set) and closes the file. Idempotent.
  Result<void> finish();

  // Thread-safe for const access from the owning thread only.
  const CallstackInterner& callstacks() const;
  // Number of chunks written so far (excluding the footer).
  size_t chunkCount() const;

 private:
  struct Impl;
  explicit TraceWriter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

}  // namespace tachyon::trace
