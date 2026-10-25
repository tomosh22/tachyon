#include "inst/merge.h"

#include "trace/trace_reader.h"
#include "trace/trace_writer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <system_error>
#include <unordered_map>

namespace tachyon::inst {

namespace fs = std::filesystem;
using namespace trace;

namespace {

// Name of the temporary file the merge writes next to its destination.
fs::path tempPathFor(const fs::path& out) {
  static std::atomic<uint32_t> counter{0};
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path tmp = out;
  tmp += ".merge-" + std::to_string(static_cast<uint64_t>(ticks)) + "-" + std::to_string(counter++) + ".tmp";
  return tmp;
}

// Chunk types the merge re-appends untouched, in this order.
template <typename T>
Result<void> copyChunks(TraceReader& reader, TraceWriter& writer, ChunkType type) {
  std::vector<std::byte> payload;
  for (const FooterRec& chunk : reader.chunks()) {
    if (chunk.type != static_cast<uint32_t>(type)) continue;
    TACHYON_TRY(reader.readChunk(chunk, payload));
    std::vector<T> records;
    TACHYON_TRY(TraceReader::decodeRecords<T>(chunk, payload, records));
    writer.append(std::span<const T>(records));
  }
  return ok();
}

// ModuleRec is 78 bytes, so elements of a std::vector<ModuleRec> are misaligned and constructing them is
// undefined behaviour (see trace-format.md, Alignment). The payload is handed to the writer as it is.
Result<void> copyModuleChunks(TraceReader& reader, TraceWriter& writer) {
  std::vector<std::byte> payload;
  for (const FooterRec& chunk : reader.chunks()) {
    if (chunk.type != static_cast<uint32_t>(ChunkType::Modules)) continue;
    TACHYON_TRY(reader.readChunk(chunk, payload));
    if (chunk.record_count > kMaxRecordsPerChunk || payload.size() != chunk.record_count * sizeof(ModuleRec))
      return makeError(ErrorCode::CorruptData, "Modules chunk size does not match its record count");
    writer.append(std::span<const ModuleRec>(reinterpret_cast<const ModuleRec*>(payload.data()),
                                             static_cast<size_t>(chunk.record_count)));
  }
  return ok();
}

Result<void> copySamples(TraceReader& reader, TraceWriter& writer) {
  std::vector<std::byte> payload;
  for (const FooterRec& chunk : reader.chunks()) {
    if (chunk.type != static_cast<uint32_t>(ChunkType::Samples)) continue;
    TACHYON_TRY(reader.readChunk(chunk, payload));
    SampleBatch batch;
    TACHYON_TRY(TraceReader::decodeSamples(chunk, payload, batch));
    writer.append(batch);
  }
  return ok();
}

bool isKnownChunkType(uint32_t type) {
  return (type >= static_cast<uint32_t>(ChunkType::Strings) &&
          type <= static_cast<uint32_t>(ChunkType::CaptureInfo));
}

template <typename Rec>
size_t clipRecords(std::vector<Rec>& records, uint64_t startTs, uint64_t endTs) {
  const size_t before = records.size();
  records.erase(std::remove_if(records.begin(), records.end(),
                               [&](const Rec& r) {
                                 const uint64_t ts = r.ts;
                                 return ts < startTs || ts > endTs;
                               }),
                records.end());
  return before - records.size();
}

template <typename Rec>
void noteRange(const std::vector<Rec>& records, bool& any, uint64_t& lo, uint64_t& hi) {
  if (records.empty()) return;
  // Sorted by timestamp.
  const uint64_t first = records.front().ts, last = records.back().ts;
  if (!any) {
    lo = first;
    hi = last;
    any = true;
  } else {
    lo = std::min(lo, first);
    hi = std::max(hi, last);
  }
}

// existing (already in the trace's index space) followed by `added`, stable-sorted by timestamp.
template <typename Rec>
std::vector<Rec> unionSorted(std::vector<Rec> existing, std::vector<Rec> added) {
  const bool needSort = !existing.empty();
  existing.insert(existing.end(), added.begin(), added.end());
  if (needSort)
    std::stable_sort(existing.begin(), existing.end(), [](const Rec& a, const Rec& b) {
      const uint64_t ta = a.ts, tb = b.ts;
      return ta < tb;
    });
  return existing;
}

std::string buildCaptureInfo(const std::string* previous, const MergeReport& report,
                             const DecodedInstrumentation& data) {
  using nlohmann::json;
  json info = json::object();
  if (previous != nullptr) {
    json parsed = json::parse(*previous, nullptr, false);
    if (parsed.is_object()) {
      info = std::move(parsed);
    } else {
      info["original_capture_info"] = *previous;  // not a JSON object: keep it verbatim
    }
  }
  uint64_t merges = 1;
  if (info.contains("instrumentation") && info["instrumentation"].is_object()) {
    const json& old = info["instrumentation"];
    if (old.contains("merge_count") && old["merge_count"].is_number_unsigned())
      merges = old["merge_count"].get<uint64_t>() + 1;
  }
  json streams = json::array();
  for (const StreamInfo& s : data.streams) {
    streams.push_back({{"pid", s.pid},
                       {"platform", s.platform},
                       {"clock_frequency_hz", s.clockFrequencyHz},
                       {"wire_version", s.wireVersion},
                       {"api_version", s.apiVersion},
                       {"clock_mapper", s.clockMapper},
                       {"source", s.source}});
  }
  json warnings = json::array();
  for (const std::string& w : report.warnings) warnings.push_back(w);
  info["instrumentation"] = {{"merge_count", merges},
                             {"streams", streams},
                             {"scope_events", report.scopeEvents},
                             {"fiber_events", report.fiberEvents},
                             {"frame_marks", report.frameMarks},
                             {"markers", report.markers},
                             {"plot_samples", report.plots},
                             {"thread_names", report.threadNames},
                             {"dropped_records", data.stats.droppedRecords},
                             {"clipped_records", report.clippedRecords},
                             {"unmatched_scope_ends", data.stats.unmatchedScopeEnds},
                             {"unclosed_scopes", data.stats.unclosedScopes},
                             {"warnings", warnings}};
  return info.dump();
}

// Remaps the local string ids of `records` through `map` in place.
Result<void> remapId(StringIdx& id, const std::vector<StringIdx>& map) {
  const uint32_t v = id.get();
  if (v >= map.size())
    return makeError(ErrorCode::Internal, "instrumentation record refers to an unknown string");
  id = map[v];
  return ok();
}

}  // namespace

Result<DecoderConfig> decoderConfigFor(const fs::path& tracePath) {
  TACHYON_TRY_ASSIGN(TraceReader reader, TraceReader::open(tracePath));
  DecoderConfig config;
  config.traceClockHz = reader.header().clock_frequency_hz;
  auto calibrations = reader.readClockCalibrations();
  if (calibrations.ok()) config.calibrations = std::move(*calibrations);
  return config;
}

Result<MergeReport> mergeInstrumentation(const fs::path& tracePath, std::span<const MergeInput> inputs,
                                         const MergeOptions& options) {
  TACHYON_TRY_ASSIGN(DecoderConfig config, decoderConfigFor(tracePath));
  config.clockMapperFactory = options.clockMapperFactory;
  InstDecoder decoder(std::move(config));
  for (const MergeInput& in : inputs) TACHYON_TRY(decoder.addFile(in.file, in.maxBytes));
  return mergeDecoded(tracePath, decoder.finish(), options);
}

Result<MergeReport> mergeDecoded(const fs::path& tracePath, DecodedInstrumentation data,
                                 const MergeOptions& options) {
  std::error_code ec;
  const bool inPlace = options.outPath.empty() || fs::equivalent(options.outPath, tracePath, ec);
  const fs::path outPath = inPlace ? tracePath : options.outPath;

  MergeReport report;
  report.outputPath = outPath;
  report.decode = data.stats;
  report.streams = data.streams;
  report.warnings = data.warnings;

  // Held in an optional so the input can be closed before the output replaces it (Windows).
  std::optional<TraceReader> readerHolder;
  {
    TACHYON_TRY_ASSIGN(TraceReader opened, TraceReader::open(tracePath));
    readerHolder.emplace(std::move(opened));
  }
  TraceReader& reader = *readerHolder;
  const FileHeader inHeader = reader.header();
  report.startTs = inHeader.start_ts;
  report.endTs = inHeader.end_ts;

  if (data.empty() && inPlace) {
    report.warnings.push_back("no instrumentation records to merge; the trace was left unchanged");
    return report;
  }

  // Records outside the trace window.
  // A header without a time range (both zero) cannot clip anything; it is then filled from the records.
  const bool clipping = options.clipToTraceRange && inHeader.end_ts > inHeader.start_ts;
  if (clipping) {
    size_t clipped = 0;
    clipped += clipRecords(data.scopes, inHeader.start_ts, inHeader.end_ts);
    clipped += clipRecords(data.fibers, inHeader.start_ts, inHeader.end_ts);
    clipped += clipRecords(data.frameMarks, inHeader.start_ts, inHeader.end_ts);
    clipped += clipRecords(data.markers, inHeader.start_ts, inHeader.end_ts);
    clipped += clipRecords(data.plots, inHeader.start_ts, inHeader.end_ts);
    clipped += clipRecords(data.threadNames, inHeader.start_ts, inHeader.end_ts);
    report.clippedRecords = clipped;
    if (clipped > 0)
      report.warnings.push_back(std::to_string(clipped) +
                                " instrumentation records outside the trace window were dropped; scopes that "
                                "straddle the window start may have an End without a Begin");
  }

  // Window of the output: the input's, extended over the new records unless they were clipped.
  bool any = false;
  uint64_t lo = 0, hi = 0;
  noteRange(data.scopes, any, lo, hi);
  noteRange(data.fibers, any, lo, hi);
  noteRange(data.frameMarks, any, lo, hi);
  noteRange(data.markers, any, lo, hi);
  noteRange(data.plots, any, lo, hi);
  noteRange(data.threadNames, any, lo, hi);
  uint64_t startTs = inHeader.start_ts, endTs = inHeader.end_ts;
  if (any && !clipping) {
    if (startTs == 0 && endTs == 0) {
      startTs = lo;
      endTs = hi;
    } else {
      startTs = std::min(startTs, lo);
      endTs = std::max(endTs, hi);
    }
  }
  report.startTs = startTs;
  report.endTs = endTs;

  if ((inHeader.flags & kFlagPartial) != 0)
    report.warnings.push_back(
        "the input trace was partial (unfinished); the output contains what was readable");

  // ---- write the new file ----
  const fs::path tmp = tempPathFor(outPath);
  struct TempGuard {
    fs::path path;
    bool armed = true;
    ~TempGuard() {
      if (!armed) return;
      std::error_code removeEc;
      fs::remove(path, removeEc);
    }
  } guard{tmp};

  FileHeader outHeader = inHeader;
  outHeader.start_ts = startTs;
  outHeader.end_ts = endTs;
  TACHYON_TRY_ASSIGN(TraceWriter writer, TraceWriter::open(tmp, outHeader));

  // Tables keep their indices: the records refer to them.
  {
    TACHYON_TRY_ASSIGN(StringTable strings, reader.readStrings());
    for (uint32_t i = 1; i < strings.size(); ++i) {
      if (writer.internString(strings.get(StringIdx(i))).get() != i)
        return makeError(
            ErrorCode::Unsupported,
            "the trace's string table contains duplicate strings; cannot merge without renumbering");
    }
    TACHYON_TRY_ASSIGN(std::vector<FrameRec> frames, reader.readFrames());
    for (uint32_t i = 0; i < frames.size(); ++i) {
      const FrameRec f = frames[i];
      if (writer.internFrame(f.module, f.addr_or_rva).get() != i)
        return makeError(
            ErrorCode::Unsupported,
            "the trace's frame table contains duplicate frames; cannot merge without renumbering");
    }
    TACHYON_TRY_ASSIGN(std::vector<CallstackNodeRec> nodes, reader.readCallstackNodes());
    for (uint32_t i = 1; i < nodes.size(); ++i) {
      const CallstackNodeRec n = nodes[i];
      if (n.parent.get() >= i)
        return makeError(ErrorCode::CorruptData, "callstack node refers to a later parent");
      if (writer.internCallstackNode(n.parent, n.frame).get() != i)
        return makeError(
            ErrorCode::Unsupported,
            "the trace's callstack table contains duplicate nodes; cannot merge without renumbering");
    }
  }

  TACHYON_TRY(copyChunks<ProcessRec>(reader, writer, ChunkType::Processes));
  TACHYON_TRY(copyChunks<ThreadRec>(reader, writer, ChunkType::Threads));
  TACHYON_TRY(copyModuleChunks(reader, writer));
  TACHYON_TRY(copySamples(reader, writer));
  TACHYON_TRY(copyChunks<ContextSwitchRec>(reader, writer, ChunkType::ContextSwitches));
  TACHYON_TRY(copyChunks<ReadyRec>(reader, writer, ChunkType::ReadyEvents));
  TACHYON_TRY(copyChunks<ClockCalibrationRec>(reader, writer, ChunkType::ClockCalibration));
  for (const FooterRec& chunk : reader.chunks()) {
    if (!isKnownChunkType(chunk.type))
      report.warnings.push_back("a chunk of unknown type " + std::to_string(chunk.type) +
                                " was not carried over");
  }

  // Instrumentation: new strings first, then the union with what the trace already had.
  std::vector<StringIdx> map(data.strings.size());
  map[0] = StringIdx(0);
  for (uint32_t i = 1; i < data.strings.size(); ++i)
    map[i] = writer.internString(data.strings.get(StringIdx(i)));
  for (ScopeEventRec& r : data.scopes) {
    StringIdx id = r.id, text = r.data;
    TACHYON_TRY(remapId(id, map));
    TACHYON_TRY(remapId(text, map));
    r.id = id;
    r.data = text;
  }
  for (FrameMarkRec& r : data.frameMarks) {
    StringIdx name = r.name;
    TACHYON_TRY(remapId(name, map));
    r.name = name;
  }
  for (MarkerRec& r : data.markers) {
    StringIdx name = r.name, text = r.data;
    TACHYON_TRY(remapId(name, map));
    TACHYON_TRY(remapId(text, map));
    r.name = name;
    r.data = text;
  }
  for (PlotSampleRec& r : data.plots) {
    StringIdx name = r.name;
    TACHYON_TRY(remapId(name, map));
    r.name = name;
  }
  for (ThreadNameRec& r : data.threadNames) {
    StringIdx name = r.name;
    TACHYON_TRY(remapId(name, map));
    r.name = name;
  }

  TACHYON_TRY_ASSIGN(std::vector<ScopeEventRec> oldScopes, reader.readScopeEvents());
  TACHYON_TRY_ASSIGN(std::vector<FiberEventRec> oldFibers, reader.readFiberEvents());
  TACHYON_TRY_ASSIGN(std::vector<FrameMarkRec> oldMarks, reader.readFrameMarks());
  TACHYON_TRY_ASSIGN(std::vector<MarkerRec> oldMarkers, reader.readMarkers());
  TACHYON_TRY_ASSIGN(std::vector<PlotSampleRec> oldPlots, reader.readPlotSamples());
  TACHYON_TRY_ASSIGN(std::vector<ThreadNameRec> oldNames, reader.readThreadNames());
  report.existingInstrumentationRecords = oldScopes.size() + oldFibers.size() + oldMarks.size() +
                                          oldMarkers.size() + oldPlots.size() + oldNames.size();

  report.scopeEvents = data.scopes.size();
  report.fiberEvents = data.fibers.size();
  report.frameMarks = data.frameMarks.size();
  report.markers = data.markers.size();
  report.plots = data.plots.size();
  report.threadNames = data.threadNames.size();

  const std::vector<ScopeEventRec> scopes = unionSorted(std::move(oldScopes), std::move(data.scopes));
  const std::vector<FiberEventRec> fibers = unionSorted(std::move(oldFibers), std::move(data.fibers));
  const std::vector<FrameMarkRec> marks = unionSorted(std::move(oldMarks), std::move(data.frameMarks));
  const std::vector<MarkerRec> markers = unionSorted(std::move(oldMarkers), std::move(data.markers));
  const std::vector<PlotSampleRec> plots = unionSorted(std::move(oldPlots), std::move(data.plots));
  const std::vector<ThreadNameRec> names = unionSorted(std::move(oldNames), std::move(data.threadNames));
  if (!scopes.empty()) writer.append(std::span<const ScopeEventRec>(scopes));
  if (!fibers.empty()) writer.append(std::span<const FiberEventRec>(fibers));
  if (!marks.empty()) writer.append(std::span<const FrameMarkRec>(marks));
  if (!markers.empty()) writer.append(std::span<const MarkerRec>(markers));
  if (!plots.empty()) writer.append(std::span<const PlotSampleRec>(plots));
  if (!names.empty()) writer.append(std::span<const ThreadNameRec>(names));

  const bool haveInstrumentation = report.existingInstrumentationRecords + report.scopeEvents +
                                       report.fiberEvents + report.frameMarks + report.markers +
                                       report.plots + report.threadNames >
                                   0;
  if (haveInstrumentation) writer.addFlags(kFlagHasInstrumentation);

  auto previousInfo = reader.readCaptureInfo();
  const std::string previous = previousInfo.ok() ? *previousInfo : std::string();
  writer.appendCaptureInfo(buildCaptureInfo(previousInfo.ok() ? &previous : nullptr, report, data));

  TACHYON_TRY(writer.finish());

  readerHolder.reset();
  fs::rename(tmp, outPath, ec);
  if (ec)
    return makeError(ErrorCode::IoError,
                     "cannot move the merged trace to " + outPath.string() + ": " + ec.message());
  guard.armed = false;
  report.wroteOutput = true;
  return report;
}

ScopeBalance checkScopeBalance(std::span<const ScopeEventRec> events) {
  struct PerThread {
    uint64_t depth = 0;
    uint64_t lastTs = 0;
  };
  std::unordered_map<uint32_t, PerThread> threads;
  ScopeBalance result;
  for (const ScopeEventRec& rec : events) {
    const ScopeEventRec r = rec;  // packed: work on an aligned copy
    PerThread& t = threads[r.tid];
    if (r.ts < t.lastTs) ++result.outOfOrder;
    t.lastTs = std::max(t.lastTs, static_cast<uint64_t>(r.ts));
    if (r.kind == kScopeBegin) {
      ++result.begins;
      ++t.depth;
    } else {
      ++result.ends;
      if (t.depth == 0) {
        ++result.unmatchedEnds;
      } else {
        --t.depth;
      }
    }
  }
  result.threads = threads.size();
  for (const auto& [tid, t] : threads) result.unclosedBegins += t.depth;
  return result;
}

}  // namespace tachyon::inst
