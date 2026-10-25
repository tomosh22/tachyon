#include "inst/decoder.h"

#include "inst/frame_stream.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace tachyon::inst {

namespace {

constexpr size_t kReadBlockBytes = 1 << 20;

template <typename P>
bool load(const uint8_t* payload, size_t len, P& out) {
  if (len < sizeof(P)) return false;
  std::memcpy(&out, payload, sizeof(P));
  return true;
}

struct ThreadNameKey {
  uint32_t tid;
  uint64_t ts;
  uint32_t name;
  bool operator==(const ThreadNameKey&) const = default;
};
struct ThreadNameKeyHash {
  size_t operator()(const ThreadNameKey& k) const {
    uint64_t h = k.ts * 0x9E3779B97F4A7C15ull;
    h ^= (static_cast<uint64_t>(k.tid) << 32 | k.name) + 0x7F4A7C15ull + (h << 6) + (h >> 2);
    return static_cast<size_t>(h);
  }
};

struct ThreadState {
  uint64_t lastTs = 0;
  bool inFiber = false;
  uint64_t fiberId = 0;
  uint32_t depth = 0;  // open scopes outside any fiber
};

}  // namespace

struct InstDecoder::State {
  DecoderConfig config;
  DecodedInstrumentation out;

  // Per stream.
  bool haveStream = false;
  std::unordered_map<uint64_t, StringIdx> keyToString;
  std::unique_ptr<IClockMapper> mapper;
  // Per source.
  std::string label;
  FrameAssembler assembler;
  bool sawFirstFrame = false;
  bool firstFrameBad = false;
  uint64_t sourceBytes = 0;
  // Whole decode: threads outlive streams (a stream switch keeps the rings' contents in order).
  std::unordered_map<uint32_t, ThreadState> threads;
  std::unordered_map<uint64_t, uint32_t> fiberDepth;
  std::unordered_set<ThreadNameKey, ThreadNameKeyHash> seenThreadNames;
  std::vector<std::string> notes;
  StringIdx unresolvedString;
  StringIdx droppedName;
  bool anyRecord = false;

  void noteTs(uint64_t ts) {
    if (!anyRecord) {
      out.minTs = out.maxTs = ts;
      anyRecord = true;
    } else {
      out.minTs = std::min(out.minTs, ts);
      out.maxTs = std::max(out.maxTs, ts);
    }
  }

  // Trace-clock timestamp for a per-thread event: mapped, then never earlier than the thread's previous one.
  uint64_t threadTs(uint32_t tid, uint64_t raw) {
    uint64_t ts = mapper ? mapper->toTraceTicks(raw) : raw;
    ThreadState& t = threads[tid];
    if (ts < t.lastTs) {
      ts = t.lastTs;
      ++out.stats.clampedTimestamps;
    }
    t.lastTs = ts;
    noteTs(ts);
    return ts;
  }
  uint64_t plainTs(uint64_t raw) {
    const uint64_t ts = mapper ? mapper->toTraceTicks(raw) : raw;
    noteTs(ts);
    return ts;
  }

  StringIdx resolve(uint64_t key) {
    const auto it = keyToString.find(key);
    if (it != keyToString.end()) return it->second;
    ++out.stats.unresolvedStrings;
    if (!unresolvedString.valid()) unresolvedString = out.strings.intern("<unresolved string>");
    return unresolvedString;
  }
  StringIdx internInline(const uint8_t* bytes, size_t len) {
    if (len == 0) return StringIdx(0);
    return out.strings.intern(std::string_view(reinterpret_cast<const char*>(bytes), len));
  }

  void beginSource(const std::string& name) {
    label = name;
    assembler = FrameAssembler();
    sawFirstFrame = false;
    firstFrameBad = false;
    sourceBytes = 0;
    haveStream = false;
    ++out.stats.sources;
  }

