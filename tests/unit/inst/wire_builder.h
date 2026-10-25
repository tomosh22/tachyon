// Test-only writer for instrumentation wire streams (sdk/src/inst_wire.h), independent of the SDK runtime.
#pragma once

#include "inst_wire.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tachyon::inst::testing_util {

class StreamBuilder {
 public:
  // Starts a stream: the StreamHeader frame.
  StreamBuilder& header(uint32_t pid = 1234, uint64_t clockHz = 1'000'000'000, uint64_t processStartTs = 1,
                        uint32_t wireVersion = kWireVersion, uint32_t platform = 2) {
    StreamHeaderPayload sh{};
    sh.magic = kStreamMagic;
    sh.wire_version = wireVersion;
    sh.pid = pid;
    sh.platform = platform;
    sh.clock_frequency_hz = clockHz;
    sh.process_start_ts = processStartTs;
    sh.api_version = 0x00010000;
    return frame(FrameType::StreamHeader, &sh, sizeof(sh));
  }

  // Defines a static string under a made-up pointer key and returns the key.
  uint64_t string(const std::string& text, uint32_t tag = 0) {
    const uint64_t key = makeStringKey(0x1000 + 0x40 * (++m_strings), tag);
    stringAs(key, text);
    return key;
  }
  StreamBuilder& stringAs(uint64_t key, const std::string& text) {
    std::vector<uint8_t> payload(sizeof(StringDefPayload) + text.size());
    StringDefPayload d{key, static_cast<uint32_t>(text.size())};
    std::memcpy(payload.data(), &d, sizeof(d));
    if (!text.empty()) std::memcpy(payload.data() + sizeof(d), text.data(), text.size());
    return frame(FrameType::StringDef, payload.data(), payload.size());
  }

  StreamBuilder& scopeBegin(uint64_t ts, uint32_t tid, uint64_t idKey, const std::string& data = {},
                            uint32_t color = 0xFFFFFFFFu, uint64_t caller = 0xCAFE) {
    std::vector<uint8_t> payload(sizeof(ScopeBeginPayload) + data.size());
    ScopeBeginPayload p{};
    p.ts = ts;
    p.tid = tid;
    p.color_rgba = color;
    p.id_key = idKey;
    p.caller_addr = caller;
    p.data_len = static_cast<uint16_t>(data.size());
    std::memcpy(payload.data(), &p, sizeof(p));
    if (!data.empty()) std::memcpy(payload.data() + sizeof(p), data.data(), data.size());
    return frame(FrameType::ScopeBegin, payload.data(), payload.size());
  }
  StreamBuilder& scopeEnd(uint64_t ts, uint32_t tid) {
    ScopeEndPayload p{ts, tid};
    return frame(FrameType::ScopeEnd, &p, sizeof(p));
  }
  StreamBuilder& fiber(uint64_t ts, uint32_t tid, FiberEventKind kind, uint64_t fiberId, uint64_t other = 0) {
    FiberEventPayload p{};
    p.ts = ts;
    p.tid = tid;
    p.kind = static_cast<uint8_t>(kind);
    p.fiber_id = fiberId;
    p.other_fiber_id = other;
    return frame(FrameType::FiberEvent, &p, sizeof(p));
  }
  StreamBuilder& frameMark(uint64_t ts, uint32_t tid, uint64_t nameKey, uint64_t number) {
    FrameMarkPayload p{ts, tid, nameKey, number};
    return frame(FrameType::FrameMark, &p, sizeof(p));
  }
  StreamBuilder& marker(uint64_t ts, uint32_t tid, uint64_t nameKey, const std::string& data = {},
                        uint32_t color = 0x11223344u) {
    std::vector<uint8_t> payload(sizeof(MarkerPayload) + data.size());
    MarkerPayload p{};
    p.ts = ts;
    p.tid = tid;
    p.name_key = nameKey;
    p.color_rgba = color;
    p.data_len = static_cast<uint16_t>(data.size());
    std::memcpy(payload.data(), &p, sizeof(p));
    if (!data.empty()) std::memcpy(payload.data() + sizeof(p), data.data(), data.size());
    return frame(FrameType::Marker, payload.data(), payload.size());
  }
  StreamBuilder& plot(uint64_t ts, uint64_t nameKey, double value) {
    PlotPayload p{ts, nameKey, value};
    return frame(FrameType::Plot, &p, sizeof(p));
  }
  StreamBuilder& threadName(uint64_t ts, uint32_t tid, const std::string& name) {
    std::vector<uint8_t> payload(sizeof(ThreadNamePayload) + name.size());
    ThreadNamePayload p{ts, tid, static_cast<uint16_t>(name.size())};
    std::memcpy(payload.data(), &p, sizeof(p));
    if (!name.empty()) std::memcpy(payload.data() + sizeof(p), name.data(), name.size());
    return frame(FrameType::ThreadName, payload.data(), payload.size());
  }
  StreamBuilder& dropped(uint64_t ts, uint32_t tid, uint64_t count) {
    DroppedPayload p{ts, tid, count};
    return frame(FrameType::Dropped, &p, sizeof(p));
  }

  // Any frame, including types the decoder does not know.
  StreamBuilder& frame(FrameType type, const void* payload, size_t size) {
    return frameRaw(static_cast<uint32_t>(type), payload, size);
  }
  StreamBuilder& frameRaw(uint32_t type, const void* payload, size_t size) {
    FrameHeader fh{type, static_cast<uint32_t>(size)};
    const size_t at = m_bytes.size();
    m_bytes.resize(at + sizeof(fh) + size);
    std::memcpy(m_bytes.data() + at, &fh, sizeof(fh));
    if (size > 0) std::memcpy(m_bytes.data() + at + sizeof(fh), payload, size);
    ++m_frames;
    return *this;
  }

  const std::vector<uint8_t>& bytes() const { return m_bytes; }
  size_t frames() const { return m_frames; }
  // Writes the stream to a file; returns the path.
  std::filesystem::path write(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(m_bytes.data()), static_cast<std::streamsize>(m_bytes.size()));
    return path;
  }

 private:
  std::vector<uint8_t> m_bytes;
  uint64_t m_strings = 0;
  size_t m_frames = 0;
};

}  // namespace tachyon::inst::testing_util
