// Test-only reader for the instrumentation wire format (sdk/src/inst_wire.h). Independent of the writer in
// sdk/src so tests check the bytes rather than the implementation.
#pragma once

#include "inst_wire.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tachyon::inst::test_support {

// Incremental frame splitter: feed arbitrary chunks, get whole frames in order.
class FrameSplitter {
 public:
  using Callback = std::function<void(FrameType type, const uint8_t* payload, uint32_t len)>;

  explicit FrameSplitter(Callback cb) : m_callback(std::move(cb)) {}

  // Not thread-safe.
  void feed(const uint8_t* data, size_t size) {
    m_pending.insert(m_pending.end(), data, data + size);
    size_t off = 0;
    while (m_pending.size() - off >= sizeof(FrameHeader)) {
      FrameHeader fh;
      std::memcpy(&fh, m_pending.data() + off, sizeof(fh));
      if (fh.byte_len > (1u << 24)) {
        m_error = "implausible frame length";
        m_pending.clear();
        return;
      }
      if (m_pending.size() - off < sizeof(fh) + fh.byte_len) break;
      m_callback(static_cast<FrameType>(fh.type), m_pending.data() + off + sizeof(fh), fh.byte_len);
      ++m_frames;
      off += sizeof(fh) + fh.byte_len;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(off));
  }

  size_t frames() const { return m_frames; }
  size_t leftoverBytes() const { return m_pending.size(); }
  const std::string& error() const { return m_error; }

 private:
  Callback m_callback;
  std::vector<uint8_t> m_pending;
  size_t m_frames = 0;
  std::string m_error;
};

struct Event {
  FrameType type = FrameType::ScopeEnd;
  uint64_t ts = 0;
  uint32_t tid = 0;
  std::string id;    // resolved string (scope id / marker name / frame mark name / plot name)
  std::string data;  // scope/marker data, thread name
  uint32_t color = 0;
  uint64_t caller = 0;
  uint64_t number = 0;  // frame number / dropped count / fiber id
  uint64_t other = 0;   // other fiber id
  uint8_t kind = 0;     // fiber event kind
  double value = 0;     // plot
};

// Parses a complete stream into events. Validates structure: the first frame is a StreamHeader with the right
// magic, every string reference is defined earlier, no string key is defined twice.
struct ParsedStream {
  bool headerSeen = false;
  StreamHeaderPayload header{};
  std::map<uint64_t, std::string> strings;
  std::vector<Event> events;
  std::vector<std::string> errors;
  size_t frameCount = 0;

  static ParsedStream parse(const std::vector<uint8_t>& bytes) {
    ParsedStream ps;
    FrameSplitter splitter([&ps](FrameType t, const uint8_t* p, uint32_t n) { ps.onFrame(t, p, n); });
    splitter.feed(bytes.data(), bytes.size());
    ps.frameCount = splitter.frames();
    if (!splitter.error().empty()) ps.errors.push_back(splitter.error());
    if (splitter.leftoverBytes() != 0) ps.errors.push_back("truncated trailing frame");
    return ps;
  }

  std::string str(uint64_t key) {
    auto it = strings.find(key);
    if (it == strings.end()) {
      errors.push_back("undefined string key");
      return {};
    }
    return it->second;
  }

  template <typename P>
  static bool load(const uint8_t* p, uint32_t n, P& out) {
    if (n < sizeof(P)) return false;
    std::memcpy(&out, p, sizeof(P));
    return true;
  }