  // False when the frame is not a StreamHeader at all (wrong size or magic).
  bool onStreamHeader(const uint8_t* payload, size_t len) {
    StreamHeaderPayload sh{};
    StreamInfo info;
    info.index = static_cast<uint32_t>(out.streams.size());
    info.source = label;
    if (!load(payload, len, sh) || sh.magic != kStreamMagic) {
      ++out.stats.malformedFrames;
      return false;
    }
    info.wireVersion = sh.wire_version;
    info.pid = sh.pid;
    info.platform = sh.platform;
    info.clockFrequencyHz = sh.clock_frequency_hz;
    info.processStartTs = sh.process_start_ts;
    info.apiVersion = sh.api_version;
    keyToString.clear();
    if (sh.wire_version != kWireVersion) {
      haveStream = false;
      notes.push_back(label + ": stream with unsupported wire version " + std::to_string(sh.wire_version) +
                      " skipped");
      out.streams.push_back(info);
      return true;
    }
    if (config.clockMapperFactory) {
      mapper = config.clockMapperFactory(info);
    } else {
      ClockContext ctx;
      ctx.streamClockHz = sh.clock_frequency_hz;
      ctx.traceClockHz = config.traceClockHz;
      ctx.calibrations = config.calibrations;
      std::string note;
      mapper = makeClockMapper(ctx, &note);
      if (!note.empty()) notes.push_back(label + ": " + note);
    }
    if (!mapper) mapper = std::make_unique<IdentityClockMapper>();
    info.clockMapper = mapper->describe();
    haveStream = true;
    ++out.stats.streams;
    out.streams.push_back(info);
    return true;
  }

  void onScopeBegin(const uint8_t* payload, size_t len) {
    ScopeBeginPayload p{};
    if (!load(payload, len, p) || len < sizeof(p) + p.data_len) {
      ++out.stats.malformedFrames;
      return;
    }
    trace::ScopeEventRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = threadTs(p.tid, p.ts);
    rec.tid = p.tid;
    rec.kind = trace::kScopeBegin;
    rec.id = resolve(p.id_key);
    rec.data = internInline(payload + sizeof(p), p.data_len);
    rec.color_rgba = p.color_rgba;
    rec.caller_addr = p.caller_addr;
    out.scopes.push_back(rec);
    ++out.stats.scopeBegins;
    ThreadState& t = threads[p.tid];
    if (t.inFiber) {
      ++fiberDepth[t.fiberId];
    } else {
      ++t.depth;
    }
  }

  void onScopeEnd(const uint8_t* payload, size_t len) {
    ScopeEndPayload p{};
    if (!load(payload, len, p)) {
      ++out.stats.malformedFrames;
      return;
    }
    trace::ScopeEventRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = threadTs(p.tid, p.ts);
    rec.tid = p.tid;
    rec.kind = trace::kScopeEnd;
    out.scopes.push_back(rec);
    ++out.stats.scopeEnds;
    ThreadState& t = threads[p.tid];
    uint32_t& depth = t.inFiber ? fiberDepth[t.fiberId] : t.depth;
    if (depth == 0) {
      ++out.stats.unmatchedScopeEnds;
    } else {
      --depth;
    }
  }

  void onFiberEvent(const uint8_t* payload, size_t len) {
    FiberEventPayload p{};
    if (!load(payload, len, p)) {
      ++out.stats.malformedFrames;
      return;
    }
    trace::FiberEventRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = threadTs(p.tid, p.ts);
    rec.tid = p.tid;
    rec.kind = p.kind;
    rec.fiber_id = p.fiber_id;
    rec.other_fiber_id = p.other_fiber_id;
    out.fibers.push_back(rec);
    ++out.stats.fiberEvents;
    ThreadState& t = threads[p.tid];
    switch (static_cast<FiberEventKind>(p.kind)) {
      case FiberEventKind::SwitchBegin:
        // The thread leaves p.fiber_id: scopes opened outside any registered fiber belong to it from now on.
        if (!t.inFiber) {
          fiberDepth[p.fiber_id] = t.depth;
          t.depth = 0;
        }
        t.inFiber = false;
        break;
      case FiberEventKind::SwitchEnd:
        t.inFiber = true;
        t.fiberId = p.fiber_id;
        break;
      case FiberEventKind::Unregister: {
        const auto it = fiberDepth.find(p.fiber_id);
        if (it != fiberDepth.end()) {
          out.stats.unclosedScopes += it->second;
          fiberDepth.erase(it);
        }
        break;
      }
      case FiberEventKind::Register: break;
    }
  }

  void onFrameMark(const uint8_t* payload, size_t len) {
    FrameMarkPayload p{};
    if (!load(payload, len, p)) {
      ++out.stats.malformedFrames;
      return;
    }
    trace::FrameMarkRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = threadTs(p.tid, p.ts);
    rec.tid = p.tid;
    rec.name = resolve(p.name_key);
    rec.frame_number = p.frame_number;
    out.frameMarks.push_back(rec);
    ++out.stats.frameMarks;
  }

