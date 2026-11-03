#include "inst/spool.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

namespace tachyon::inst {

namespace fs = std::filesystem;

namespace {
constexpr size_t kBufferBytes = 256 * 1024;
constexpr uint64_t kMinSegmentBytes = 64 * 1024;
}  // namespace

// Segment files that rotation retired while a snapshot still lists them are deleted when the last snapshot
// is released.
struct Spool::PinState {
  std::mutex mutex;
  int pins = 0;
  std::vector<fs::path> pending;

  void retire(const fs::path& file) {
    std::lock_guard lock(mutex);
    if (pins > 0) {
      pending.push_back(file);
      return;
    }
    std::error_code ec;
    fs::remove(file, ec);
  }
  void pin() {
    std::lock_guard lock(mutex);
    ++pins;
  }
  void unpin() {
    std::vector<fs::path> doomed;
    {
      std::lock_guard lock(mutex);
      if (--pins > 0) return;
      doomed.swap(pending);
    }
    for (const fs::path& file : doomed) {
      std::error_code ec;
      fs::remove(file, ec);
    }
  }
};

Spool::Spool(fs::path path, uint64_t maxBytes, std::shared_ptr<SpoolBudget> budget, SpoolOptions options)
    : m_path(std::move(path)),
      m_maxBytes(options.ringBytes > 0 ? options.ringBytes : maxBytes),
      m_budget(std::move(budget)),
      m_ring(options.ringBytes > 0) {
  m_buffer.reserve(kBufferBytes);
  if (m_ring) {
    m_pins = std::make_shared<PinState>();
    uint64_t seg = options.segmentBytes != 0 ? options.segmentBytes : m_maxBytes / 8;
    seg = std::max(seg, kMinSegmentBytes);
    seg = std::min(seg, std::max<uint64_t>(m_maxBytes / 2, 1));
    m_segmentBytes = seg;
  }
}

Result<std::unique_ptr<Spool>> Spool::create(const fs::path& file, uint64_t maxBytes,
                                             std::shared_ptr<SpoolBudget> budget, SpoolOptions options) {
  std::unique_ptr<Spool> spool(new Spool(file, maxBytes, std::move(budget), options));
  if (spool->m_ring) {
    std::lock_guard lock(spool->m_mutex);
    TACHYON_TRY(spool->openSegmentLocked());
  } else {
    spool->m_file.open(file, std::ios::binary | std::ios::trunc);
    if (!spool->m_file) return makeError(ErrorCode::IoError, "cannot create spool file " + file.string());
  }
  return spool;
}

Spool::~Spool() {
  (void)close();
}

fs::path Spool::segmentPath(uint64_t seq) const {
  char number[32];
  std::snprintf(number, sizeof number, "%06llu", static_cast<unsigned long long>(seq));
  return m_path.parent_path() / (m_path.stem().string() + ".ring-" + number + m_path.extension().string());
}

Result<void> Spool::openSegmentLocked() {
  Segment seg;
  seg.seq = m_nextSeq++;
  seg.file = segmentPath(seg.seq);
  m_file.close();
  m_file.clear();
  m_file.open(seg.file, std::ios::binary | std::ios::trunc);
  if (!m_file) {
    m_ioError = true;
    return makeError(ErrorCode::IoError, "cannot create spool file " + seg.file.string());
  }
  // Header, strings and thread names, so that the segment decodes on its own.
  const size_t before = m_buffer.size();
  m_buffer.insert(m_buffer.end(), m_prefixHeader.begin(), m_prefixHeader.end());
  m_buffer.insert(m_buffer.end(), m_prefixDefs.begin(), m_prefixDefs.end());
  for (const auto& entry : m_prefixNames)
    m_buffer.insert(m_buffer.end(), entry.second.begin(), entry.second.end());
  seg.prefix = seg.bytes = m_buffer.size() - before;
  if (m_budget != nullptr && seg.prefix > 0) m_budget->force(seg.prefix);
  m_segments.push_back(std::move(seg));
  ++m_stats.segmentsStarted;
  return ok();
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

void Spool::recordDropLocked(const FramePeek& peek) {
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

void Spool::dropOldestLocked() {
  Segment oldest = std::move(m_segments.front());
  m_segments.pop_front();
  if (m_budget != nullptr) m_budget->release(oldest.bytes);
  ++m_stats.segmentsDiscarded;
  m_stats.bytesDiscarded += oldest.bytes;
  m_pins->retire(oldest.file);
}

void Spool::rotateLocked() {
  (void)flushLocked();
  uint64_t retained = 0;
  for (const Segment& s : m_segments) retained += s.bytes;
  while (m_segments.size() > 1 && retained + m_segmentBytes > m_maxBytes) {
    retained -= m_segments.front().bytes;
    dropOldestLocked();
  }
  (void)openSegmentLocked();
}

bool Spool::notePrefixLocked(const uint8_t* frame, size_t bytes, const FramePeek& peek) {
  switch (peek.type) {
    case FrameType::StreamHeader:
      // A new stream: its strings and thread names start from scratch.
      m_prefixHeader.assign(frame, frame + bytes);
      m_prefixDefs.clear();
      m_prefixNames.clear();
      m_prefixNamesBytes = 0;
      return true;
    case FrameType::StringDef:
      if (m_prefixDefs.size() + bytes > kMaxRingPrefixBytes) {
        ++m_stats.prefixOverflows;
        return false;
      }
      m_prefixDefs.insert(m_prefixDefs.end(), frame, frame + bytes);
      return true;
    case FrameType::ThreadName: {
      auto it = m_prefixNames.find(peek.tid);
      if (it == m_prefixNames.end()) {
        if (m_prefixNames.size() >= kMaxRingThreadNames || m_prefixNamesBytes + bytes > kMaxRingPrefixBytes) {
          ++m_stats.prefixOverflows;
          return false;
        }
        m_prefixNames.emplace(peek.tid, std::vector<uint8_t>(frame, frame + bytes));
      } else {
        m_prefixNamesBytes -= it->second.size();
        it->second.assign(frame, frame + bytes);
      }
      m_prefixNamesBytes += bytes;
      return true;
    }
    default: return false;
  }
}

void Spool::appendRingLocked(const uint8_t* frame, size_t bytes, const FramePeek& peek) {
  const bool retainedInPrefix = notePrefixLocked(frame, bytes, peek);
  const Segment& cur = m_segments.back();
  if (cur.bytes > cur.prefix && cur.bytes + bytes > m_segmentBytes) {
    rotateLocked();
    if (m_ioError) {
      recordDropLocked(peek);
      return;
    }
    if (retainedInPrefix) {  // the new segment already starts with it
      ++m_stats.framesWritten;
      m_stats.bytesWritten += bytes;
      if (peek.hasTimestamp && peek.ts > m_lastTs) m_lastTs = peek.ts;
      return;
    }
  }
  if (m_budget != nullptr && !m_budget->reserve(bytes)) {
    // Out of the capture-wide budget: make room by discarding our own oldest data.
    bool reserved = false;
    while (!reserved && m_segments.size() > 1) {
      dropOldestLocked();
      reserved = m_budget->reserve(bytes);
    }
    if (!reserved) {
      recordDropLocked(peek);
      return;
    }
  }
  Segment& seg = m_segments.back();
  m_buffer.insert(m_buffer.end(), frame, frame + bytes);
  seg.bytes += bytes;
  m_stats.bytesWritten += bytes;
  ++m_stats.framesWritten;
  if (peek.hasTimestamp && peek.ts > m_lastTs) m_lastTs = peek.ts;
  if (m_buffer.size() >= kBufferBytes) (void)flushLocked();
}

void Spool::appendFrame(const uint8_t* frame, size_t bytes) {
  const FramePeek peek = peekFrame(frame, bytes);
  std::lock_guard lock(m_mutex);
  if (m_closed) return;
  if (m_ring) {
    if (bytes < sizeof(FrameHeader) || m_ioError) {
      recordDropLocked(peek);
      return;
    }
    appendRingLocked(frame, bytes, peek);
    return;
  }
  if (!m_stats.overflowed && !m_ioError && m_stats.bytesWritten + bytes <= m_maxBytes &&
      (m_budget == nullptr || m_budget->reserve(bytes))) {
    m_buffer.insert(m_buffer.end(), frame, frame + bytes);
    m_stats.bytesWritten += bytes;
    ++m_stats.framesWritten;
    if (peek.hasTimestamp && peek.ts > m_lastTs) m_lastTs = peek.ts;
    if (m_buffer.size() >= kBufferBytes) (void)flushLocked();
    return;
  }
  // Past the cap (sticky), or the disk failed: drop and count.
  recordDropLocked(peek);
}

void Spool::noteTruncatedTail(size_t tailBytes) {
  std::lock_guard lock(m_mutex);
  m_stats.truncatedTailBytes += tailBytes;
}

void Spool::noteCorrupt(uint64_t discarded) {
  std::lock_guard lock(m_mutex);
  m_stats.corruptBytes += discarded;
}

void Spool::noteSkipped(uint64_t frames) {
  std::lock_guard lock(m_mutex);
  m_stats.skippedFrames += frames;
}

void Spool::noteConnection() {
  std::lock_guard lock(m_mutex);
  ++m_stats.connections;
}

Result<uint64_t> Spool::flush() {
  std::lock_guard lock(m_mutex);
  if (m_closed) return m_ring ? m_segments.back().bytes : m_stats.bytesWritten;
  TACHYON_TRY(flushLocked());
  return m_ring ? m_segments.back().bytes : m_stats.bytesWritten;
}

Result<SpoolSnapshot> Spool::snapshot() {
  std::lock_guard lock(m_mutex);
  SpoolSnapshot snap;
  if (!m_closed) TACHYON_TRY(flushLocked());
  if (!m_ring) {
    snap.segments.push_back({m_path, m_stats.bytesWritten});
    return snap;
  }
  for (const Segment& s : m_segments) snap.segments.push_back({s.file, s.bytes});
  if (!m_closed) {
    m_pins->pin();
    std::shared_ptr<PinState> pins = m_pins;
    snap.lease = std::shared_ptr<void>(new int(0), [pins](void* p) {
      delete static_cast<int*>(p);
      pins->unpin();
    });
  }
  return snap;
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
    if (m_ring) m_segments.back().bytes += sizeof(fh) + sizeof(dp);
    m_stats.bytesWritten += sizeof(fh) + sizeof(dp);
    ++m_stats.framesWritten;
  }
  Result<void> flushed = flushLocked();
  m_file.close();
  return flushed;
}

SpoolStats Spool::stats() const {
  std::lock_guard lock(m_mutex);
  SpoolStats s = m_stats;
  for (const Segment& seg : m_segments) s.retainedBytes += seg.bytes;
  return s;
}

fs::path Spool::path() const {
  std::lock_guard lock(m_mutex);
  return m_ring ? m_segments.back().file : m_path;
}

std::vector<SpoolSegment> Spool::segments() const {
  std::lock_guard lock(m_mutex);
  std::vector<SpoolSegment> out;
  if (!m_ring) {
    out.push_back({m_path, m_stats.bytesWritten});
    return out;
  }
  for (const Segment& s : m_segments) out.push_back({s.file, s.bytes});
  return out;
}

}  // namespace tachyon::inst
