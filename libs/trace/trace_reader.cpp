#include "trace/trace_reader.h"

#include "core/span_util.h"

#include <zstd.h>

#include <cstring>
#include <new>

namespace tachyon::trace {

namespace {

constexpr uint64_t kMaxUncompressedChunk = uint64_t(1) << 30;  // 1 GiB sanity cap per chunk payload
constexpr uint64_t kHeaderSize = sizeof(ChunkHeader);

bool isKnownType(uint32_t t) {
  return (t >= 1 && t <= static_cast<uint32_t>(ChunkType::CaptureInfo)) ||
         t == static_cast<uint32_t>(ChunkType::Footer);
}

// Structural validation of a chunk header located at `offset` in a file of `fileSize` bytes.
bool chunkHeaderValid(const ChunkHeader& h, uint64_t offset, uint64_t fileSize) {
  if (!isKnownType(h.type)) return false;
  if (h.compression > kCompressionZstd) return false;
  if (h.compression == kCompressionNone && h.compressed_size != h.uncompressed_size) return false;
  if (h.uncompressed_size > kMaxUncompressedChunk) return false;
  if (offset > fileSize || fileSize - offset < kHeaderSize) return false;
  if (h.compressed_size > fileSize - offset - kHeaderSize) return false;
  return true;
}

}  // namespace

TraceReader::TraceReader() = default;
TraceReader::TraceReader(TraceReader&&) noexcept = default;
TraceReader& TraceReader::operator=(TraceReader&&) noexcept = default;
TraceReader::~TraceReader() = default;

Result<TraceReader> TraceReader::open(const std::filesystem::path& path) {
  TraceReader r;
  r.m_file = std::make_unique<std::ifstream>(path, std::ios::in | std::ios::binary);
  if (!*r.m_file) return makeError(ErrorCode::NotFound, "cannot open trace file " + path.string());
  r.m_file->seekg(0, std::ios::end);
  const std::streamoff size = r.m_file->tellg();
  if (size < static_cast<std::streamoff>(sizeof(FileHeader)))
    return makeError(ErrorCode::CorruptData, "file is smaller than a trace header");
  r.m_fileSize = static_cast<uint64_t>(size);
  r.m_file->seekg(0);
  r.m_file->read(reinterpret_cast<char*>(&r.m_header), sizeof(FileHeader));
  if (!*r.m_file) return makeError(ErrorCode::IoError, "failed to read trace header");
  if (std::memcmp(r.m_header.magic, kMagic, sizeof(kMagic)) != 0)
    return makeError(ErrorCode::CorruptData, "bad trace magic");
  if (r.m_header.format_version != kFormatVersion)
    return makeError(ErrorCode::Unsupported, "unsupported trace format version");

  bool haveFooter = false;
  if (r.m_header.footer_offset != 0) haveFooter = r.readFooter(r.m_fileSize).ok();
  if (!haveFooter) {
    r.m_chunks.clear();
    r.scanChunks(r.m_fileSize);
    r.m_header.flags |= kFlagPartial;
    r.m_header.footer_offset = 0;
  }
  return r;
}

Result<void> TraceReader::readFooter(uint64_t fileSize) {
  const uint64_t off = m_header.footer_offset;
  if (off < sizeof(FileHeader) || off > fileSize || fileSize - off < kHeaderSize)
    return makeError(ErrorCode::CorruptData, "footer offset out of range");
  ChunkHeader h;
  m_file->clear();
  m_file->seekg(static_cast<std::streamoff>(off));
  m_file->read(reinterpret_cast<char*>(&h), sizeof(h));
  if (!*m_file) return makeError(ErrorCode::IoError, "failed to read footer header");
  if (h.type != static_cast<uint32_t>(ChunkType::Footer) || h.compression != kCompressionNone ||
      !chunkHeaderValid(h, off, fileSize) || h.uncompressed_size % sizeof(FooterRec) != 0 ||
      h.uncompressed_size / sizeof(FooterRec) != h.record_count)
    return makeError(ErrorCode::CorruptData, "invalid footer chunk");
  std::vector<FooterRec> recs(h.record_count);
  if (h.record_count) {
    m_file->read(reinterpret_cast<char*>(recs.data()), static_cast<std::streamsize>(h.uncompressed_size));
    if (!*m_file) return makeError(ErrorCode::IoError, "failed to read footer");
  }
  for (const FooterRec& f : recs) {
    if (!isKnownType(f.type) || f.type == static_cast<uint32_t>(ChunkType::Footer) ||
        f.offset < sizeof(FileHeader) || f.offset > off || off - f.offset < kHeaderSize ||
        f.record_count > kMaxRecordsPerChunk)
      return makeError(ErrorCode::CorruptData, "invalid footer record");
  }
  m_chunks = std::move(recs);
  return ok();
}

void TraceReader::scanChunks(uint64_t fileSize) {
  uint64_t off = sizeof(FileHeader);
  m_file->clear();
  while (off <= fileSize && fileSize - off >= kHeaderSize) {
    ChunkHeader h;
    m_file->seekg(static_cast<std::streamoff>(off));
    m_file->read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!*m_file) break;
    if (!chunkHeaderValid(h, off, fileSize)) break;
    if (h.type == static_cast<uint32_t>(ChunkType::Footer)) break;
    if (h.record_count > kMaxRecordsPerChunk) break;
    m_chunks.push_back(FooterRec{h.type, h.version, off, h.record_count});
    off += kHeaderSize + h.compressed_size;
  }
  m_file->clear();
}