  void onMarker(const uint8_t* payload, size_t len) {
    MarkerPayload p{};
    if (!load(payload, len, p) || len < sizeof(p) + p.data_len) {
      ++out.stats.malformedFrames;
      return;
    }
    trace::MarkerRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = threadTs(p.tid, p.ts);
    rec.tid = p.tid;
    rec.name = resolve(p.name_key);
    rec.data = internInline(payload + sizeof(p), p.data_len);
    rec.color_rgba = p.color_rgba;
    out.markers.push_back(rec);
    ++out.stats.markers;
  }

  void onPlot(const uint8_t* payload, size_t len) {
    PlotPayload p{};
    if (!load(payload, len, p)) {
      ++out.stats.malformedFrames;
      return;
    }
    trace::PlotSampleRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = plainTs(p.ts);
    rec.name = resolve(p.name_key);
    rec.value = p.value;
    out.plots.push_back(rec);
    ++out.stats.plots;
  }

  void onThreadName(const uint8_t* payload, size_t len) {
    ThreadNamePayload p{};
    if (!load(payload, len, p) || len < sizeof(p) + p.name_len) {
      ++out.stats.malformedFrames;
      return;
    }
    const StringIdx name = internInline(payload + sizeof(p), p.name_len);
    // Not clamped: a stream switch re-sends every name with its original timestamp.
    const uint64_t ts = mapper ? mapper->toTraceTicks(p.ts) : p.ts;
    if (!seenThreadNames.insert({p.tid, ts, name.get()}).second) {
      ++out.stats.duplicateThreadNames;
      return;
    }
    noteTs(ts);
    trace::ThreadNameRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = ts;
    rec.tid = p.tid;
    rec.name = name;
    out.threadNames.push_back(rec);
    ++out.stats.threadNames;
  }

  void onDropped(const uint8_t* payload, size_t len) {
    DroppedPayload p{};
    if (!load(payload, len, p)) {
      ++out.stats.malformedFrames;
      return;
    }
    ++out.stats.droppedFrames;
    out.stats.droppedRecords += p.count;
    if (!droppedName.valid()) droppedName = out.strings.intern(kDroppedMarkerName);
    trace::MarkerRec rec;
    std::memset(static_cast<void*>(&rec), 0, sizeof(rec));
    rec.ts = threadTs(p.tid, p.ts);
    rec.tid = p.tid;
    rec.name = droppedName;
    rec.data = out.strings.intern(std::to_string(p.count));
    rec.color_rgba = kDroppedMarkerColor;
    out.markers.push_back(rec);
    ++out.stats.markers;
  }

  void onFrame(const uint8_t* frame, size_t bytes) {
    FrameHeader fh;
    std::memcpy(&fh, frame, sizeof(fh));
    const uint8_t* payload = frame + sizeof(fh);
    const size_t len = bytes - sizeof(fh);
    const auto type = static_cast<FrameType>(fh.type);
    const bool first = !sawFirstFrame;
    sawFirstFrame = true;
    ++out.stats.frames;
    if (type == FrameType::StreamHeader) {
      if (!onStreamHeader(payload, len) && first) firstFrameBad = true;
      return;
    }
    if (first) firstFrameBad = true;
    if (!haveStream) {
      // Data before any usable header (or inside a stream of an unsupported version).
      ++out.stats.malformedFrames;
      return;
    }
    switch (type) {
      case FrameType::StringDef: {
        StringDefPayload p{};
        if (!load(payload, len, p) || len < sizeof(p) + p.len) {
          ++out.stats.malformedFrames;
          return;
        }
        keyToString[p.ptr_key] = internInline(payload + sizeof(p), p.len);
        break;
      }
      case FrameType::ScopeBegin: onScopeBegin(payload, len); break;
      case FrameType::ScopeEnd: onScopeEnd(payload, len); break;
      case FrameType::FiberEvent: onFiberEvent(payload, len); break;
      case FrameType::FrameMark: onFrameMark(payload, len); break;
      case FrameType::Marker: onMarker(payload, len); break;
      case FrameType::Plot: onPlot(payload, len); break;
      case FrameType::ThreadName: onThreadName(payload, len); break;
      case FrameType::Dropped: onDropped(payload, len); break;
      case FrameType::StreamHeader: break;
      default: ++out.stats.unknownFrames; break;
    }
  }

  Result<void> feed(const uint8_t* data, size_t size) {
    out.stats.bytes += size;
    assembler.feed(data, size, [this](const uint8_t* f, size_t n) { onFrame(f, n); });
    sourceBytes += size;
    // Garbage from the very first byte is not a stream either.
    if (firstFrameBad || (!sawFirstFrame && assembler.corrupt()))
      return makeError(ErrorCode::CorruptData,
                       label + ": not an instrumentation stream (no StreamHeader first)");
    return ok();
  }

