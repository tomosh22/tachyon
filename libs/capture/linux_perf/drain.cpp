#include "capture/linux_perf/drain.h"

#include "core/log.h"

#include <poll.h>

#include <algorithm>
#include <system_error>

namespace tachyon::capture::perf {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------------
// DescendantTracker

void DescendantTracker::setRoots(const std::vector<uint32_t>& roots, bool follow) {
  std::lock_guard lock(m_mutex);
  m_pids.clear();
  m_pids.insert(roots.begin(), roots.end());
  m_follow = follow;
}

void DescendantTracker::onFork(uint32_t pid, uint32_t ppid, uint32_t tid) {
  if (pid != tid) return;  // a new thread, not a new process
  std::lock_guard lock(m_mutex);
  if (m_follow && m_pids.count(ppid) != 0) m_pids.insert(pid);
}

void DescendantTracker::onExit(uint32_t pid, uint32_t tid) {
  (void)pid;
  (void)tid;  // pids stay tracked: liveness is read from /proc (a leader may exit before its threads)
}

std::vector<uint32_t> DescendantTracker::pids() const {
  std::lock_guard lock(m_mutex);
  std::vector<uint32_t> out(m_pids.begin(), m_pids.end());
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------------------------------------
// Drain

Drain::Drain(DrainConfig config, std::vector<IRing*> rings, DescendantTracker* tracker)
    : m_config(std::move(config)), m_rings(std::move(rings)), m_tracker(tracker) {}

Drain::~Drain() {
  (void)finish();
}

Result<void> Drain::open() {
  std::lock_guard lock(m_mutex);
  std::error_code ec;
  fs::create_directories(m_config.spoolDir, ec);
  if (ec)
    return makeError(ErrorCode::IoError, "cannot create " + m_config.spoolDir.string() + ": " + ec.message());
  auto meta = SpoolWriter::create(spoolMetaPath(m_config.spoolDir), SpoolKind::Meta, m_config.layout);
  if (!meta.ok()) return meta.error();
  m_meta = std::make_unique<SpoolWriter>(std::move(*meta));
  TACHYON_TRY(openSegmentLocked());
  m_opened = true;
  return ok();
}

Result<void> Drain::openSegmentLocked() {
  const uint32_t index = m_nextSegment++;
  auto w = SpoolWriter::create(spoolDataPath(m_config.spoolDir, index), SpoolKind::Data, m_config.layout);
  if (!w.ok()) return w.error();
  m_data = std::make_unique<SpoolWriter>(std::move(*w));
  Segment seg;
  seg.index = index;
  seg.info.path = spoolDataPath(m_config.spoolDir, index);
  m_segments.push_back(std::move(seg));
  ++m_stats.segmentsWritten;
  return ok();
}

void Drain::sealCurrentLocked() {
  if (!m_data) return;
  (void)m_data->close();
  if (!m_segments.empty()) m_segments.back().info.bytes = m_data->bytesWritten();
  m_data.reset();
}

void Drain::enforceBudgetLocked() {
  if (m_config.circularMb == 0) return;
  const uint64_t budget = static_cast<uint64_t>(m_config.circularMb) << 20;
  for (;;) {
    uint64_t total = 0;
    for (const Segment& s : m_segments) total += s.info.bytes;
    if (m_data && !m_segments.empty()) total += m_data->bytesWritten() - m_segments.back().info.bytes;
    if (total <= budget || m_segments.size() <= 1) return;
    std::error_code ec;
    fs::remove(m_segments.front().info.path, ec);
    m_segments.erase(m_segments.begin());
    ++m_stats.segmentsDropped;
  }
}

void Drain::fail(const Error& e) {
  if (!m_stats.ioFailed) {
    m_stats.ioFailed = true;
    m_stats.ioError = e.message;
    TACHYON_LOG_ERROR("perf: spool write failed, the capture is truncated: {}", e.message);
  }
}

Result<void> Drain::appendMeta(uint32_t ringId, std::span<const uint8_t> records) {
  std::lock_guard lock(m_mutex);
  if (!m_meta) return makeError(ErrorCode::Internal, "the spool is not open");
  return m_meta->append(ringId, records);
}

void Drain::start() {
  m_thread = std::thread([this] { run(); });
}

Result<void> Drain::finish() {
  if (m_finished) return ok();
  m_finished = true;
  m_stop.store(true);
  m_wake.notify_all();
  if (m_thread.joinable()) m_thread.join();
  std::lock_guard lock(m_mutex);
  Result<void> result = ok();
  if (m_meta) {
    auto r = m_meta->close();
    if (!r.ok()) result = r;
  }
  sealCurrentLocked();
  return result;
}

DrainStats Drain::stats() const {
  std::lock_guard lock(m_mutex);
  DrainStats s = m_stats;
  for (IRing* r : m_rings) s.ringOverruns += r->overruns();
  return s;
}

std::vector<SealedSegment> Drain::segments() const {
  std::lock_guard lock(m_mutex);
  std::vector<SealedSegment> out;
  for (const Segment& s : m_segments) out.push_back(s.info);
  return out;
}

Result<std::vector<SealedSegment>> Drain::sealForFlush() {
  std::lock_guard lock(m_mutex);
  if (!m_opened) return makeError(ErrorCode::Internal, "the spool is not open");
  if (m_meta) (void)m_meta->flush();  // the converter reads meta.bin while the capture keeps appending to it
  if (m_data && m_data->bytesWritten() > sizeof(SpoolFileHeader)) {
    (void)m_data->flush();
    sealCurrentLocked();
    TACHYON_TRY(openSegmentLocked());
  } else if (m_data) {
    (void)m_data->flush();  // empty current segment: nothing to seal
  }
  std::vector<SealedSegment> out;
  for (size_t i = 0; i + 1 < m_segments.size(); ++i) out.push_back(m_segments[i].info);
  return out;
}

void Drain::release(const std::vector<SealedSegment>& segments) {
  std::lock_guard lock(m_mutex);
  for (const SealedSegment& gone : segments) {
    std::error_code ec;
    fs::remove(gone.path, ec);
    m_segments.erase(std::remove_if(m_segments.begin(), m_segments.end(),
                                    [&](const Segment& s) { return s.info.path == gone.path; }),
                     m_segments.end());
  }
}

void Drain::run() {
  std::vector<pollfd> fds;
  for (IRing* r : m_rings) {
    if (r->pollFd() >= 0) fds.push_back({r->pollFd(), POLLIN, 0});
  }
  const int timeoutMs = static_cast<int>(m_config.pollInterval.count());
  while (!m_stop.load()) {
    if (!fds.empty()) {
      ::poll(fds.data(), fds.size(), timeoutMs);
    } else {
      std::unique_lock lock(m_wakeMutex);
      m_wake.wait_for(lock, m_config.pollInterval, [this] { return m_stop.load(); });
    }
    drainAll();
  }
  drainAll();  // the events are disabled: everything the kernel wrote is in the rings now
  drainAll();
}

void Drain::drainAll() {
  for (size_t i = 0; i < m_rings.size(); ++i) {
    for (int round = 0; round < 64; ++round) {  // bounded so one busy ring cannot starve the others
      m_scratch.clear();
      const RingStatus st = m_rings[i]->drain(m_scratch);
      if (st != RingStatus::Data) break;
      process(static_cast<uint32_t>(i), m_scratch);
    }
  }
}

void Drain::process(uint32_t ringId, const std::vector<uint8_t>& bytes) {
  m_metaBuf.clear();
  m_dataBuf.clear();
  uint64_t samples = 0, metaRecords = 0, lostRecords = 0, lostEvents = 0, throttles = 0;
  uint64_t firstTs = 0, lastTs = 0;
  bool malformed = false;

  size_t consumed = 0;
  const ParseStatus walk =
      forEachRecord(std::span<const uint8_t>(bytes), consumed, [&](std::span<const uint8_t> rec) {
        RecordHeader h;
        (void)readRecordHeader(rec, h);
        // Context-switch side-band records are as frequent as samples and as time-ordered: they rotate with
        // the sample segments in circular mode instead of growing meta.bin without bound.
        if (h.type == kRecordSample || h.type == kRecordSwitch || h.type == kRecordSwitchCpuWide) {
          ++samples;
          m_dataBuf.insert(m_dataBuf.end(), rec.begin(), rec.end());
          ParsedRecord pr;
          if (parseRecord(rec, m_config.layout, pr) == ParseStatus::Ok && pr.hasTime) {
            if (firstTs == 0 || pr.time < firstTs) firstTs = pr.time;
            lastTs = std::max(lastTs, pr.time);
          }
          return;
        }
        ++metaRecords;
        m_metaBuf.insert(m_metaBuf.end(), rec.begin(), rec.end());
        if (h.type == kRecordFork || h.type == kRecordExit || h.type == kRecordLost ||
            h.type == kRecordLostSamples || h.type == kRecordThrottle) {
          ParsedRecord pr;
          if (parseRecord(rec, m_config.layout, pr) != ParseStatus::Ok) return;
          if (h.type == kRecordFork && m_tracker != nullptr) m_tracker->onFork(pr.pid, pr.ppid, pr.tid);
          if (h.type == kRecordExit && m_tracker != nullptr) m_tracker->onExit(pr.pid, pr.tid);
          if (h.type == kRecordLost || h.type == kRecordLostSamples) {
            ++lostRecords;
            lostEvents += pr.lost;
          }
          if (h.type == kRecordThrottle) ++throttles;
        }
      });
  if (walk != ParseStatus::Ok) malformed = true;  // the rest of this read is not a record sequence
  if (m_tap && (!m_metaBuf.empty() || !m_dataBuf.empty())) m_tap(m_metaBuf, m_dataBuf);

  std::lock_guard lock(m_mutex);
  m_stats.bytesDrained += consumed;
  m_stats.samples += samples;
  m_stats.metaRecords += metaRecords;
  m_stats.lostRecords += lostRecords;
  m_stats.lostEvents += lostEvents;
  m_stats.throttleRecords += throttles;
  if (malformed) ++m_stats.malformedChunks;
  if (m_stats.ioFailed || !m_meta) return;
  if (!m_metaBuf.empty()) {
    auto r = m_meta->append(ringId, m_metaBuf);
    if (!r.ok()) fail(r.error());
  }
  if (!m_dataBuf.empty() && m_data) {
    auto r = m_data->append(ringId, m_dataBuf);
    if (!r.ok()) {
      fail(r.error());
      return;
    }
    if (!m_segments.empty()) {
      SealedSegment& cur = m_segments.back().info;
      if (cur.firstTs == 0 || (firstTs != 0 && firstTs < cur.firstTs)) cur.firstTs = firstTs;
      cur.lastTs = std::max(cur.lastTs, lastTs);
    }
    if (m_config.circularMb != 0) {
      const uint64_t segmentBytes =
          std::max<uint64_t>((static_cast<uint64_t>(m_config.circularMb) << 20) / 8, 1u << 20);
      if (m_data->bytesWritten() >= segmentBytes) {
        sealCurrentLocked();
        auto open = openSegmentLocked();
        if (!open.ok()) fail(open.error());
      }
      enforceBudgetLocked();
    }
  }
}

}  // namespace tachyon::capture::perf
