// Platform-independent parts of the ETW session recipe (WP-11): buffer sizing, enable flags, stack-walk
// selection, sampling interval, session naming/GUIDs, orphan matching and the SessionConfig handed to
// EtwSession. Compiled and unit-tested on every platform; only etw_session.cpp talks to advapi32.
#pragma once

#include "capture/capture_backend.h"
#include "core/result.h"
#include "etlconvert/etl_events.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::capture::etw {

// ---------------------------------------------------------------------------------------------------------
// Buffer sizing

struct BufferSettings {
  uint32_t bufferSizeKb = 1024;
  uint32_t minimumBuffers = 64;
  uint32_t maximumBuffers = 256;
  uint32_t flushTimerSeconds = 1;
};

// Explicit overrides; an unset field uses the formula.
struct BufferOverrides {
  std::optional<uint32_t> bufferSizeKb;
  std::optional<uint32_t> minimumBuffers;
  std::optional<uint32_t> maximumBuffers;
  std::optional<uint32_t> flushTimerSeconds;
};

inline constexpr uint32_t kDefaultBufferSizeKb = 1024;
inline constexpr uint32_t kMinBufferCount = 64;
inline constexpr uint32_t kMaxBufferCountCap = 512;  // 512 x 1 MB of non-paged memory at most
inline constexpr uint32_t kDefaultSamplingHz = 8000;

// Thread-safe: pure function.
// Formula:
//   BufferSize     = 1024 KB
//   MinimumBuffers = max(64, 4 x logicalCores)           (a few buffers per CPU so no CPU ever waits)
//   MaximumBuffers = MinimumBuffers x (2 + ceil(hz / 4000)), capped at 512 buffers, never below Minimum.
//                    8 kHz gives 4x (the WP-01 recipe), 16 kHz or more would give 6x; a lower rate needs
//                    less headroom for bursts (context switches dominate, not samples).
//   FlushTimer     = 1 s
// Overrides replace individual values after the formula; the result is then clamped so that
// 64 <= bufferSizeKb <= 16384, 2 <= minimumBuffers <= maximumBuffers.
BufferSettings computeBufferSettings(uint32_t logicalCores, uint32_t samplingHz,
                                     const BufferOverrides& overrides = {});

// ---------------------------------------------------------------------------------------------------------
// Sampling interval and enable flags

// Smallest TraceSampledProfileIntervalInfo value Windows accepts, in 100 ns units (122.1 us, ~8.19 kHz).
inline constexpr uint32_t kMinProfileIntervalUnits = 1221;

// Thread-safe: pure function. Requested rate -> 100 ns units for TRACE_PROFILE_INTERVAL.Interval. A rate of 0
// means the default (8 kHz); rates of 8000 Hz or more use the hardware-clock minimum (1221).
uint32_t profileIntervalUnits(uint32_t hz);
// Thread-safe: pure function. Actual sampling rate for an interval in 100 ns units.
double intervalUnitsToHz(uint32_t units);

// EVENT_TRACE_FLAG_* values (evntrace.h), duplicated so Linux tests can check the composition; the Windows
// build static_asserts that they match the SDK.
inline constexpr uint32_t kFlagProcess = 0x00000001;
inline constexpr uint32_t kFlagThread = 0x00000002;
inline constexpr uint32_t kFlagImageLoad = 0x00000004;
inline constexpr uint32_t kFlagCSwitch = 0x00000010;
inline constexpr uint32_t kFlagDispatcher = 0x00000800;
inline constexpr uint32_t kFlagProfile = 0x01000000;

// Thread-safe: pure function. Process | Thread | ImageLoad | Profile | CSwitch | Dispatcher. Context switches
// and ready events are the basis of thread states, so they are always on; the stackCSwitch / stackReady /
// stackProfile options only choose which events get a stack walk (see stackEventsFor).
uint32_t enableFlagsFor(const CaptureOptions& options);

// One CLASSIC_EVENT_ID (provider GUID + opcode) that TraceStackTracingInfo should walk.
struct StackEventId {
  etlconvert::Guid provider;
  uint8_t opcode = 0;
};

