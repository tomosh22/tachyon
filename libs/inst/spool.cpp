#include "inst/spool.h"

#include <cstring>
#include <utility>

namespace tachyon::inst {

namespace {
constexpr size_t kBufferBytes = 256 * 1024;
}

Spool::Spool(std::filesystem::path path, std::ofstream file, uint64_t maxBytes)
    : m_path(std::move(path)), m_maxBytes(maxBytes), m_file(std::move(file)) {
  m_buffer.reserve(kBufferBytes);
}

Result<std::unique_ptr<Spool>> Spool::create(const std::filesystem::path& file, uint64_t maxBytes) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return makeError(ErrorCode::IoError, "cannot create spool file " + file.string());
  return std::unique_ptr<Spool>(new Spool(file, std::move(out), maxBytes));
}

Spool::~Spool() {
  (void)close();
}

Result<void> Spool::flushLocked() {
  if (!m_buffer.empty()) {
    m_file.write(reinterpret_cast<const char*>(m_buffer.data()),
                 static_cast<std::streamsize>(m_buffer.size()));
    m_buffer.clear();
  }
  m_file.flush();
  if (!m_file) {
    m_ioError = true;
    return makeError(ErrorCode::IoError, "write to spool file failed: " + m_path.string());
  }
  return ok();
}

void Spool::appendFrame(const uint8_t* frame, size_t bytes) {
  const FramePeek peek = peekFrame(frame, bytes);
  std::lock_guard lock(m_mutex);
  if (m_closed) return;
  if (!m_stats.overflowed && !m_ioError && m_stats.bytesWritten + bytes <= m_maxBytes) {
    m_buffer.insert(m_buffer.end(), frame, frame + bytes);
    m_stats.bytesWritten += bytes;
    ++m_stats.framesWritten;
    if (peek.hasTimestamp && peek.ts > m_lastTs) m_lastTs = peek.ts;
    if (m_buffer.size() >= kBufferBytes) (void)flushLocked();
    return;
  }
  // Past the cap (sticky), or the disk failed: drop and count.
  m_stats.overflowed = true;
  ++m_stats.framesDropped;
  if (peek.isRecord) {
    m_stats.recordsDropped += peek.type == FrameType::Dropped ? peek.dropped : 1;
    if (!m_haveFirstDrop) {
      m_haveFirstDrop = true;
      m_firstDropTs = peek.hasTimestamp ? peek.ts : m_lastTs;
      m_firstDropTid = peek.tid;
    }
  }
}

void Spool::noteTruncatedTail(size_t tailBytes) {
  std::lock_guard lock(m_mutex);
  m_stats.truncatedTailBytes += tailBytes;
}

void Spool::noteCorrupt(uint64_t discarded) {
  std::lock_guard lock(m_mutex);
  m_stats.corruptBytes += discarded;
}

void Spool::noteConnection() {
  std::lock_guard lock(m_mutex);
  ++m_stats.connections;
}

Result<uint64_t> Spool::flush() {
  std::lock_guard lock(m_mutex);
  if (m_closed) return m_stats.bytesWritten;
  TACHYON_TRY(flushLocked());
  return m_stats.bytesWritten;
}

Result<void> Spool::close() {
  std::lock_guard lock(m_mutex);
  if (m_closed) return ok();
  m_closed = true;
  if (m_stats.recordsDropped > 0 && !m_ioError) {
    // The cap does not apply to this one small frame.
    FrameHeader fh{static_cast<uint32_t>(FrameType::Dropped), sizeof(DroppedPayload)};
    DroppedPayload dp{};
    dp.ts = m_haveFirstDrop ? m_firstDropTs : m_lastTs;
    dp.tid = m_firstDropTid;
    dp.count = m_stats.recordsDropped;
    const size_t at = m_buffer.size();
    m_buffer.resize(at + sizeof(fh) + sizeof(dp));
    std::memcpy(m_buffer.data() + at, &fh, sizeof(fh));
    std::memcpy(m_buffer.data() + at + sizeof(fh), &dp, sizeof(dp));
    m_stats.bytesWritten += sizeof(fh) + sizeof(dp);
    ++m_stats.framesWritten;
  }
  Result<void> flushed = flushLocked();
  m_file.close();
  return flushed;
}

SpoolStats Spool::stats() const {
  std::lock_guard lock(m_mutex);
  return m_stats;
}

}  // namespace tachyon::inst
