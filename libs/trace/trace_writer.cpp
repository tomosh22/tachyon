#include "trace/trace_writer.h"

#include "core/hash.h"
#include "core/span_util.h"
#include "trace/string_table.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tachyon::trace {

namespace {

constexpr int kZstdLevel = 3;
constexpr size_t kMinCompressSize = 64;

struct FrameKey {
  uint32_t module;
  uint64_t addr;
  bool operator==(const FrameKey&) const = default;
};
struct FrameKeyHash {
  size_t operator()(const FrameKey& k) const noexcept {
    return static_cast<size_t>(core::hashU64(k.addr ^ core::hashU64(k.module)));
  }
};

}  // namespace

struct TraceWriter::Impl {
  std::fstream file;
  FileHeader header;
  uint64_t offset = 0;
  bool finished = false;
  std::optional<Error> error;
  std::vector<FooterRec> footer;

  StringTable strings;
  std::vector<FrameRec> frames;
  std::unordered_map<FrameKey, uint32_t, FrameKeyHash> frameIndex;
  CallstackInterner callstacks;
  size_t emittedStrings = 0;
  size_t emittedFrames = 0;
  size_t emittedNodes = 0;

  std::vector<std::byte> scratch;
  std::vector<std::byte> compressed;

  void fail(std::string message) {
    if (!error) error = makeError(ErrorCode::IoError, std::move(message));
  }

  void writeRaw(const void* data, size_t size) {
    if (error) return;
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file) fail("write to trace file failed");
    offset += size;
  }

  void writeHeaderAtStart() {
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!file) fail("failed to write file header");
  }

  // Writes one chunk whose payload is in `payload`.
  void writeChunk(ChunkType type, std::span<const std::byte> payload, uint64_t recordCount, bool compress) {
    if (error || finished) return;
    ChunkHeader ch;
    ch.type = static_cast<uint32_t>(type);
    ch.version = kChunkSchemaVersion;
    ch.uncompressed_size = payload.size();
    ch.record_count = recordCount;
    const std::byte* body = payload.data();
    size_t bodySize = payload.size();
    ch.compression = kCompressionNone;
    if (compress && payload.size() >= kMinCompressSize) {
      compressed.resize(ZSTD_compressBound(payload.size()));
      const size_t n =
          ZSTD_compress(compressed.data(), compressed.size(), payload.data(), payload.size(), kZstdLevel);
      if (!ZSTD_isError(n) && n < payload.size()) {
        ch.compression = kCompressionZstd;
        body = compressed.data();
        bodySize = n;
      }
    }
    ch.compressed_size = bodySize;
    footer.push_back(FooterRec{ch.type, ch.version, offset, recordCount});
    writeRaw(&ch, sizeof(ch));
    if (bodySize) writeRaw(body, bodySize);
  }

  template <typename T>
  void writeRecords(ChunkType type, std::span<const T> records) {
    for (size_t i = 0; i < records.size(); i += kMaxRecordsPerChunk) {
      const size_t n = std::min<size_t>(kMaxRecordsPerChunk, records.size() - i);
      writeChunk(type, std::as_bytes(records.subspan(i, n)), n, true);
    }
  }

  void flushTables() {
    while (emittedStrings < strings.size()) {
      const size_t n = std::min<size_t>(kMaxRecordsPerChunk, strings.size() - emittedStrings);
      scratch.clear();
      strings.serialize(scratch, emittedStrings, n);
      writeChunk(ChunkType::Strings, scratch, n, true);
      emittedStrings += n;
    }
    while (emittedFrames < frames.size()) {
      const size_t n = std::min<size_t>(kMaxRecordsPerChunk, frames.size() - emittedFrames);
      writeChunk(ChunkType::Frames,
                 std::as_bytes(std::span<const FrameRec>(frames).subspan(emittedFrames, n)), n, true);
      emittedFrames += n;
    }
    const auto nodes = callstacks.nodes();
    while (emittedNodes < nodes.size()) {
      const size_t n = std::min<size_t>(kMaxRecordsPerChunk, nodes.size() - emittedNodes);
      writeChunk(ChunkType::CallstackNodes, std::as_bytes(nodes.subspan(emittedNodes, n)), n, true);
      emittedNodes += n;
    }
  }

  template <typename T>
  void appendRecords(ChunkType type, std::span<const T> records) {
    flushTables();
    writeRecords(type, records);
  }

  void appendSamples(const SampleBatch& s) {
    if (!s.consistent()) {
      fail("SampleBatch arrays have different lengths");
      return;
    }
    flushTables();
    for (size_t i = 0; i < s.size(); i += kMaxRecordsPerChunk) {
      const size_t n = std::min<size_t>(kMaxRecordsPerChunk, s.size() - i);
      scratch.clear();
      scratch.reserve(n * kSampleBytes);
      core::appendBytes(scratch, std::as_bytes(std::span<const uint64_t>(s.ts).subspan(i, n)));
      core::appendBytes(scratch, std::as_bytes(std::span<const uint32_t>(s.tid).subspan(i, n)));
      core::appendBytes(scratch, std::as_bytes(std::span<const CallstackIdx>(s.callstack).subspan(i, n)));
      core::appendBytes(scratch, std::as_bytes(std::span<const uint16_t>(s.cpu).subspan(i, n)));
      core::appendBytes(scratch, std::as_bytes(std::span<const uint16_t>(s.flags).subspan(i, n)));
      writeChunk(ChunkType::Samples, scratch, n, true);
    }
  }
};