// Thread-safe: pure function. SampledProfile if stackProfile, CSwitch if stackCSwitch, ReadyThread if
// stackReady, in that order. `kernelStacks` cannot be separated from user stacks by ETW; it is recorded in
// the capture options and left for the converter.
std::vector<StackEventId> stackEventsFor(const CaptureOptions& options);

// ---------------------------------------------------------------------------------------------------------
// Session naming, GUIDs and orphan matching

inline constexpr std::string_view kSessionNamePrefix = "Tachyon";

// Thread-safe: pure function. "Tachyon <pid> <counter>".
std::string makeSessionName(uint32_t pid, uint32_t counter);

// Thread-safe: pure function. Unique per (pid, counter); `stamp` (any time-like value) breaks ties between
// reused pids.
etlconvert::Guid makeSessionGuid(uint32_t pid, uint32_t counter, uint64_t stamp);

struct SessionNameInfo {
  bool tachyon = false;             // starts with "Tachyon " (prefix followed by a space)
  std::optional<uint32_t> pid;      // the first all-digit token after the prefix (non-zero)
  std::optional<uint32_t> counter;  // the digit token after the pid, if any
};

// Thread-safe: pure function. Understands "Tachyon <pid> <counter>" (this library) and "Tachyon etlstat
// <pid>" (WP-01 tool): the owning pid is the first all-digit token after the prefix.
SessionNameInfo parseSessionName(std::string_view name);

enum class OrphanVerdict : uint8_t {
  NotTachyon,   // not ours: never touch
  Unparseable,  // Tachyon-prefixed but no pid in the name: cannot prove it is dead, leave it alone
  Alive,        // the owning process still exists (a live sibling agent or tool): leave it alone
  Orphan,       // the owning process is gone: safe to stop
};

// Thread-safe if `isProcessAlive` is. Decides whether an enumerated logger is an orphan of a crashed agent.
OrphanVerdict classifySession(std::string_view loggerName,
                              const std::function<bool(uint32_t)>& isProcessAlive);

// ---------------------------------------------------------------------------------------------------------
// The complete description of one ETW session

struct SessionConfig {
  std::string name;
  etlconvert::Guid guid;
  std::filesystem::path etlPath;
  bool circular = false;
  uint32_t maxFileMb = 0;  // MaximumFileSize; the circular cap (0 = unlimited sequential file)
  BufferSettings buffers;
  uint32_t enableFlags = 0;
  uint32_t profileIntervalUnits = 0;
  std::vector<StackEventId> stackEvents;
};

inline constexpr uint32_t kMinCircularMb = 4;
inline constexpr uint32_t kMaxCircularMb = 64 * 1024;

// Thread-safe: pure function. Validates and composes the config for one segment of a capture. circularMb > 0
// selects circular mode (EVENT_TRACE_FILE_MODE_CIRCULAR with MaximumFileSize = circularMb), else sequential.
Result<SessionConfig> makeSessionConfig(const CaptureOptions& options, std::string name,
                                        const etlconvert::Guid& guid, std::filesystem::path etlPath,
                                        uint32_t logicalCores, const BufferOverrides& overrides = {});

// Thread-safe: pure function. Validates the parts of CaptureOptions the ETW backend supports: System needs
// nothing more, Attach needs a pid, Launch needs an exe; outPath must be set.
Result<void> validateCaptureOptions(const CaptureOptions& options);

// Thread-safe: pure function. Segment 0 is "<out>.etl" (for example "capture.tachyon.etl"); segment N > 0 is
// "<out>.<N>.etl". Every flush restarts the session on the next segment.
std::filesystem::path etlSegmentPath(const std::filesystem::path& out, uint32_t segment);

// Thread-safe: pure function. A flush name is a bare file stem: not empty, no separators, not "." or "..".
bool isValidFlushName(std::string_view name);

// Thread-safe: pure function. JSON object embedded in the trace as capture_options. `targetPids` are the pids
// handed to the converter (roots plus followed children); empty in system mode.
std::string captureOptionsJson(const CaptureOptions& options, const SessionConfig& config,
                               const std::vector<uint32_t>& targetPids = {});

}  // namespace tachyon::capture::etw
