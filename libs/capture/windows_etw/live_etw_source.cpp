#include "capture/windows_etw/live_etw_source.h"

#include "core/log.h"
#include "core/platform.h"

#include <algorithm>
#include <cstring>
#include <utility>

#if TACHYON_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace tachyon::capture::etw {

namespace {

// One event inside a chunk: this header, then `payloadLen` payload bytes (no padding).
struct RecordHeader {
  uint8_t provider[16];
  uint64_t ts;
  uint32_t tid;
  uint32_t pid;
  uint32_t payloadLen;
  uint16_t cpu;
  uint8_t opcode;
  uint8_t version;
};
static_assert(sizeof(RecordHeader) == 40);

constexpr size_t kStageFlushBytes = 256 * 1024;

enum class Kind : uint8_t { Ignore, Meta, Data };

Kind classify(const etlconvert::EtwEvent& e) {
  namespace g = etlconvert::guids;
  namespace op = etlconvert::opcodes;
  if (e.provider == g::kPerfInfo) return e.opcode == op::kPerfInfoSampledProfile ? Kind::Data : Kind::Ignore;
  if (e.provider == g::kStackWalk) return e.opcode == op::kStackWalkStack ? Kind::Data : Kind::Ignore;
  if (e.provider == g::kThread) {
    if (e.opcode == op::kThreadCSwitch || e.opcode == op::kThreadReadyThread) return Kind::Data;
    return (e.opcode >= op::kThreadStart && e.opcode <= op::kThreadDcEnd) || e.opcode == op::kThreadSetName
               ? Kind::Meta
               : Kind::Ignore;
  }
  if (e.provider == g::kProcess)
    return e.opcode >= op::kProcessStart && e.opcode <= op::kProcessDcEnd ? Kind::Meta : Kind::Ignore;
  if (e.provider == g::kImage) return Kind::Meta;
  return Kind::Ignore;
}

struct Ref {
  uint64_t ts;
  uint32_t chunk;
  uint32_t offset;
  bool data;
};

}  // namespace

double currentThreadCpuSeconds() {
#if TACHYON_PLATFORM_WINDOWS
  FILETIME creation, exitTime, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &creation, &exitTime, &kernel, &user)) return 0.0;
  auto toTicks = [](const FILETIME& f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
  };
  return static_cast<double>(toTicks(kernel) + toTicks(user)) * 1e-7;
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0.0;
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

LiveEtwSource::LiveEtwSource(LiveEtwSourceConfig config) : m_config(std::move(config)) {}

LiveEtwSource::~LiveEtwSource() {
  finish();
}

void LiveEtwSource::start() {
  m_thread = std::thread([this] { run(); });
}

void LiveEtwSource::arm(std::vector<uint32_t> rootPids, std::function<RundownSnapshot()> rundown) {
  LiveEtwDecoderConfig dc = m_config.decoder;
  dc.rootPids = std::move(rootPids);
  auto decoder = std::make_unique<LiveEtwDecoder>(std::move(dc));
  {
    std::lock_guard lock(m_mutex);
    if (m_armed) return;
    m_decoder = std::move(decoder);
    m_rundown = std::move(rundown);
    m_armed = true;
  }
  m_cv.notify_all();
}

void LiveEtwSource::stageEvent(Stage& stage, const etlconvert::EtwEvent& e) {
  RecordHeader h{};
  std::memcpy(h.provider, &e.provider, sizeof(h.provider));
  h.ts = e.timestamp;
  h.tid = e.threadId;
  h.pid = e.processId;
  h.payloadLen = static_cast<uint32_t>(e.payload.size());
  h.cpu = e.cpu;
  h.opcode = e.opcode;
  h.version = e.version;
  const size_t at = stage.bytes.size();
  stage.bytes.resize(at + sizeof(h) + e.payload.size());
  std::memcpy(stage.bytes.data() + at, &h, sizeof(h));
  if (!e.payload.empty())
    std::memcpy(stage.bytes.data() + at + sizeof(h), e.payload.data(), e.payload.size());
  ++stage.events;
}

