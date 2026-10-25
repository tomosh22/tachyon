// Merging decoded instrumentation into a finished `.tachyon` trace.
//
// The merge rewrites the trace: tables (Strings / Frames / CallstackNodes) are carried over with identical
// indices, all sampling chunks are re-appended unchanged, the instrumentation chunks (ScopeEvents,
// FiberEvents, FrameMarks, Markers, PlotSamples, ThreadNames) are the stable, timestamp-sorted union of what
// the trace already had and the new records, `kFlagHasInstrumentation` is set and CaptureInfo gains an
// "instrumentation" object. The output is written to a temporary file next to its final path and renamed
// into place, so a failed merge never damages the input and an interrupted one never leaves a half-written
// trace under the real name.
//
// ThreadNames are not events of a time window: when the window is known (clipping, or the range after
// extension) each thread keeps the last name given at or before its start, stamped with the window start,
// names after the end are moved to the end, and names never widen the header range.
//
// CaptureInfo gets an "instrumentation" object with a "merges" list that grows by one entry per merge (inputs
// with a content hash, stream facts, counts, warnings). Merging an input whose hash is already listed fails
// unless MergeOptions::force is set. If the input trace was partial the output keeps the partial flag (it is
// a finalized file with a valid footer, but the capture behind it was cut short).
//
// Timestamps outside the trace's [start_ts, end_ts]: by the format (trace-format.md) nothing forbids them,
// so by default they are kept and the header range is extended to cover them. A circular (flight-recorder)
// capture covers only a recent window, so for those the caller sets `clipToTraceRange` and records outside
// the window are dropped instead (and counted).
#pragma once

#include "core/result.h"
#include "inst/decoder.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace tachyon::inst {

struct MergeInput {
  std::filesystem::path file;
  uint64_t maxBytes = ~uint64_t{0};  // read at most this much (a snapshot of a growing spool)
};

struct MergeOptions {
  // Destination. Empty: replace the input trace in place (temporary file + atomic rename).
  std::filesystem::path outPath;
  bool clipToTraceRange = false;
  // Merge even when a sidecar with the same content was merged into this trace before (it would duplicate
  // every record); without it that is an error.
  bool force = false;
  // Overrides the clock mapping of the decoder (tests); see DecoderConfig.
  std::function<std::unique_ptr<IClockMapper>(const StreamInfo&)> clockMapperFactory;
};

struct MergeReport {
  std::filesystem::path outputPath;
  bool wroteOutput = false;  // false: nothing to merge and the trace was left untouched
  DecodeStats decode;
  std::vector<StreamInfo> streams;
  // Records written to the output by this merge (after clipping).
  uint64_t scopeEvents = 0, fiberEvents = 0, frameMarks = 0, markers = 0, plots = 0, threadNames = 0;
  uint64_t clippedRecords = 0;
  uint64_t existingInstrumentationRecords = 0;  // already in the input trace
  uint64_t startTs = 0, endTs = 0;              // header range of the output
  // Decode warnings plus merge-level ones (unbalanced scopes, dropped records, clipped records, ...).
  std::vector<std::string> warnings;
};

// Not thread-safe per output path. Decodes `inputs` (each may hold several streams, in the given order) and
// merges them into `tracePath`. Fails without touching the input when the trace cannot be read, an input is
// not an instrumentation stream, or the output cannot be written.
Result<MergeReport> mergeInstrumentation(const std::filesystem::path& tracePath,
                                         std::span<const MergeInput> inputs,
                                         const MergeOptions& options = {});

// Not thread-safe per output path. The same for records that were decoded already (with a decoder configured
// for the trace's clock). `data` is consumed.
Result<MergeReport> mergeDecoded(const std::filesystem::path& tracePath, DecodedInstrumentation data,
                                 const MergeOptions& options = {});

// Thread-safe: reads the trace header and calibration records. The decoder configuration that matches a
// trace (its clock frequency and ClockCalibration records).
Result<DecoderConfig> decoderConfigFor(const std::filesystem::path& tracePath);

// Counters from checkScopeBalance().
struct ScopeBalance {
  uint64_t threads = 0;
  uint64_t begins = 0;
  uint64_t ends = 0;
  uint64_t unmatchedEnds = 0;   // an End with nothing open on its thread
  uint64_t unclosedBegins = 0;  // Begins still open at the end
  uint64_t outOfOrder = 0;      // events earlier than the previous event of the same thread
};

// Thread-safe: pure function. Per-thread Begin/End balance and ordering of a ScopeEvents sequence as stored
// in a trace (no fiber awareness: it treats every thread as one stack). Pairing is WP-53.
ScopeBalance checkScopeBalance(std::span<const trace::ScopeEventRec> events);

}  // namespace tachyon::inst
