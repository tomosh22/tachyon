// Conversion of instrumentation timestamps into the trace clock.
//
// Today the SDK stamps events with the trace clock itself (QPC on Windows, CLOCK_MONOTONIC_RAW nanoseconds on
// Linux), so the default mapper is the identity. The interface exists for streams in another clock domain
// (the later TSC fast path): such a stream is mapped through the ClockCalibration records of the trace when
// there are enough of them, or by the frequency ratio otherwise.
#pragma once

#include "trace/trace_format.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tachyon::inst {

class IClockMapper {
 public:
  virtual ~IClockMapper() = default;
  // Thread-safe for const access. Trace-clock ticks for a timestamp of the stream's clock.
  virtual uint64_t toTraceTicks(uint64_t streamTicks) const = 0;
  // Thread-safe for const access.
  virtual bool isIdentity() const { return false; }
  // Thread-safe for const access. Short description for diagnostics ("identity", "scaled 1:10", ...).
  virtual std::string describe() const = 0;
};

// The default: stream ticks are trace ticks.
class IdentityClockMapper final : public IClockMapper {
 public:
  uint64_t toTraceTicks(uint64_t streamTicks) const override { return streamTicks; }
  bool isIdentity() const override { return true; }
  std::string describe() const override { return "identity"; }
};

// trace = stream * traceHz / streamHz, in 128-bit arithmetic. Assumes both clocks start at zero together.
class ScaledClockMapper final : public IClockMapper {
 public:
  // Thread-safe: pure. Both frequencies must be non-zero.
  ScaledClockMapper(uint64_t streamHz, uint64_t traceHz) : m_streamHz(streamHz), m_traceHz(traceHz) {}
  uint64_t toTraceTicks(uint64_t streamTicks) const override;
  std::string describe() const override;

 private:
  uint64_t m_streamHz, m_traceHz;
};

// Piecewise linear interpolation through (other_clock_ts -> trace_ts) calibration points, extrapolated with
// the first / last segment's slope. Needs at least two points with distinct other_clock_ts.
class CalibratedClockMapper final : public IClockMapper {
 public:
  struct Point {
    uint64_t other = 0;  // stream clock
    uint64_t trace = 0;  // trace clock
  };
  // Thread-safe: pure. `points` are sorted by `other` and de-duplicated by the constructor; fewer than two
  // distinct points are a programming error (use makeClockMapper, which checks).
  explicit CalibratedClockMapper(std::vector<Point> points);
  uint64_t toTraceTicks(uint64_t streamTicks) const override;
  std::string describe() const override;
  size_t pointCount() const { return m_points.size(); }

 private:
  std::vector<Point> m_points;
};

// What is known about the stream being mapped.
struct ClockContext {
  uint64_t streamClockHz = 0;  // StreamHeader.clock_frequency_hz
  uint64_t traceClockHz = 0;   // FileHeader.clock_frequency_hz
  // The trace's ClockCalibration records; only kClockTsc ones are used.
  std::span<const trace::ClockCalibrationRec> calibrations;
};

// Thread-safe: pure. Picks a mapper: identity when the frequencies match (or either is unknown), a
// calibrated mapper when the clocks differ and the trace has at least two distinct TSC calibration points,
// else a scaled mapper. `note` (optional) receives a warning when the choice is a fallback.
std::unique_ptr<IClockMapper> makeClockMapper(const ClockContext& context, std::string* note = nullptr);

}  // namespace tachyon::inst
