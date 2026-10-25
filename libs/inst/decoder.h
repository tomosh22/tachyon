// Decoding of instrumentation wire streams (agent spools, `.tachyon-inst` sidecar files) into trace records.
//
// Output records use *local* StringIdx values (indices into DecodedInstrumentation::strings); the merge step
// remaps them into the trace's string table. Timestamps are converted to the trace clock through an
// IClockMapper (identity by default), clamped so that each thread's events never go backwards, and the
// records of every kind are sorted by timestamp (stable: the order within one thread is the order it was
// produced in).
//
// Validation done while decoding, reported as warnings: unresolved string keys, malformed or unknown frames,
// truncated tails, per-thread timestamp regressions, Dropped frames, ScopeEnds with nothing open and scopes
// still open at the end (balance is tracked in wire order per thread, and per fiber across fiber switches).
// Pairing and blending of scopes are WP-53.
#pragma once

#include "core/result.h"
#include "inst/clock_mapper.h"
#include "trace/string_table.h"
#include "trace/trace_format.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace tachyon::inst {

// Reserved scope / marker names.
inline constexpr const char* kDroppedMarkerName = "tachyon.dropped";
// Colour of the tachyon.dropped markers (opaque red).
inline constexpr uint32_t kDroppedMarkerColor = 0xFF4040FFu;

struct StreamInfo {
  uint32_t index = 0;  // order of appearance
  std::string source;  // file path or label
  uint32_t wireVersion = 0;
  uint32_t pid = 0;
  uint32_t platform = 0;
  uint64_t clockFrequencyHz = 0;
  uint64_t processStartTs = 0;  // as stamped by the SDK (stream clock)
  uint32_t apiVersion = 0;
  std::string clockMapper;  // IClockMapper::describe()
};

struct DecodeStats {
  uint64_t sources = 0;
  uint64_t bytes = 0;
  uint64_t streams = 0;
  uint64_t frames = 0;
  uint64_t unknownFrames = 0;    // skipped by length
  uint64_t malformedFrames = 0;  // too short for their type, skipped
  uint64_t truncatedTailBytes = 0;
  uint64_t corruptBytes = 0;  // discarded after an implausible frame length
  uint64_t scopeBegins = 0;
  uint64_t scopeEnds = 0;
  uint64_t fiberEvents = 0;
  uint64_t frameMarks = 0;
  uint64_t markers = 0;  // includes the tachyon.dropped ones
  uint64_t plots = 0;
  uint64_t threadNames = 0;
  uint64_t droppedFrames = 0;   // Dropped frames seen
  uint64_t droppedRecords = 0;  // records they report lost
  uint64_t unresolvedStrings = 0;
  uint64_t clampedTimestamps = 0;
  uint64_t duplicateThreadNames = 0;  // re-sent after a stream switch, not repeated in the output
  uint64_t unmatchedScopeEnds = 0;
  uint64_t unclosedScopes = 0;  // valid after finish()
};

// One decoded input (file or byte range): what was read and a hash of it, used to detect merging the same
// sidecar twice.
struct SourceInfo {
  std::string name;
  uint64_t bytes = 0;
  uint64_t hash = 0;  // XXH3-64 over the bytes read (0 for an empty source)
};

struct DecodedInstrumentation {
  trace::StringTable strings;  // local string table; index 0 is ""
  std::vector<trace::ScopeEventRec> scopes;
  std::vector<trace::FiberEventRec> fibers;
  std::vector<trace::FrameMarkRec> frameMarks;
  std::vector<trace::MarkerRec> markers;
  std::vector<trace::PlotSampleRec> plots;
  std::vector<trace::ThreadNameRec> threadNames;
  std::vector<StreamInfo> streams;
  std::vector<SourceInfo> sources;
  DecodeStats stats;
  std::vector<std::string> warnings;
  uint64_t minTs = 0;  // over all records; both 0 when there are none
  uint64_t maxTs = 0;

  // Thread-safe for const access.
  size_t recordCount() const {
    return scopes.size() + fibers.size() + frameMarks.size() + markers.size() + plots.size() +
           threadNames.size();
  }
  bool empty() const { return recordCount() == 0; }
};

struct DecoderConfig {
  uint64_t traceClockHz = 0;  // 0: unknown, streams are taken to be in the trace clock
  std::vector<trace::ClockCalibrationRec> calibrations;
  // Overrides makeClockMapper(); called once per stream.
  std::function<std::unique_ptr<IClockMapper>(const StreamInfo&)> clockMapperFactory;
};

class InstDecoder {
 public:
  // Thread-safe: constructs.
  explicit InstDecoder(DecoderConfig config = {});

  // Not thread-safe. Decodes a file that holds one or more streams. At most `maxBytes` bytes are read (a
  // snapshot of a spool that is still growing). Fails when the file cannot be read or does not start with an
  // instrumentation StreamHeader.
  Result<void> addFile(const std::filesystem::path& file, uint64_t maxBytes = ~uint64_t{0});
  // Not thread-safe. Decodes the bytes of one source (tests, in-memory streams).
  Result<void> addBytes(std::span<const uint8_t> bytes, const std::string& label);

  // Not thread-safe. Sorts the records, closes the balance accounting, builds the warnings. The decoder is
  // empty afterwards.
  DecodedInstrumentation finish();

 private:
  struct State;
  std::shared_ptr<State> m_state;
};

}  // namespace tachyon::inst
