#include "inst/live_forward.h"

#include "inst/frame_stream.h"
#include "trace/trace_format.h"

#include <cstring>

namespace tachyon::inst {

namespace {
constexpr size_t kMaxStringBytes = 8u << 20;  // per process
constexpr size_t kMaxStrings = 1u << 18;

template <typename T>
bool read(const uint8_t* payload, size_t len, T& out) {
  if (len < sizeof(T)) return false;
  std::memcpy(&out, payload, sizeof(T));
  return true;
}
}  // namespace

LiveForwarder::LiveForwarder(uint32_t pid, std::shared_ptr<capture::ILiveSink> sink)
    : m_pid(pid), m_sink(std::move(sink)) {}

uint64_t LiveForwarder::malformedFrames() const {
  std::lock_guard lock(m_mutex);
  return m_malformed;
}

void LiveForwarder::onFrame(const uint8_t* frame, size_t bytes) {
  if (!m_sink || bytes < sizeof(FrameHeader)) return;
  FrameHeader fh;
  std::memcpy(&fh, frame, sizeof(fh));
  const uint8_t* payload = frame + sizeof(fh);
  const size_t len = bytes - sizeof(fh);

  std::lock_guard lock(m_mutex);
  auto name = [&](uint64_t key) -> std::string_view {
    auto it = m_strings.find(key);
    if (it == m_strings.end()) {
      ++m_malformed;
      return {};
    }
    return it->second;
  };
  switch (static_cast<FrameType>(fh.type)) {
    case FrameType::StringDef: {
      StringDefPayload p;
      if (!read(payload, len, p) || len < sizeof(p) + p.len) {
        ++m_malformed;
        return;
      }
      if (m_strings.size() >= kMaxStrings || m_stringBytes + p.len > kMaxStringBytes) return;
      auto [it, inserted] = m_strings.try_emplace(
          p.ptr_key, std::string(reinterpret_cast<const char*>(payload + sizeof(p)), p.len));
      if (inserted) m_stringBytes += p.len;
      return;
    }
    case FrameType::ScopeBegin: {
      ScopeBeginPayload p;
      if (!read(payload, len, p) || len < sizeof(p) + p.data_len) {
        ++m_malformed;
        return;
      }
      capture::LiveScopeEvent e;
      e.ts = p.ts;
      e.pid = m_pid;
      e.tid = p.tid;
      e.kind = trace::kScopeBegin;
      e.id = name(p.id_key);
      e.data = std::string_view(reinterpret_cast<const char*>(payload + sizeof(p)), p.data_len);
      e.colorRgba = p.color_rgba;
      e.callerAddr = p.caller_addr;
      m_sink->onScopeEvent(e);
      return;
    }
    case FrameType::ScopeEnd: {
      ScopeEndPayload p;
      if (!read(payload, len, p)) {
        ++m_malformed;
        return;
      }
      capture::LiveScopeEvent e;
      e.ts = p.ts;
      e.pid = m_pid;
      e.tid = p.tid;
      e.kind = trace::kScopeEnd;
      m_sink->onScopeEvent(e);
      return;
    }
    case FrameType::FrameMark: {
      FrameMarkPayload p;
      if (!read(payload, len, p)) {
        ++m_malformed;
        return;
      }
      m_sink->onFrameMark({p.ts, m_pid, p.tid, name(p.name_key), p.frame_number});
      return;
    }
    case FrameType::Marker: {
      MarkerPayload p;
      if (!read(payload, len, p) || len < sizeof(p) + p.data_len) {
        ++m_malformed;
        return;
      }
      capture::LiveMarker m;
      m.ts = p.ts;
      m.pid = m_pid;
      m.tid = p.tid;
      m.name = name(p.name_key);
      m.data = std::string_view(reinterpret_cast<const char*>(payload + sizeof(p)), p.data_len);
      m.colorRgba = p.color_rgba;
      m_sink->onMarker(m);
      return;
    }
    case FrameType::Plot: {
      PlotPayload p;
      if (!read(payload, len, p)) {
        ++m_malformed;
        return;
      }
      m_sink->onPlot({p.ts, name(p.name_key), p.value});
      return;
    }
    case FrameType::ThreadName: {
      ThreadNamePayload p;
      if (!read(payload, len, p) || len < sizeof(p) + p.name_len) {
        ++m_malformed;
        return;
      }
      capture::LiveThread t;
      t.tid = p.tid;
      t.pid = m_pid;
      t.name.assign(reinterpret_cast<const char*>(payload + sizeof(p)), p.name_len);
      m_sink->onThread(t);
      return;
    }
    case FrameType::Dropped: {
      DroppedPayload p;
      if (read(payload, len, p)) m_sink->onDropped(p.count);
      return;
    }
    default: return;  // StreamHeader, FiberEvent, unknown
  }
}

}  // namespace tachyon::inst