  void onFrame(FrameType type, const uint8_t* p, uint32_t n) {
    if (!headerSeen && type != FrameType::StreamHeader) {
      errors.push_back("first frame is not a StreamHeader");
      headerSeen = true;
    }
    switch (type) {
      case FrameType::StreamHeader: {
        if (headerSeen) {
          errors.push_back("second StreamHeader");
          return;
        }
        if (!load(p, n, header) || header.magic != kStreamMagic) errors.push_back("bad StreamHeader");
        headerSeen = true;
        return;
      }
      case FrameType::StringDef: {
        StringDefPayload d;
        if (!load(p, n, d) || n != sizeof(d) + d.len) {
          errors.push_back("bad StringDef");
          return;
        }
        if (!strings
                 .emplace(uint64_t{d.ptr_key},
                          std::string(reinterpret_cast<const char*>(p + sizeof(d)), d.len))
                 .second)
          errors.push_back("duplicate StringDef");
        return;
      }
      case FrameType::ScopeBegin: {
        ScopeBeginPayload b;
        if (!load(p, n, b) || n != sizeof(b) + b.data_len) {
          errors.push_back("bad ScopeBegin");
          return;
        }
        Event e;
        e.type = type;
        e.ts = b.ts;
        e.tid = b.tid;
        e.color = b.color_rgba;
        e.caller = b.caller_addr;
        e.id = str(b.id_key);
        e.data.assign(reinterpret_cast<const char*>(p + sizeof(b)), b.data_len);
        events.push_back(std::move(e));
        return;
      }
      case FrameType::ScopeEnd: {
        ScopeEndPayload x;
        if (!load(p, n, x) || n != sizeof(x)) {
          errors.push_back("bad ScopeEnd");
          return;
        }
        Event e;
        e.type = type;
        e.ts = x.ts;
        e.tid = x.tid;
        events.push_back(std::move(e));
        return;
      }
      case FrameType::FiberEvent: {
        FiberEventPayload f;
        if (!load(p, n, f) || n != sizeof(f)) {
          errors.push_back("bad FiberEvent");
          return;
        }
        Event e;
        e.type = type;
        e.ts = f.ts;
        e.tid = f.tid;
        e.kind = f.kind;
        e.number = f.fiber_id;
        e.other = f.other_fiber_id;
        events.push_back(std::move(e));
        return;
      }
      case FrameType::FrameMark: {
        FrameMarkPayload f;
        if (!load(p, n, f) || n != sizeof(f)) {
          errors.push_back("bad FrameMark");
          return;
        }
        Event e;
        e.type = type;
        e.ts = f.ts;
        e.tid = f.tid;
        e.id = str(f.name_key);
        e.number = f.frame_number;
        events.push_back(std::move(e));
        return;
      }
      case FrameType::Marker: {
        MarkerPayload m;
        if (!load(p, n, m) || n != sizeof(m) + m.data_len) {
          errors.push_back("bad Marker");
          return;
        }
        Event e;
        e.type = type;
        e.ts = m.ts;
        e.tid = m.tid;
        e.color = m.color_rgba;
        e.id = str(m.name_key);
        e.data.assign(reinterpret_cast<const char*>(p + sizeof(m)), m.data_len);
        events.push_back(std::move(e));
        return;
      }
      case FrameType::Plot: {
        PlotPayload pl;
        if (!load(p, n, pl) || n != sizeof(pl)) {
          errors.push_back("bad Plot");
          return;
        }
        Event e;
        e.type = type;
        e.ts = pl.ts;
        e.id = str(pl.name_key);
        e.value = pl.value;
        events.push_back(std::move(e));
        return;
      }
      case FrameType::ThreadName: {
        ThreadNamePayload t;
        if (!load(p, n, t) || n != sizeof(t) + t.name_len) {
          errors.push_back("bad ThreadName");
          return;
        }
        Event e;
        e.type = type;
        e.ts = t.ts;
        e.tid = t.tid;
        e.data.assign(reinterpret_cast<const char*>(p + sizeof(t)), t.name_len);
        events.push_back(std::move(e));
        return;
      }
      case FrameType::Dropped: {
        DroppedPayload d;
        if (!load(p, n, d) || n != sizeof(d)) {
          errors.push_back("bad Dropped");
          return;
        }
        Event e;
        e.type = type;
        e.ts = d.ts;
        e.tid = d.tid;
        e.number = d.count;
        events.push_back(std::move(e));
        return;
      }
    }
  }

  // Events of one type, optionally for one thread.
  std::vector<const Event*> select(FrameType type, uint32_t tid = 0) const {
    std::vector<const Event*> out;
    for (const Event& e : events)
      if (e.type == type && (tid == 0 || e.tid == tid)) out.push_back(&e);
    return out;
  }
};

}  // namespace tachyon::inst::test_support
