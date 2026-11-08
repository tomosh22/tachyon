#include "trace/trace_writer.h"

#include "core/hash.h"
#include "core/platform.h"
#include "core/span_util.h"
#include "trace/string_table.h"

#include <zstd.h>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <algorithm>
#include <cstring>
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

// The output file, opened without ever following a link at the final path (WP-66b). POSIX: O_NOFOLLOW and an
// fstat before anything is truncated. Windows: FILE_FLAG_OPEN_REPARSE_POINT, and a reparse point is refused
// (needs Windows verification). Writes are unbuffered; the writer hands over whole chunks.
class OutFile {
 public:
  OutFile() = default;
  OutFile(const OutFile&) = delete;
  OutFile& operator=(const OutFile&) = delete;
  ~OutFile() { close(); }

  enum class Mode { CreateOrReuse, CreateNew, Existing };

  // CreateOrReuse: created, or an existing regular file reused (truncated). CreateNew: fails with Busy when
  // anything is there. Existing: `expected` names the regular file that must be there (truncated).
  Result<void> open(const std::filesystem::path& path, Mode mode, const TraceWriter::ReservedFile* expected) {
#if TACHYON_PLATFORM_WINDOWS
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    mode == Mode::Existing    ? OPEN_EXISTING
                    : mode == Mode::CreateNew ? CREATE_NEW
                                              : OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD err = GetLastError();
      if (mode == Mode::CreateNew && (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS))
        return makeError(ErrorCode::Busy, path.string() + " already exists");
      return makeError(ErrorCode::IoError, "cannot create trace file " + path.string() + " (Windows error " +
                                               std::to_string(err) + ")");
    }
    m_handle = h;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info) == 0) return fail(path, "cannot query the file");
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
      return fail(path, "is a link or a directory, not a regular file");
    if (mode != Mode::CreateNew && info.nNumberOfLinks != 1)
      return fail(path, "has more than one hard link; refusing to overwrite a file that may be shared");
    if (mode == Mode::CreateNew) {
      m_identity = {info.dwVolumeSerialNumber,
                    (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
      return ok();
    }
    if (expected != nullptr) {
      const uint64_t index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
      if (info.dwVolumeSerialNumber != expected->volume || index != expected->index)
        return fail(path, "is not the file that was reserved for it");
    }
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) == 0 || SetEndOfFile(h) == 0)
      return fail(path, "cannot truncate the file");
    return ok();
#else
    int fd;
    do {
      fd = ::open(path.c_str(),
                  O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY |
                      (mode == Mode::Existing ? 0 : O_CREAT) | (mode == Mode::CreateNew ? O_EXCL : 0),
                  0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
      if (mode == Mode::CreateNew && errno == EEXIST)
        return makeError(ErrorCode::Busy, path.string() + " already exists");
      return makeError(ErrorCode::IoError,
                       "cannot create trace file " + path.string() + ": " +
                           (errno == ELOOP ? "it is a symbolic link" : std::strerror(errno)));
    }
    m_fd = fd;
    struct stat st {};
    if (::fstat(fd, &st) != 0) return fail(path, "cannot stat the file");
    if (!S_ISREG(st.st_mode)) return fail(path, "is not a regular file");
    m_identity = {static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino)};
    if (mode == Mode::CreateNew) return ok();
    // A hard link planted at the name (rm + ln by another user) would make the truncate below destroy the
    // file it points to: an existing file that has other names is never overwritten.
    if (st.st_nlink != 1)
      return fail(path, "has more than one hard link; refusing to overwrite a file that may be shared");
    if (expected != nullptr && (static_cast<uint64_t>(st.st_dev) != expected->volume ||
                                static_cast<uint64_t>(st.st_ino) != expected->index))
      return fail(path, "is not the file that was reserved for it");
    if (::ftruncate(fd, 0) != 0) return fail(path, "cannot truncate the file");
    return ok();
#endif
  }

  bool write(const void* data, size_t size) {
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
#if TACHYON_PLATFORM_WINDOWS
      DWORD got = 0;
      const DWORD want = static_cast<DWORD>(std::min<size_t>(size, size_t{1} << 30));
      if (WriteFile(m_handle, p, want, &got, nullptr) == 0 || got == 0) return false;
#else
      const ssize_t got = ::write(m_fd, p, size);
      if (got < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (got == 0) return false;
#endif
      p += got;
      size -= static_cast<size_t>(got);
    }
    return true;
  }

  bool seekStart() {
#if TACHYON_PLATFORM_WINDOWS
    LARGE_INTEGER zero{};
    return SetFilePointerEx(m_handle, zero, nullptr, FILE_BEGIN) != 0;
#else
    return ::lseek(m_fd, 0, SEEK_SET) == 0;
#endif
  }

  TraceWriter::ReservedFile identity() const { return m_identity; }

  void close() {
#if TACHYON_PLATFORM_WINDOWS
    if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;
#else
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
#endif
  }

 private:
  TraceWriter::ReservedFile m_identity;
  Error fail(const std::filesystem::path& path, const char* what) {
    close();
    return makeError(ErrorCode::IoError, "trace file " + path.string() + " " + what);
  }

#if TACHYON_PLATFORM_WINDOWS
  HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
  int m_fd = -1;
#endif
};

}  // namespace

struct TraceWriter::Impl {
  OutFile file;
  FileHeader header;
  uint64_t offset = 0;
  bool finished = false;
  bool keepPartial = false;
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
    if (!file.write(data, size)) fail("write to trace file failed");
    offset += size;
  }

  void writeHeaderAtStart() {
    if (!file.seekStart() || !file.write(&header, sizeof(header))) fail("failed to write file header");
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
  return openImpl(path, header, nullptr);
}

Result<TraceWriter::ReservedFile> TraceWriter::reserveNew(const std::filesystem::path& path) {
  OutFile file;
  TACHYON_TRY(file.open(path, OutFile::Mode::CreateNew, nullptr));
  return file.identity();
}

Result<TraceWriter> TraceWriter::open(const std::filesystem::path& path, FileHeader header,
                                      const ReservedFile& reserved) {
  return openImpl(path, header, &reserved);
}

Result<TraceWriter> TraceWriter::openImpl(const std::filesystem::path& path, FileHeader header,
                                          const ReservedFile* reserved) {
  auto impl = std::make_unique<Impl>();
  if (auto opened =
          impl->file.open(path, reserved ? OutFile::Mode::Existing : OutFile::Mode::CreateOrReuse, reserved);
      !opened.ok())
    return opened.error();
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

void TraceWriter::keepPartialFlag() {
  m_impl->keepPartial = true;
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
  if (!d.keepPartial) d.header.flags &= ~kFlagPartial;
  if (!d.error) {
    d.writeHeaderAtStart();
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
