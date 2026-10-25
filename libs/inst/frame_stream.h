// Splitting a byte stream of instrumentation wire frames (sdk/src/inst_wire.h) into whole frames, plus small
// helpers to peek into a frame without decoding it.
#pragma once

#include "inst_wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tachyon::inst {

// Largest frame payload the agent keeps. Real frames are at most kMaxFrameBytes. A frame announcing a larger
// payload that is still below kMaxSkippablePayloadBytes is skipped by its length (it is probably a frame of a
// future version); a larger length cannot be resynchronised and makes the stream corrupt.
constexpr uint32_t kMaxAcceptedPayloadBytes = 1u << 20;
constexpr uint32_t kMaxSkippablePayloadBytes = 64u << 20;

// Not thread-safe. Feed arbitrary chunks; the callback receives each complete frame (header followed by its
// payload, contiguous) in order. Frames that lie entirely inside the fed chunk are passed without copying.
class FrameAssembler {
 public:
  // Not thread-safe. fn(const uint8_t* frame, size_t frameBytes). Returns false once the stream is corrupt
  // (implausible length); further input is then discarded and counted in discardedBytes().
  template <typename Fn>
  bool feed(const uint8_t* data, size_t size, Fn&& fn) {
    if (m_corrupt) {
      m_discarded += size;
      return false;
    }
    while (size > 0) {
      if (m_skip > 0) {  // inside an oversized frame
        const size_t n = static_cast<size_t>(std::min<uint64_t>(m_skip, size));
        m_skip -= n;
        data += n;
        size -= n;
        continue;
      }
      if (!m_pending.empty()) {
        if (m_pending.size() < sizeof(FrameHeader)) {
          const size_t take = std::min(sizeof(FrameHeader) - m_pending.size(), size);
          m_pending.insert(m_pending.end(), data, data + take);
          data += take;
          size -= take;
          if (m_pending.size() < sizeof(FrameHeader)) continue;
        }
        FrameHeader fh;
        std::memcpy(&fh, m_pending.data(), sizeof(fh));
        if (fh.byte_len > kMaxAcceptedPayloadBytes) {
          if (!startSkip(fh.byte_len)) return fail(size);
          m_pending.clear();
          continue;
        }
        const size_t total = sizeof(FrameHeader) + fh.byte_len;
        const size_t take = std::min(total - m_pending.size(), size);
        m_pending.insert(m_pending.end(), data, data + take);
        data += take;
        size -= take;
        if (m_pending.size() == total) {
          fn(m_pending.data(), m_pending.size());
          m_pending.clear();
        }
        continue;
      }
      if (size < sizeof(FrameHeader)) {
        m_pending.assign(data, data + size);
        return true;
      }
      FrameHeader fh;
      std::memcpy(&fh, data, sizeof(fh));
      if (fh.byte_len > kMaxAcceptedPayloadBytes) {
        if (!startSkip(fh.byte_len)) return fail(size);
        data += sizeof(FrameHeader);
        size -= sizeof(FrameHeader);
        continue;
      }
      const size_t total = sizeof(FrameHeader) + fh.byte_len;
      if (size < total) {
        m_pending.assign(data, data + size);
        return true;
      }
      fn(data, total);
      data += total;
      size -= total;
    }
    return true;
  }

  // Thread-safe for const access from the owning thread only. Bytes of an incomplete trailing frame.
  size_t pendingBytes() const { return m_pending.size(); }
  bool corrupt() const { return m_corrupt; }
  // Bytes dropped because the stream was corrupt.
  uint64_t discardedBytes() const { return m_discarded; }
  // Frames larger than kMaxAcceptedPayloadBytes that were skipped by their length.
  uint64_t skippedFrames() const { return m_skippedFrames; }
  // Bytes of a skipped frame still to come (non-zero when the stream ended inside one).
  uint64_t skipRemaining() const { return m_skip; }

 private:
  // Starts skipping the payload of an oversized frame; false when the length is not plausible.
  bool startSkip(uint32_t byteLen) {
    if (byteLen > kMaxSkippablePayloadBytes) return false;
    m_skip = byteLen;
    ++m_skippedFrames;
    return true;
  }
  bool fail(size_t remaining) {
    m_corrupt = true;
    m_discarded += m_pending.size() + remaining;
    m_pending.clear();
    return false;
  }

  std::vector<uint8_t> m_pending;
  bool m_corrupt = false;
  uint64_t m_discarded = 0;
  uint64_t m_skip = 0;
  uint64_t m_skippedFrames = 0;
};

// What a record frame says about when and on which thread it happened (without decoding it).
struct FramePeek {
  FrameType type = FrameType::StreamHeader;
  bool hasTimestamp = false;
  uint64_t ts = 0;
  uint32_t tid = 0;       // 0 for frames without a thread (Plot)
  uint64_t dropped = 0;   // Dropped frames: the number of records they report lost
  bool isRecord = false;  // ScopeBegin .. Dropped: counts as a record when it cannot be stored
};

// Thread-safe: pure function. `frame` is a complete frame (header + payload) of `bytes` bytes.
inline FramePeek peekFrame(const uint8_t* frame, size_t bytes) {
  FramePeek peek;
  FrameHeader fh{};
  if (bytes < sizeof(fh)) return peek;
  std::memcpy(&fh, frame, sizeof(fh));
  peek.type = static_cast<FrameType>(fh.type);
  const uint8_t* payload = frame + sizeof(fh);
  const size_t len = bytes - sizeof(fh);
  switch (peek.type) {
    case FrameType::ScopeBegin:
    case FrameType::ScopeEnd:
    case FrameType::FiberEvent:
    case FrameType::FrameMark:
    case FrameType::Marker:
    case FrameType::ThreadName:
    case FrameType::Dropped:
      if (len >= 12) {
        std::memcpy(&peek.ts, payload, 8);
        std::memcpy(&peek.tid, payload + 8, 4);
        peek.hasTimestamp = true;
      }
      peek.isRecord = true;
      if (peek.type == FrameType::Dropped && len >= sizeof(DroppedPayload))
        std::memcpy(&peek.dropped, payload + 12, 8);
      break;
    case FrameType::Plot:
      if (len >= 8) {
        std::memcpy(&peek.ts, payload, 8);
        peek.hasTimestamp = true;
      }
      peek.isRecord = true;
      break;
    default: break;
  }
  return peek;
}

}  // namespace tachyon::inst
