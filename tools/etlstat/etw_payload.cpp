#include "etw_payload.h"

#include <cstring>

#include "core/span_util.h"

namespace tachyon::etlstat {

using core::ByteReader;
using core::readPod;

std::optional<SampledProfilePayload> decodeSampledProfile(Bytes payload) {
  ByteReader r(payload);
  SampledProfilePayload p;
  auto ip = r.read<uint64_t>();
  auto tid = r.read<uint32_t>();
  auto count = r.read<uint16_t>();
  auto reserved = r.read<uint16_t>();
  if (!ip || !tid || !count || !reserved) return std::nullopt;
  p.instructionPointer = *ip;
  p.threadId = *tid;
  p.count = *count;
  p.reserved = *reserved;
  return p;
}

std::optional<CSwitchPayload> decodeCSwitch(Bytes payload) {
  if (payload.size() < kCSwitchSize) return std::nullopt;
  ByteReader r(payload);
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

std::optional<ReadyThreadPayload> decodeReadyThread(Bytes payload) {
  if (payload.size() < kReadyThreadSize) return std::nullopt;
  ByteReader r(payload);
  ReadyThreadPayload p;
  p.threadId = *r.read<uint32_t>();
  p.adjustReason = *r.read<int8_t>();
  p.adjustIncrement = *r.read<int8_t>();
  p.flag = *r.read<int8_t>();
  p.reserved = *r.read<int8_t>();
  return p;
}

std::optional<StackWalkPayload> decodeStackWalk(Bytes payload) {
  if (payload.size() < kStackWalkHeaderSize) return std::nullopt;
  const size_t frameBytes = payload.size() - kStackWalkHeaderSize;
  if (frameBytes % sizeof(uint64_t) != 0) return std::nullopt;
  ByteReader r(payload);
  StackWalkPayload p;
  p.eventTimeStamp = *r.read<uint64_t>();
  p.stackProcess = *r.read<uint32_t>();
  p.stackThread = *r.read<uint32_t>();
  p.depth = static_cast<uint32_t>(frameBytes / sizeof(uint64_t));
  p.frames = payload.subspan(kStackWalkHeaderSize);
  return p;
}

uint64_t stackFrame(const StackWalkPayload& stack, uint32_t index) {
  if (index >= stack.depth) return 0;
  return readPod<uint64_t>(stack.frames, static_cast<size_t>(index) * sizeof(uint64_t)).value_or(0);
}

std::string utf16LeToUtf8(Bytes bytes) {
  std::string out;
  const size_t units = bytes.size() / 2;
  for (size_t i = 0; i < units; ++i) {
    uint32_t cp = *readPod<uint16_t>(bytes, i * 2);
    if (cp == 0) break;
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units) {
      const uint32_t lo = *readPod<uint16_t>(bytes, (i + 1) * 2);
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

std::optional<ImagePayload> decodeImage(Bytes payload) {
  if (payload.size() < kImageFixedSize) return std::nullopt;
  ByteReader r(payload);
  ImagePayload p;
  p.imageBase = *r.read<uint64_t>();
  p.imageSize = *r.read<uint64_t>();
  p.processId = *r.read<uint32_t>();
  p.imageChecksum = *r.read<uint32_t>();
  p.timeDateStamp = *r.read<uint32_t>();
  p.reserved0 = *r.read<uint32_t>();
  p.defaultBase = *r.read<uint64_t>();
  for (uint32_t& v : p.reserved) v = *r.read<uint32_t>();
  p.fileName = utf16LeToUtf8(*r.readBytes(r.remaining()));
  return p;
}

std::optional<ThreadLifetimePayload> decodeThreadLifetime(Bytes payload) {
  ByteReader r(payload);
  auto pid = r.read<uint32_t>();
  auto tid = r.read<uint32_t>();
  if (!pid || !tid) return std::nullopt;
  return ThreadLifetimePayload{*pid, *tid};
}

std::optional<ThreadSetNamePayload> decodeThreadSetName(Bytes payload) {
  ByteReader r(payload);
  auto pid = r.read<uint32_t>();
  auto tid = r.read<uint32_t>();
  if (!pid || !tid) return std::nullopt;
  ThreadSetNamePayload p;
  p.processId = *pid;
  p.threadId = *tid;
  p.name = utf16LeToUtf8(*r.readBytes(r.remaining()));
  return p;
}

namespace {
bool isPrintableAscii(char c) {
  return c >= 0x20 && c < 0x7F;
}

// Reads a NUL-terminated ANSI string at `offset`; empty when unterminated or non-printable.
std::string readAnsiAt(Bytes payload, size_t offset) {
  std::string s;
  for (size_t i = offset; i < payload.size(); ++i) {
    const char c = static_cast<char>(payload[i]);
    if (c == '\0') return s;
    if (!isPrintableAscii(c)) return {};
    s.push_back(c);
  }
  return {};
}
}  // namespace

std::optional<ProcessPayload> decodeProcess(Bytes payload, uint8_t version) {
  // v3/v4 x64: {ptr UniqueProcessKey; u32 ProcessId; u32 ParentId; u32 SessionId; i32 ExitStatus;
  //             ptr DirectoryTableBase; [v4: u32 Flags;] sid UserSID; char ImageFileName[]; wchar
  //             CommandLine[]}
  ByteReader r(payload);
  if (!r.skip(8)) return std::nullopt;
  auto pid = r.read<uint32_t>();
  auto ppid = r.read<uint32_t>();
  if (!pid || !ppid) return std::nullopt;
  ProcessPayload p;
  p.processId = *pid;
  p.parentId = *ppid;
  // Fixed part before the SID: 8 + 4 + 4 + 4 + 4 + 8 (+ 4 for v4 flags).
  const size_t sidOffset = 32 + (version >= 4 ? 4 : 0);
  if (payload.size() <= sidOffset) return p;
  // SID encoding (to be confirmed): a zero pointer-sized value means "no SID" (8 bytes); otherwise a
  // TOKEN_USER-like 16-byte header followed by SID{rev, subAuthCount, idAuth[6], subAuth[count]}.
  size_t nameOffset = sidOffset + 8;
  const auto first = readPod<uint64_t>(payload, sidOffset);
  if (first && *first != 0) {
    const auto subCount = readPod<uint8_t>(payload, sidOffset + 17);
    if (subCount) nameOffset = sidOffset + 16 + 8 + 4 * static_cast<size_t>(*subCount);
  }
  p.imageFileName = readAnsiAt(payload, nameOffset);
  if (p.imageFileName.empty()) {
    // Fallback heuristic: first printable NUL-terminated string of >= 3 chars after the fixed part.
    for (size_t off = sidOffset; off + 3 < payload.size(); ++off) {
      std::string s = readAnsiAt(payload, off);
      if (s.size() >= 3) {
        p.imageFileName = std::move(s);
        break;
      }
    }
  }
  return p;
}

}  // namespace tachyon::etlstat
