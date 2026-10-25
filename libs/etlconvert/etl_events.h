// Header-only, platform-independent ETW vocabulary and kernel event payload decoders (Appendix B, x64).
// Re-implemented here (a library must not depend on tools/etlstat); layouts follow docs/etw-notes.md and
// stay "documented, to be confirmed by observation" until the [VERIFY] items there are closed.
// All decoders are bounds-checked and return nullopt on short payloads; trailing bytes beyond the documented
// layout are ignored (newer event versions append fields).
#pragma once

#include "core/span_util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tachyon::etlconvert {

using Bytes = std::span<const std::byte>;

// Binary layout identical to the Windows GUID struct.
struct Guid {
  uint32_t data1 = 0;
  uint16_t data2 = 0;
  uint16_t data3 = 0;
  std::array<uint8_t, 8> data4{};

  friend bool operator==(const Guid& a, const Guid& b) = default;
};
static_assert(sizeof(Guid) == 16);

namespace guids {
inline constexpr Guid kProcess{0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
inline constexpr Guid kThread{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
inline constexpr Guid kImage{0x2cb15d1d, 0x5fc1, 0x11d2, {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};
inline constexpr Guid kPerfInfo{0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
inline constexpr Guid kStackWalk{
    0xdef2fe46, 0x7bd6, 0x4b80, {0xbd, 0x94, 0xf5, 0x7f, 0xe2, 0x0d, 0x0c, 0xe3}};
}  // namespace guids

namespace opcodes {
inline constexpr uint8_t kProcessStart = 1;
inline constexpr uint8_t kProcessEnd = 2;
inline constexpr uint8_t kProcessDcStart = 3;
inline constexpr uint8_t kProcessDcEnd = 4;
inline constexpr uint8_t kThreadStart = 1;
inline constexpr uint8_t kThreadEnd = 2;
inline constexpr uint8_t kThreadDcStart = 3;
inline constexpr uint8_t kThreadDcEnd = 4;
inline constexpr uint8_t kThreadCSwitch = 36;
inline constexpr uint8_t kThreadReadyThread = 50;
inline constexpr uint8_t kThreadSetName = 72;
inline constexpr uint8_t kImageUnload = 2;
inline constexpr uint8_t kImageDcStart = 3;
inline constexpr uint8_t kImageDcEnd = 4;
inline constexpr uint8_t kImageLoad = 10;
inline constexpr uint8_t kPerfInfoSampledProfile = 46;
inline constexpr uint8_t kStackWalkStack = 32;
inline constexpr uint8_t kStackWalkKeyKernel = 34;
inline constexpr uint8_t kStackWalkKeyUser = 35;
inline constexpr uint8_t kStackWalkKeyRundown = 36;
inline constexpr uint8_t kStackWalkKeyDelete = 37;
}  // namespace opcodes

// Lowest canonical x64 kernel-mode address.
inline constexpr uint64_t kKernelAddressBase = 0xFFFF800000000000ull;

// One decoded EVENT_RECORD, reduced to what the converter needs. `payload` is only valid during the call.
struct EtwEvent {
  Guid provider;
  uint8_t opcode = 0;
  uint8_t version = 0;
  uint64_t timestamp = 0;  // raw QPC ticks (PROCESS_TRACE_MODE_RAW_TIMESTAMP)
  uint32_t threadId = 0;   // EVENT_HEADER.ThreadId
  uint32_t processId = 0;  // EVENT_HEADER.ProcessId
  uint16_t cpu = 0;        // BufferContext.ProcessorIndex
  Bytes payload;
};

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
  uint32_t threadId = 0;  // the thread being readied
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
  Bytes frames;        // depth * 8 bytes, little-endian addresses, leaf first
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
  std::string fileName;  // UTF-8, NT device path as logged
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
  std::string name;
};

// Process Start/End/DCStart/DCEnd (v3/v4), best effort; see docs/etw-notes.md.
struct ProcessPayload {
  uint32_t processId = 0;
  uint32_t parentId = 0;
  std::string imageFileName;  // ANSI image name following the variable-length SID, may be empty
  std::string commandLine;    // UTF-16 command line following the image name, may be empty
};

// Thread-safe: pure function. Converts UTF-16LE bytes (stops at the first NUL) to UTF-8.
inline std::string utf16LeToUtf8(Bytes bytes) {
  std::string out;
  const size_t units = bytes.size() / 2;
  for (size_t i = 0; i < units; ++i) {
    uint32_t cp = *core::readPod<uint16_t>(bytes, i * 2);
    if (cp == 0) break;
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units) {
      const uint32_t lo = *core::readPod<uint16_t>(bytes, (i + 1) * 2);
      if (lo >= 0xDC00 && lo < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// Thread-safe: pure function.
inline std::optional<SampledProfilePayload> decodeSampledProfile(Bytes payload) {
  if (payload.size() < kSampledProfileSize) return std::nullopt;
  core::ByteReader r(payload);
  SampledProfilePayload p;
  p.instructionPointer = *r.read<uint64_t>();
  p.threadId = *r.read<uint32_t>();
  p.count = *r.read<uint16_t>();
  p.reserved = *r.read<uint16_t>();
  return p;
}

// Thread-safe: pure function.
inline std::optional<CSwitchPayload> decodeCSwitch(Bytes payload) {
  if (payload.size() < kCSwitchSize) return std::nullopt;
  core::ByteReader r(payload);
  CSwitchPayload p;
  p.newThreadId = *r.read<uint32_t>();
  p.oldThreadId = *r.read<uint32_t>();
  p.newThreadPriority = *r.read<int8_t>();
  p.oldThreadPriority = *r.read<int8_t>();
  p.previousCState = *r.read<uint8_t>();
  p.spareByte = *r.read<int8_t>();
  p.oldThreadWaitReason = *r.read<int8_t>();
  p.oldThreadWaitMode = *r.read<int8_t>();
  p.oldThreadState = *r.read<int8_t>();
  p.oldThreadWaitIdealProcessor = *r.read<int8_t>();
  p.newThreadWaitTime = *r.read<uint32_t>();
  p.reserved = *r.read<uint32_t>();
  return p;
}

// Thread-safe: pure function.
inline std::optional<ReadyThreadPayload> decodeReadyThread(Bytes payload) {
  if (payload.size() < kReadyThreadSize) return std::nullopt;
  core::ByteReader r(payload);
  ReadyThreadPayload p;
  p.threadId = *r.read<uint32_t>();
  p.adjustReason = *r.read<int8_t>();
  p.adjustIncrement = *r.read<int8_t>();
  p.flag = *r.read<int8_t>();
  p.reserved = *r.read<int8_t>();
  return p;
}

// Thread-safe: pure function. Header and frame count only; frames stay a view over `payload`.
inline std::optional<StackWalkPayload> decodeStackWalk(Bytes payload) {
  if (payload.size() < kStackWalkHeaderSize) return std::nullopt;
  const size_t frameBytes = payload.size() - kStackWalkHeaderSize;
  if (frameBytes % sizeof(uint64_t) != 0) return std::nullopt;
  core::ByteReader r(payload);
  StackWalkPayload p;
  p.eventTimeStamp = *r.read<uint64_t>();
  p.stackProcess = *r.read<uint32_t>();
  p.stackThread = *r.read<uint32_t>();
  p.depth = static_cast<uint32_t>(frameBytes / sizeof(uint64_t));
  p.frames = payload.subspan(kStackWalkHeaderSize);
  return p;
}

// Thread-safe: pure function. Reads the i-th frame address of a decoded stack (0 if out of range).
inline uint64_t stackFrame(const StackWalkPayload& stack, uint32_t index) {
  if (index >= stack.depth) return 0;
  return core::readPod<uint64_t>(stack.frames, static_cast<size_t>(index) * sizeof(uint64_t)).value_or(0);
}

// Thread-safe: pure function.
inline std::optional<ImagePayload> decodeImage(Bytes payload) {
  if (payload.size() < kImageFixedSize) return std::nullopt;
  core::ByteReader r(payload);
  ImagePayload p;
  p.imageBase = *r.read<uint64_t>();
  p.imageSize = *r.read<uint64_t>();
  p.processId = *r.read<uint32_t>();
  p.imageChecksum = *r.read<uint32_t>();
  p.timeDateStamp = *r.read<uint32_t>();
  p.reserved0 = *r.read<uint32_t>();
  p.defaultBase = *r.read<uint64_t>();
  r.skip(16);  // Reserved1..4
  p.fileName = utf16LeToUtf8(*r.readBytes(r.remaining()));
  return p;
}

// Thread-safe: pure function.
inline std::optional<ThreadLifetimePayload> decodeThreadLifetime(Bytes payload) {
  core::ByteReader r(payload);
  auto pid = r.read<uint32_t>();
  auto tid = r.read<uint32_t>();
  if (!pid || !tid) return std::nullopt;
  return ThreadLifetimePayload{*pid, *tid};
}

// Thread-safe: pure function.
inline std::optional<ThreadSetNamePayload> decodeThreadSetName(Bytes payload) {
  core::ByteReader r(payload);
  auto pid = r.read<uint32_t>();
  auto tid = r.read<uint32_t>();
  if (!pid || !tid) return std::nullopt;
  ThreadSetNamePayload p;
  p.processId = *pid;
  p.threadId = *tid;
  p.name = utf16LeToUtf8(*r.readBytes(r.remaining()));
  return p;
}

namespace detail {
inline bool isPrintableAscii(char c) {
  return c >= 0x20 && c < 0x7F;
}

// Reads a NUL-terminated ANSI string at `offset`; empty when unterminated or non-printable. `end` receives
// the offset just past the NUL.
inline std::string readAnsiAt(Bytes payload, size_t offset, size_t& end) {
  std::string s;
  for (size_t i = offset; i < payload.size(); ++i) {
    const char c = static_cast<char>(payload[i]);
    if (c == '\0') {
      end = i + 1;
      return s;
    }
    if (!isPrintableAscii(c)) return {};
    s.push_back(c);
  }
  return {};
}
}  // namespace detail

// Thread-safe: pure function. v3/v4 x64: {ptr UniqueProcessKey; u32 ProcessId; u32 ParentId; u32 SessionId;
// i32 ExitStatus; ptr DirectoryTableBase; [v4: u32 Flags;] sid UserSID; char ImageFileName[]; wchar
// CommandLine[]}. The SID encoding is a [VERIFY] heuristic (see docs/etw-notes.md).
inline std::optional<ProcessPayload> decodeProcess(Bytes payload, uint8_t version) {
  core::ByteReader r(payload);
  if (!r.skip(8)) return std::nullopt;
  auto pid = r.read<uint32_t>();
  auto ppid = r.read<uint32_t>();
  if (!pid || !ppid) return std::nullopt;
  ProcessPayload p;
  p.processId = *pid;
  p.parentId = *ppid;
  const size_t sidOffset = 32 + (version >= 4 ? 4 : 0);
  if (payload.size() <= sidOffset) return p;
  size_t nameOffset = sidOffset + 8;
  const auto first = core::readPod<uint64_t>(payload, sidOffset);
  if (first && *first != 0) {
    const auto subCount = core::readPod<uint8_t>(payload, sidOffset + 17);
    if (subCount) nameOffset = sidOffset + 16 + 8 + 4 * static_cast<size_t>(*subCount);
  }
  size_t nameEnd = 0;
  p.imageFileName = detail::readAnsiAt(payload, nameOffset, nameEnd);
  if (p.imageFileName.empty()) {
    // Fallback: the first printable string of >= 3 characters that starts right after a NUL (so a scan never
    // begins in the middle of a string).
    for (size_t off = sidOffset; off + 3 < payload.size(); ++off) {
      if (off > 0 && payload[off - 1] != std::byte{0}) continue;
      std::string s = detail::readAnsiAt(payload, off, nameEnd);
      if (s.size() >= 3) {
        p.imageFileName = std::move(s);
        break;
      }
    }
  }
  if (!p.imageFileName.empty() && nameEnd < payload.size())
    p.commandLine = utf16LeToUtf8(payload.subspan(nameEnd));
  return p;
}

}  // namespace tachyon::etlconvert