  void endSource() {
    out.stats.truncatedTailBytes += assembler.pendingBytes();
    out.stats.corruptBytes += assembler.discardedBytes();
    if (assembler.pendingBytes() > 0)
      notes.push_back(label + ": " + std::to_string(assembler.pendingBytes()) +
                      " bytes of an incomplete last frame were ignored (truncated stream)");
    if (assembler.corrupt())
      notes.push_back(label + ": stream corrupt (implausible frame length); " +
                      std::to_string(assembler.discardedBytes()) + " bytes ignored");
  }
};

InstDecoder::InstDecoder(DecoderConfig config) : m_state(std::make_shared<State>()) {
  m_state->config = std::move(config);
}

Result<void> InstDecoder::addFile(const std::filesystem::path& file, uint64_t maxBytes) {
  State& s = *m_state;
  std::ifstream in(file, std::ios::binary);
  if (!in) return makeError(ErrorCode::NotFound, "cannot open instrumentation file " + file.string());
  s.beginSource(file.string());
  std::vector<uint8_t> block(kReadBlockBytes);
  uint64_t remaining = maxBytes;
  Result<void> status = ok();
  while (remaining > 0 && status.ok()) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, block.size()));
    in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(want));
    const size_t got = static_cast<size_t>(in.gcount());
    if (got == 0) break;
    remaining -= got;
    status = s.feed(block.data(), got);
    if (!in) break;
  }
  s.endSource();
  if (status.ok() && s.sourceBytes == 0) s.notes.push_back(file.string() + ": empty file");
  return status;
}

Result<void> InstDecoder::addBytes(std::span<const uint8_t> bytes, const std::string& label) {
  State& s = *m_state;
  s.beginSource(label);
  Result<void> status = bytes.empty() ? ok() : s.feed(bytes.data(), bytes.size());
  s.endSource();
  return status;
}

DecodedInstrumentation InstDecoder::finish() {
  State& s = *m_state;
  DecodedInstrumentation out = std::move(s.out);
  const auto byTs = [](const auto& a, const auto& b) {
    // Copy the keys out: records are packed.
    const uint64_t ta = a.ts, tb = b.ts;
    return ta < tb;
  };
  std::stable_sort(out.scopes.begin(), out.scopes.end(), byTs);
  std::stable_sort(out.fibers.begin(), out.fibers.end(), byTs);
  std::stable_sort(out.frameMarks.begin(), out.frameMarks.end(), byTs);
  std::stable_sort(out.markers.begin(), out.markers.end(), byTs);
  std::stable_sort(out.plots.begin(), out.plots.end(), byTs);
  std::stable_sort(out.threadNames.begin(), out.threadNames.end(), byTs);

  for (const auto& [tid, t] : s.threads) out.stats.unclosedScopes += t.depth;
  for (const auto& [fiber, depth] : s.fiberDepth) out.stats.unclosedScopes += depth;

  out.warnings = std::move(s.notes);
  const DecodeStats& st = out.stats;
  if (st.droppedRecords > 0)
    out.warnings.push_back(std::to_string(st.droppedRecords) +
                           " instrumentation records were dropped (ring or spool overflow); see the " +
                           kDroppedMarkerName + " markers");
  if (st.unresolvedStrings > 0)
    out.warnings.push_back(std::to_string(st.unresolvedStrings) + " string references had no StringDef");
  if (st.malformedFrames > 0)
    out.warnings.push_back(std::to_string(st.malformedFrames) + " malformed frames were skipped");
  if (st.unknownFrames > 0)
    out.warnings.push_back(std::to_string(st.unknownFrames) + " frames of an unknown type were skipped");
  if (st.clampedTimestamps > 0)
    out.warnings.push_back(std::to_string(st.clampedTimestamps) +
                           " timestamps went backwards within a thread and were clamped");
  if (st.unmatchedScopeEnds > 0)
    out.warnings.push_back(std::to_string(st.unmatchedScopeEnds) +
                           " scope ends had no open scope on their thread (unbalanced Begin/End)");
  if (st.unclosedScopes > 0)
    out.warnings.push_back(std::to_string(st.unclosedScopes) +
                           " scopes were still open at the end of the stream (unbalanced Begin/End, or "
                           "threads inside a scope when the capture stopped)");
  return out;
}

}  // namespace tachyon::inst
