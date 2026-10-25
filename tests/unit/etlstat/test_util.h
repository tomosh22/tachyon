// Hand-built byte payloads and event helpers for the etlstat unit tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/span_util.h"
#include "etw_stats.h"
#include "etw_types.h"

namespace tachyon::etlstat::testing {

using ByteVec = std::vector<std::byte>;

template <typename T>
void put(ByteVec& out, T value) {
  core::appendPod(out, value);
}

inline void putWide(ByteVec& out, const std::u16string& text) {
  for (char16_t c : text) put<uint16_t>(out, static_cast<uint16_t>(c));
  put<uint16_t>(out, 0);
}

inline ByteVec sampledProfilePayload(uint64_t ip, uint32_t tid) {
  ByteVec b;
  put<uint64_t>(b, ip);
  put<uint32_t>(b, tid);
  put<uint16_t>(b, 1);
  put<uint16_t>(b, 0);
  return b;
}

inline ByteVec cswitchPayload(uint32_t newTid, uint32_t oldTid, int8_t waitReason = 0) {
  ByteVec b;
  put<uint32_t>(b, newTid);
  put<uint32_t>(b, oldTid);
  put<int8_t>(b, 8);
  put<int8_t>(b, 9);
  put<uint8_t>(b, 1);
  put<int8_t>(b, 0);
  put<int8_t>(b, waitReason);
  put<int8_t>(b, 1);
  put<int8_t>(b, 5);
  put<int8_t>(b, 3);
  put<uint32_t>(b, 77);
  put<uint32_t>(b, 0);
  return b;
}

inline ByteVec readyThreadPayload(uint32_t tid) {
  ByteVec b;
  put<uint32_t>(b, tid);
  put<int8_t>(b, 1);
  put<int8_t>(b, 2);
  put<int8_t>(b, 3);
  put<int8_t>(b, 0);
  return b;
}

inline ByteVec stackWalkPayload(uint64_t eventTs, uint32_t pid, uint32_t tid,
                                const std::vector<uint64_t>& frames) {
  ByteVec b;
  put<uint64_t>(b, eventTs);
  put<uint32_t>(b, pid);
  put<uint32_t>(b, tid);
  for (uint64_t f : frames) put<uint64_t>(b, f);
  return b;
}

inline EtwEvent makeEvent(const Guid& provider, uint8_t opcode, uint8_t version, uint64_t ts, uint32_t tid,
                          uint16_t cpu, const ByteVec& payload) {
  EtwEvent e;
  e.provider = provider;
  e.opcode = opcode;
  e.version = version;
  e.timestamp = ts;
  e.threadId = tid;
  e.processId = 100;
  e.cpu = cpu;
  e.payload = std::span<const std::byte>(payload.data(), payload.size());
  return e;
}

}  // namespace tachyon::etlstat::testing
