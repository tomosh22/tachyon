// Platform-independent ETW vocabulary used by etlstat: GUIDs, kernel provider ids, opcodes, event views.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tachyon::etlstat {

// Binary layout identical to the Windows GUID struct.
struct Guid {
  uint32_t data1 = 0;
  uint16_t data2 = 0;
  uint16_t data3 = 0;
  std::array<uint8_t, 8> data4{};

  friend bool operator==(const Guid& a, const Guid& b) = default;
  friend auto operator<=>(const Guid& a, const Guid& b) = default;
};
static_assert(sizeof(Guid) == 16);

// Thread-safe: pure function. Parses "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (optionally in braces).
std::optional<Guid> parseGuid(std::string_view text);
// Thread-safe: pure function. Lower-case, no braces.
std::string guidToString(const Guid& guid);

namespace guids {
inline constexpr Guid kProcess{0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
inline constexpr Guid kThread{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
inline constexpr Guid kImage{0x2cb15d1d, 0x5fc1, 0x11d2, {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};
inline constexpr Guid kPerfInfo{0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
inline constexpr Guid kStackWalk{
    0xdef2fe46, 0x7bd6, 0x4b80, {0xbd, 0x94, 0xf5, 0x7f, 0xe2, 0x0d, 0x0c, 0xe3}};
// Header event that starts every ETL file.
inline constexpr Guid kEventTrace{
    0x68fdd900, 0x4a3e, 0x11d1, {0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3}};
}  // namespace guids

// Thread-safe: pure function. "Process", "Thread", ... or the GUID text for unknown providers.
std::string providerName(const Guid& guid);

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
inline constexpr uint8_t kImageLoad = 10;
inline constexpr uint8_t kImageUnload = 2;
inline constexpr uint8_t kImageDcStart = 3;
inline constexpr uint8_t kImageDcEnd = 4;
inline constexpr uint8_t kPerfInfoSampledProfile = 46;
inline constexpr uint8_t kStackWalkStack = 32;
inline constexpr uint8_t kStackWalkKeyKernel = 34;
inline constexpr uint8_t kStackWalkKeyUser = 35;
inline constexpr uint8_t kStackWalkKeyRundown = 36;
inline constexpr uint8_t kStackWalkKeyDelete = 37;
}  // namespace opcodes

// Thread-safe: pure function. Human-readable opcode name for a provider, empty when unknown.
std::string_view opcodeName(const Guid& provider, uint8_t opcode);

// One decoded EVENT_RECORD, reduced to what etlstat needs. `payload` is only valid during the callback.
struct EtwEvent {
  Guid provider;
  uint8_t opcode = 0;
  uint8_t version = 0;
  uint64_t timestamp = 0;  // raw QPC ticks (PROCESS_TRACE_MODE_RAW_TIMESTAMP)
  uint32_t threadId = 0;   // EVENT_HEADER.ThreadId
  uint32_t processId = 0;  // EVENT_HEADER.ProcessId
  uint16_t cpu = 0;        // BufferContext.ProcessorIndex
  std::span<const std::byte> payload;
};

}  // namespace tachyon::etlstat
