// Instrumentation wire format (SDK -> agent / sidecar file). Header-only and dependency-free so that the
// SDK runtime (WP-51), the agent ingestion (WP-52) and tests can all include it. Little-endian only. The
// prose description is in inst_wire.md. A stream (agent pipe/socket or `.tachyon-inst` sidecar file) is a
// sequence of frames: FrameHeader followed by `byte_len` payload bytes. The first frame is StreamHeader.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tachyon::inst {

constexpr uint32_t kWireVersion = 1;
// "TINS" as little-endian bytes 'T','I','N','S'.
constexpr uint32_t kStreamMagic = 0x534E4954u;

// Limits shared by writer and readers.
constexpr uint32_t kMaxDataBytes = 4096;  // scope/marker data, string definitions (truncated, not rejected)
constexpr uint32_t kMaxThreadNameBytes = 256;                // thread names are truncated to this
constexpr uint32_t kMaxFrameBytes = 8 + 64 + kMaxDataBytes;  // upper bound of header + payload

enum class FrameType : uint32_t {
  StreamHeader = 1,  // first frame of every stream
  StringDef = 2,     // static string definition, referenced by ptr_key
  ScopeBegin = 3,
  ScopeEnd = 4,
  FiberEvent = 5,
  FrameMark = 6,
  Marker = 7,
  Plot = 8,
  ThreadName = 9,
  Dropped = 10,
};

enum class Platform : uint32_t { Windows = 1, Linux = 2 };

enum class FiberEventKind : uint8_t { Register = 0, Unregister = 1, SwitchBegin = 2, SwitchEnd = 3 };

// ptr_key: low 48 bits are the string's pointer value, high 16 bits are a tag. Tag 0 = NUL-terminated string
// (Tachyon_BeginEvent & co, length found with strlen on first use); tag L+1 = explicit-length variant with
// length L (Tachyon_BeginEvent_N, L <= kMaxDataBytes). The same pointer used with two different lengths
// therefore yields two distinct keys and two StringDef frames, so a key always maps to exactly one string.
// Keys are opaque to readers; the agent remaps them to StringIdx at conversion.
constexpr uint64_t kPtrKeyPointerMask = 0x0000FFFFFFFFFFFFull;
constexpr uint64_t makeStringKey(uint64_t pointerValue, uint32_t tag) {
  return (pointerValue & kPtrKeyPointerMask) | (static_cast<uint64_t>(tag) << 48);
}

#pragma pack(push, 1)

struct FrameHeader {
  uint32_t type;      // FrameType
  uint32_t byte_len;  // payload bytes following this header
};

struct StreamHeaderPayload {
  uint32_t magic;  // kStreamMagic
  uint32_t wire_version;
  uint32_t pid;
  uint32_t platform;  // Platform
  uint64_t clock_frequency_hz;
  uint64_t process_start_ts;  // trace-clock ticks when the SDK runtime started
  uint32_t api_version;       // TACHYON_API_VERSION of the SDK
  uint32_t reserved;
};

struct StringDefPayload {
  uint64_t ptr_key;
  uint32_t len;
  // uint8_t bytes[len] follows (UTF-8, no terminator)
};

struct ScopeBeginPayload {
  uint64_t ts;
  uint32_t tid;
  uint32_t color_rgba;
  uint64_t id_key;       // ptr_key of a StringDef
  uint64_t caller_addr;  // return address into the function that began the scope
  uint16_t data_len;
  // uint8_t data[data_len] follows (dynamic string, inline, <= kMaxDataBytes)
};

struct ScopeEndPayload {
  uint64_t ts;
  uint32_t tid;
};

struct FiberEventPayload {
  uint64_t ts;
  uint32_t tid;
  uint8_t kind;  // FiberEventKind
  uint8_t reserved[3];
  uint64_t fiber_id;
  uint64_t other_fiber_id;  // switch_begin: the fiber being switched to; otherwise 0
};

struct FrameMarkPayload {
  uint64_t ts;
  uint32_t tid;
  uint64_t name_key;
  uint64_t frame_number;
};

struct MarkerPayload {
  uint64_t ts;
  uint32_t tid;
  uint64_t name_key;
  uint32_t color_rgba;
  uint16_t data_len;
  // uint8_t data[data_len] follows
};

struct PlotPayload {
  uint64_t ts;
  uint64_t name_key;
  double value;
};

struct ThreadNamePayload {
  uint64_t ts;
  uint32_t tid;
  uint16_t name_len;
  // uint8_t name[name_len] follows
};

// Records the producer could not store because its ring was full. The count is the number of dropped
// records (each Begin, End, marker, ... counts one) since the previous Dropped frame for that thread.
struct DroppedPayload {
  uint64_t ts;  // when the loss was reported (not when it happened)
  uint32_t tid;
  uint64_t count;
};

#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 8, "wire layout");
static_assert(sizeof(StreamHeaderPayload) == 40, "wire layout");
static_assert(sizeof(StringDefPayload) == 12, "wire layout");
static_assert(sizeof(ScopeBeginPayload) == 34, "wire layout");
static_assert(sizeof(ScopeEndPayload) == 12, "wire layout");
static_assert(sizeof(FiberEventPayload) == 32, "wire layout");
static_assert(sizeof(FrameMarkPayload) == 28, "wire layout");
static_assert(sizeof(MarkerPayload) == 26, "wire layout");
static_assert(sizeof(PlotPayload) == 24, "wire layout");
static_assert(sizeof(ThreadNamePayload) == 14, "wire layout");
static_assert(sizeof(DroppedPayload) == 20, "wire layout");

// Total on-the-wire size (header + payload) of the fixed-size frames.
constexpr uint32_t kScopeEndFrameBytes = sizeof(FrameHeader) + sizeof(ScopeEndPayload);

}  // namespace tachyon::inst