TraceWriter::TraceWriter(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
TraceWriter::TraceWriter(TraceWriter&&) noexcept = default;
TraceWriter& TraceWriter::operator=(TraceWriter&&) noexcept = default;
TraceWriter::~TraceWriter() = default;

Result<TraceWriter> TraceWriter::open(const std::filesystem::path& path, FileHeader header) {
  auto impl = std::make_unique<Impl>();
  impl->file.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
  if (!impl->file) return makeError(ErrorCode::IoError, "cannot create trace file " + path.string());
  std::memcpy(header.magic, kMagic, sizeof(kMagic));
  header.format_version = kFormatVersion;
  header.flags |= kFlagPartial;
  header.footer_offset = 0;
  impl->header = header;
  impl->writeHeaderAtStart();
  impl->offset = sizeof(FileHeader);
  if (impl->error) return *impl->error;
  return TraceWriter(std::move(impl));
}

StringIdx TraceWriter::internString(std::string_view s) {
  return m_impl->strings.intern(s);
}

FrameIdx TraceWriter::internFrame(ModuleIdx module, uint64_t addrOrRva) {
  Impl& d = *m_impl;
  const auto [it, inserted] =
      d.frameIndex.try_emplace(FrameKey{module.get(), addrOrRva}, static_cast<uint32_t>(d.frames.size()));
  if (inserted) d.frames.push_back(FrameRec{module, addrOrRva});
  return FrameIdx(it->second);
}

CallstackIdx TraceWriter::internCallstack(std::span<const FrameIdx> leafToRoot) {
  return m_impl->callstacks.internStack(leafToRoot);
}

CallstackIdx TraceWriter::internCallstackNode(CallstackIdx parent, FrameIdx frame) {
  return m_impl->callstacks.intern(parent, frame);
}

void TraceWriter::append(const SampleBatch& samples) {
  m_impl->appendSamples(samples);
}
void TraceWriter::append(std::span<const ProcessRec> r) {
  m_impl->appendRecords(ChunkType::Processes, r);
}
void TraceWriter::append(std::span<const ThreadRec> r) {
  m_impl->appendRecords(ChunkType::Threads, r);
}
void TraceWriter::append(std::span<const ModuleRec> r) {
  m_impl->appendRecords(ChunkType::Modules, r);
}
void TraceWriter::append(std::span<const ContextSwitchRec> r) {
  m_impl->appendRecords(ChunkType::ContextSwitches, r);
}
void TraceWriter::append(std::span<const ReadyRec> r) {
  m_impl->appendRecords(ChunkType::ReadyEvents, r);
}
void TraceWriter::append(std::span<const ScopeEventRec> r) {
  m_impl->appendRecords(ChunkType::ScopeEvents, r);
}
void TraceWriter::append(std::span<const FiberEventRec> r) {
  m_impl->appendRecords(ChunkType::FiberEvents, r);
}
void TraceWriter::append(std::span<const FrameMarkRec> r) {
  m_impl->appendRecords(ChunkType::FrameMarks, r);
}
void TraceWriter::append(std::span<const MarkerRec> r) {
  m_impl->appendRecords(ChunkType::Markers, r);
}
void TraceWriter::append(std::span<const PlotSampleRec> r) {
  m_impl->appendRecords(ChunkType::PlotSamples, r);
}
void TraceWriter::append(std::span<const ThreadNameRec> r) {
  m_impl->appendRecords(ChunkType::ThreadNames, r);
}
void TraceWriter::append(std::span<const ClockCalibrationRec> r) {
  m_impl->appendRecords(ChunkType::ClockCalibration, r);
}

void TraceWriter::appendCaptureInfo(std::string_view json) {
  m_impl->flushTables();
  m_impl->writeChunk(ChunkType::CaptureInfo, std::as_bytes(std::span<const char>(json.data(), json.size())),
                     1, true);
}

void TraceWriter::setTimeRange(uint64_t startTs, uint64_t endTs) {
  m_impl->header.start_ts = startTs;
  m_impl->header.end_ts = endTs;
}

void TraceWriter::addFlags(uint32_t flags) {
  m_impl->header.flags |= flags;
}

Result<void> TraceWriter::finish() {
  Impl& d = *m_impl;
  if (d.finished) return d.error ? Result<void>(*d.error) : ok();
  d.flushTables();
  const uint64_t footerOffset = d.offset;
  const std::vector<FooterRec> recs = d.footer;  // the footer chunk lists only the preceding chunks
  d.writeChunk(ChunkType::Footer, std::as_bytes(std::span<const FooterRec>(recs)), recs.size(), false);
  d.finished = true;
  d.footer.pop_back();
  d.header.footer_offset = footerOffset;
  d.header.flags &= ~kFlagPartial;
  if (!d.error) {
    d.writeHeaderAtStart();
    d.file.flush();
    if (!d.file) d.fail("flush of trace file failed");
  }
  d.file.close();
  if (d.error) return *d.error;
  return ok();
}

const CallstackInterner& TraceWriter::callstacks() const {
  return m_impl->callstacks;
}

size_t TraceWriter::chunkCount() const {
  return m_impl->footer.size();
}

}  // namespace tachyon::trace