Result<void> TraceReader::readChunk(const FooterRec& chunk, std::vector<std::byte>& out) {
  ChunkHeader h;
  if (chunk.offset < sizeof(FileHeader) || chunk.offset > m_fileSize ||
      m_fileSize - chunk.offset < kHeaderSize)
    return makeError(ErrorCode::CorruptData, "chunk offset out of range");
  m_file->clear();
  m_file->seekg(static_cast<std::streamoff>(chunk.offset));
  m_file->read(reinterpret_cast<char*>(&h), sizeof(h));
  if (!*m_file) return makeError(ErrorCode::IoError, "failed to read chunk header");
  if (h.type != chunk.type || !chunkHeaderValid(h, chunk.offset, m_fileSize))
    return makeError(ErrorCode::CorruptData, "chunk header is invalid or does not match the footer");
  if (h.record_count != chunk.record_count)
    return makeError(ErrorCode::CorruptData, "chunk record count does not match the footer");
  if (h.type != static_cast<uint32_t>(ChunkType::Footer) && h.record_count > kMaxRecordsPerChunk)
    return makeError(ErrorCode::CorruptData, "chunk record count exceeds the per-chunk maximum");

  try {
    if (h.compression == kCompressionNone) {
      out.resize(h.uncompressed_size);
      if (h.uncompressed_size) {
        m_file->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(h.uncompressed_size));
        if (!*m_file) return makeError(ErrorCode::IoError, "failed to read chunk payload");
      }
      return ok();
    }
    std::vector<std::byte> comp(h.compressed_size);  // bounded by the file size (checked above)
    m_file->read(reinterpret_cast<char*>(comp.data()), static_cast<std::streamsize>(comp.size()));
    if (!*m_file) return makeError(ErrorCode::IoError, "failed to read chunk payload");
    // Never allocate the claimed size before the zstd frame agrees with it.
    const unsigned long long frameSize = ZSTD_getFrameContentSize(comp.data(), comp.size());
    if (frameSize == ZSTD_CONTENTSIZE_UNKNOWN || frameSize == ZSTD_CONTENTSIZE_ERROR ||
        frameSize != h.uncompressed_size)
      return makeError(ErrorCode::CorruptData, "zstd frame size does not match the chunk header");
    out.resize(h.uncompressed_size);
    const size_t n = ZSTD_decompress(out.data(), out.size(), comp.data(), comp.size());
    if (ZSTD_isError(n) || n != h.uncompressed_size)
      return makeError(ErrorCode::CorruptData, "zstd decompression failed or size mismatch");
    return ok();
  } catch (const std::bad_alloc&) {
    return makeError(ErrorCode::OutOfMemory, "cannot allocate chunk payload");
  }
}

Result<StringTable> TraceReader::readStrings() {
  StringTable table;
  std::vector<std::byte> payload;
  bool first = true;
  for (const FooterRec& c : m_chunks) {
    if (c.type != static_cast<uint32_t>(ChunkType::Strings)) continue;
    TACHYON_TRY(readChunk(c, payload));
    if (c.record_count == 0) return makeError(ErrorCode::CorruptData, "empty Strings chunk");
    if (first) {
      // The first Strings chunk starts with the pre-interned "" at ordinal 0; the table already has it.
      core::ByteReader r(payload);
      auto len = r.read<uint32_t>();
      if (!len || *len != 0) return makeError(ErrorCode::CorruptData, "string 0 is not empty");
      TACHYON_TRY(
          table.deserializeAppend(std::span<const std::byte>(payload).subspan(4), c.record_count - 1));
      first = false;
      continue;
    }
    TACHYON_TRY(table.deserializeAppend(payload, c.record_count));
  }
  return table;
}

Result<void> TraceReader::decodeSamples(const FooterRec& chunk, std::span<const std::byte> payload,
                                        SampleBatch& out) {
  if (chunk.record_count > kMaxRecordsPerChunk || payload.size() % kSampleBytes != 0 ||
      payload.size() / kSampleBytes != chunk.record_count)
    return makeError(ErrorCode::CorruptData, "Samples payload size does not match record count");
  const size_t n = static_cast<size_t>(chunk.record_count);
  const std::byte* p = payload.data();
  auto take = [&](auto& vec, size_t elemSize) {
    const size_t at = vec.size();
    vec.resize(at + n);
    if (n) std::memcpy(vec.data() + at, p, n * elemSize);
    p += n * elemSize;
  };
  take(out.ts, 8);
  take(out.tid, 4);
  take(out.callstack, 4);
  take(out.cpu, 2);
  take(out.flags, 2);
  return ok();
}

Result<SampleBatch> TraceReader::readSamples() {
  SampleBatch out;
  std::vector<std::byte> payload;
  for (const FooterRec& c : m_chunks) {
    if (c.type != static_cast<uint32_t>(ChunkType::Samples)) continue;
    TACHYON_TRY(readChunk(c, payload));
    TACHYON_TRY(decodeSamples(c, payload, out));
  }
  return out;
}

Result<std::string> TraceReader::readCaptureInfo() {
  std::vector<std::byte> payload;
  for (const FooterRec& c : m_chunks) {
    if (c.type != static_cast<uint32_t>(ChunkType::CaptureInfo)) continue;
    TACHYON_TRY(readChunk(c, payload));
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
  }
  return makeError(ErrorCode::NotFound, "no CaptureInfo chunk");
}

}  // namespace tachyon::trace