void LiveEtwSource::submit(const etlconvert::EtwEvent& event) {
  const Kind kind = classify(event);
  if (kind == Kind::Ignore) {
    m_ignored.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Stage& stage = kind == Kind::Data ? m_dataStage : m_metaStage;
  stageEvent(stage, event);
  if (stage.bytes.size() >= kStageFlushBytes) pushStage(stage, kind == Kind::Data);
}

void LiveEtwSource::pushStage(Stage& stage, bool data) {
  if (stage.events == 0) return;
  std::lock_guard lock(m_mutex);
  m_stats.eventsSubmitted += stage.events;
  const uint64_t bytes = stage.bytes.size();
  if (m_stop) {
    // finish() already ran: nobody will decode this.
  } else if (data) {
    if (m_dataBytes + bytes > m_config.queueBytes) {
      m_stats.droppedEvents += stage.events;
      m_droppedPending += stage.events;
    } else {
      m_queue.push_back(Chunk{std::move(stage.bytes), true, stage.events, std::chrono::steady_clock::now()});
      m_dataBytes += bytes;
      ++m_stats.chunks;
    }
  } else {
    if (m_metaBytes + bytes > m_config.metaQueueBytes) {
      m_stats.metaLost += stage.events;
    } else {
      m_queue.push_back(Chunk{std::move(stage.bytes), false, stage.events, std::chrono::steady_clock::now()});
      m_metaBytes += bytes;
      ++m_stats.chunks;
    }
  }
  stage.bytes = {};
  stage.events = 0;
  m_stats.maxQueuedBytes = std::max(m_stats.maxQueuedBytes, m_dataBytes + m_metaBytes);
  m_cv.notify_one();
}

void LiveEtwSource::endBuffer() {
  // Metadata first: the data of the same buffer is explained by it.
  pushStage(m_metaStage, false);
  pushStage(m_dataStage, true);
}

void LiveEtwSource::finish() {
  {
    std::lock_guard lock(m_mutex);
    if (m_stop && !m_thread.joinable()) return;
    m_stop = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable()) m_thread.join();
}

LiveEtwSourceStats LiveEtwSource::stats() const {
  std::lock_guard lock(m_mutex);
  LiveEtwSourceStats s = m_stats;
  s.eventsIgnored = m_ignored.load(std::memory_order_relaxed);
  s.queuedBytes = m_dataBytes + m_metaBytes;
  return s;
}

void LiveEtwSource::process(std::vector<Chunk>& chunks, bool stopping) {
  (void)stopping;
  std::vector<Ref> refs;
  for (size_t c = 0; c < chunks.size(); ++c) {
    const std::vector<uint8_t>& b = chunks[c].bytes;
    size_t off = 0;
    while (off + sizeof(RecordHeader) <= b.size()) {
      RecordHeader h;
      std::memcpy(&h, b.data() + off, sizeof(h));
      if (h.payloadLen > b.size() - off - sizeof(h)) break;
      refs.push_back(Ref{h.ts, static_cast<uint32_t>(c), static_cast<uint32_t>(off), chunks[c].data});
      off += sizeof(h) + h.payloadLen;
    }
  }
  // The buffers of the CPUs arrive in any order; the decoder (and its stack matcher) expect trace order.
  // Metadata sorts before data of the same timestamp.
  std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
    return a.ts != b.ts ? a.ts < b.ts : (!a.data && b.data);
  });
  for (const Ref& r : refs) {
    RecordHeader h;
    const uint8_t* base = chunks[r.chunk].bytes.data() + r.offset;
    std::memcpy(&h, base, sizeof(h));
    etlconvert::EtwEvent e;
    std::memcpy(&e.provider, h.provider, sizeof(h.provider));
    e.opcode = h.opcode;
    e.version = h.version;
    e.timestamp = h.ts;
    e.threadId = h.tid;
    e.processId = h.pid;
    e.cpu = h.cpu;
    e.payload = etlconvert::Bytes(reinterpret_cast<const std::byte*>(base + sizeof(h)), h.payloadLen);
    m_decoder->onEvent(e);
  }
}

void LiveEtwSource::run() {
  ILiveSink& sink = *m_config.decoder.sink;
  {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_armed || m_stop; });
    if (!m_armed) return;  // finish() before arm(): nothing was decoded
  }
  if (m_rundown) {
    RundownSnapshot snap = m_rundown();
    m_decoder->applyRundown(snap);
  }
  using Clock = std::chrono::steady_clock;
  for (;;) {
    std::vector<Chunk> ready;
    uint64_t dropped = 0;
    bool stopping = false;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait_for(lock, m_config.period, [this] { return m_stop; });
      stopping = m_stop;
      dropped = std::exchange(m_droppedPending, 0);
      const auto now = Clock::now();
      std::deque<Chunk> keep;
      for (Chunk& c : m_queue) {
        const auto age = now - c.queuedAt;
        if (!c.data) {
          m_metaBytes -= c.bytes.size();
          ready.push_back(std::move(c));
        } else if (stopping) {
          m_dataBytes -= c.bytes.size();
          ready.push_back(std::move(c));
        } else if (age > m_config.maxLag) {
          // Stale data is dropped, not decoded (see LiveEtwSourceConfig::maxLag).
          m_dataBytes -= c.bytes.size();
          m_stats.droppedEvents += c.events;
          dropped += c.events;
        } else if (age >= m_config.holdBack) {
          m_dataBytes -= c.bytes.size();
          ready.push_back(std::move(c));
        } else {
          keep.push_back(std::move(c));
        }
      }
      m_queue = std::move(keep);
      const auto oldest = std::min_element(
          ready.begin(), ready.end(), [](const Chunk& a, const Chunk& b) { return a.queuedAt < b.queuedAt; });
      if (oldest != ready.end())
        m_stats.maxEventAgeSeconds = std::max(m_stats.maxEventAgeSeconds,
                                              std::chrono::duration<double>(now - oldest->queuedAt).count());
    }
    if (!ready.empty()) {
      const auto began = Clock::now();
      process(ready, stopping);
      const double secs = std::chrono::duration<double>(Clock::now() - began).count();
      std::lock_guard lock(m_mutex);
      m_stats.decodeSeconds += secs;
      ++m_stats.batches;
    }
    if (stopping) m_decoder->flush();
    if (dropped != 0) sink.onDropped(dropped);
    if (m_config.nowTicks) sink.onTimeAdvance(m_config.nowTicks());
    {
      std::lock_guard lock(m_mutex);
      m_stats.decoder = m_decoder->stats();
      m_stats.decodeCpuSeconds = currentThreadCpuSeconds();
      if (stopping && m_queue.empty()) return;
    }
  }
}

}  // namespace tachyon::capture::etw
