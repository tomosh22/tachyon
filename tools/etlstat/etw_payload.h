// Decoders for the x64 kernel event payload layouts in Appendix B. Every layout here is "documented, to be
// confirmed by observation" until docs/etw-notes.md says otherwise. All decoders are bounds-checked and
// return nullopt on short payloads; trailing bytes beyond the documented layout are ignored (newer versions
// append fields).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace tachyon::etlstat {

using Bytes = std::span<const std::byte>;

struct SampledProfilePayload {
  uint64_t instructionPointer = 0;
  uint32_t threadId = 0;
  uint16_t count = 0;
  uint16_t reserved = 0;
};
inline constexpr size_t kSampledProfileSize = 16;

struct CSwitchPayload {
  uint32_t newThreadId = 0;
  uint32_t oldThreadId = 0;
  int8_t newThreadPriority = 0;
  int8_t oldThreadPriority = 0;
  uint8_t previousCState = 0;
  int8_t spareByte = 0;
  int8_t oldThreadWaitReason = 0;
  int8_t oldThreadWaitMode = 0;
  int8_t oldThreadState = 0;
  int8_t oldThreadWaitIdealProcessor = 0;
  uint32_t newThreadWaitTime = 0;
  uint32_t reserved = 0;
};
inline constexpr size_t kCSwitchSize = 24;

struct ReadyThreadPayload {
  uint32_t threadId = 0;
  int8_t adjustReason = 0;
  int8_t adjustIncrement = 0;
  int8_t flag = 0;
  int8_t reserved = 0;
};
inline constexpr size_t kReadyThreadSize = 8;

struct StackWalkPayload {
  uint64_t eventTimeStamp = 0;  // timestamp of the parent event this stack belongs to
  uint32_t stackProcess = 0;
  uint32_t stackThread = 0;
  uint32_t depth = 0;  // number of 64-bit frames
  Bytes frames;        // depth * 8 bytes, little-endian addresses
};
inline constexpr size_t kStackWalkHeaderSize = 16;

struct ImagePayload {
  uint64_t imageBase = 0;
  uint64_t imageSize = 0;
  uint32_t processId = 0;
  uint32_t imageChecksum = 0;
  uint32_t timeDateStamp = 0;
  uint32_t reserved0 = 0;
  uint64_t defaultBase = 0;
  uint32_t reserved[4] = {};
  std::string fileName;  // UTF-8
};
inline constexpr size_t kImageFixedSize = 56;

// Thread Start/End/DCStart/DCEnd (v3): only the leading ids are decoded.
struct ThreadLifetimePayload {
  uint32_t processId = 0;
  uint32_t threadId = 0;
};

// Thread SetName (opcode 72): {u32 ProcessId; u32 ThreadId; wstr ThreadName}.
struct ThreadSetNamePayload {
  uint32_t processId = 0;
  uint32_t threadId = 0;
  std::string name;  // UTF-8
};

// Process Start/End/DCStart/DCEnd (v3/v4), best effort; see docs/etw-notes.md.
struct ProcessPayload {
  uint32_t processId = 0;
  uint32_t parentId = 0;
  std::string imageFileName;  // ANSI image name following the variable-length SID, may be empty
};

// All functions below: thread-safe, pure.
std::optional<SampledProfilePayload> decodeSampledProfile(Bytes payload);
std::optional<CSwitchPayload> decodeCSwitch(Bytes payload);
std::optional<ReadyThreadPayload> decodeReadyThread(Bytes payload);
std::optional<StackWalkPayload> decodeStackWalk(Bytes payload);
std::optional<ImagePayload> decodeImage(Bytes payload);
std::optional<ThreadLifetimePayload> decodeThreadLifetime(Bytes payload);
std::optional<ThreadSetNamePayload> decodeThreadSetName(Bytes payload);
std::optional<ProcessPayload> decodeProcess(Bytes payload, uint8_t version);

// Thread-safe: pure function. Reads the i-th frame address of a decoded stack (0 if out of range).
uint64_t stackFrame(const StackWalkPayload& stack, uint32_t index);

// Thread-safe: pure function. Converts UTF-16LE bytes (optionally NUL terminated) to UTF-8.
std::string utf16LeToUtf8(Bytes bytes);

}  // namespace tachyon::etlstat
