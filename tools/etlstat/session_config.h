// Platform-independent parts of the ETW session recipe: buffer sizing, sampling interval, enable flags.
#pragma once

#include <cstdint>

namespace tachyon::etlstat {

struct BufferSettings {
  uint32_t bufferSizeKb = 1024;
  uint32_t minimumBuffers = 64;
  uint32_t maximumBuffers = 256;
};

// Thread-safe: pure function. BufferSize = 1024 KB, MinimumBuffers = max(64, 4 x cores),
// MaximumBuffers = 4 x MinimumBuffers (headroom for CSwitch bursts; ~256 MB non-paged at 8 cores).
BufferSettings computeBufferSettings(uint32_t logicalCores);

// Smallest TraceSampledProfileIntervalInfo value Windows accepts, in 100 ns units (122.1 us, ~8.19 kHz).
inline constexpr uint32_t kMinProfileIntervalUnits = 1221;

// Thread-safe: pure function. Converts a requested rate to the 100 ns interval units passed to
// TRACE_PROFILE_INTERVAL.Interval. Requests of 8000 Hz or more use the hardware-clock minimum (1221, which
// is what the spec's "--hz 8000" means); lower rates use 10,000,000 / hz. A rate of 0 is treated as 1 Hz.
uint32_t profileIntervalUnits(uint32_t hz);

// Thread-safe: pure function. Actual sampling rate in Hz for an interval in 100 ns units.
double intervalUnitsToHz(uint32_t units);

// EVENT_TRACE_FLAG_* values (evntrace.h), duplicated so Linux tests can check the composition; the Windows
// build static_asserts that they match the SDK.
inline constexpr uint32_t kFlagProcess = 0x00000001;
inline constexpr uint32_t kFlagThread = 0x00000002;
inline constexpr uint32_t kFlagImageLoad = 0x00000004;
inline constexpr uint32_t kFlagCSwitch = 0x00000010;
inline constexpr uint32_t kFlagDispatcher = 0x00000800;
inline constexpr uint32_t kFlagProfile = 0x01000000;
inline constexpr uint32_t kEnableFlags =
    kFlagProcess | kFlagThread | kFlagImageLoad | kFlagProfile | kFlagCSwitch | kFlagDispatcher;

}  // namespace tachyon::etlstat
