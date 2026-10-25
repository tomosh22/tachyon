#include "inst/clock_mapper.h"

#include <algorithm>
#include <cstring>

namespace tachyon::inst {

namespace {

constexpr uint64_t kMax64 = ~uint64_t{0};

// floor(a * b / d) saturated to 64 bits, with a 128-bit intermediate and no compiler extensions (MSVC has no
// __int128). d must be non-zero.
uint64_t mulDivSat(uint64_t a, uint64_t b, uint64_t d) {
  const uint64_t aLo = a & 0xFFFFFFFFu, aHi = a >> 32;
  const uint64_t bLo = b & 0xFFFFFFFFu, bHi = b >> 32;
  const uint64_t p0 = aLo * bLo, p1 = aLo * bHi, p2 = aHi * bLo, p3 = aHi * bHi;
  const uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu);
  const uint64_t lo = (mid << 32) | (p0 & 0xFFFFFFFFu);
  const uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
  if (hi >= d) return kMax64;  // the quotient does not fit
  uint64_t rem = hi;
  uint64_t quotient = 0;
  for (int bit = 63; bit >= 0; --bit) {
    const bool carry = (rem >> 63) != 0;
    rem = (rem << 1) | ((lo >> bit) & 1u);
    quotient <<= 1;
    if (carry || rem >= d) {
      rem -= d;
      quotient |= 1u;
    }
  }
  return quotient;
}

}  // namespace

uint64_t ScaledClockMapper::toTraceTicks(uint64_t streamTicks) const {
  return mulDivSat(streamTicks, m_traceHz, m_streamHz);
}

std::string ScaledClockMapper::describe() const {
  return "scaled " + std::to_string(m_streamHz) + " Hz -> " + std::to_string(m_traceHz) + " Hz";
}

CalibratedClockMapper::CalibratedClockMapper(std::vector<Point> points) : m_points(std::move(points)) {
  std::stable_sort(m_points.begin(), m_points.end(),
                   [](const Point& a, const Point& b) { return a.other < b.other; });
  m_points.erase(std::unique(m_points.begin(), m_points.end(),
                             [](const Point& a, const Point& b) { return a.other == b.other; }),
                 m_points.end());
}

uint64_t CalibratedClockMapper::toTraceTicks(uint64_t streamTicks) const {
  if (m_points.size() < 2) return streamTicks;
  // The segment containing the timestamp; outside the range the first / last segment is extended.
  size_t hi = static_cast<size_t>(std::upper_bound(m_points.begin(), m_points.end(), streamTicks,
                                                   [](uint64_t v, const Point& p) { return v < p.other; }) -
                                  m_points.begin());
  if (hi == 0) hi = 1;
  if (hi >= m_points.size()) hi = m_points.size() - 1;
  const Point& a = m_points[hi - 1];
  const Point& b = m_points[hi];
  // trace = a.trace + (t - a.other) * (b.trace - a.trace) / (b.other - a.other), with explicit signs.
  const bool before = streamTicks < a.other;
  const uint64_t dt = before ? a.other - streamTicks : streamTicks - a.other;
  const bool falling = b.trace < a.trace;
  const uint64_t dTrace = falling ? a.trace - b.trace : b.trace - a.trace;
  const uint64_t delta = mulDivSat(dt, dTrace, b.other - a.other);
  const bool subtract = before != falling;
  if (subtract) return a.trace > delta ? a.trace - delta : 0;
  return delta > kMax64 - a.trace ? kMax64 : a.trace + delta;
}

std::string CalibratedClockMapper::describe() const {
  return "calibrated through " + std::to_string(m_points.size()) + " points";
}

std::unique_ptr<IClockMapper> makeClockMapper(const ClockContext& context, std::string* note) {
  if (context.streamClockHz == 0 || context.traceClockHz == 0 ||
      context.streamClockHz == context.traceClockHz)
    return std::make_unique<IdentityClockMapper>();
  std::vector<CalibratedClockMapper::Point> points;
  for (const trace::ClockCalibrationRec& rec : context.calibrations) {
    trace::ClockCalibrationRec r;
    std::memcpy(&r, &rec, sizeof(r));  // packed record: copy before touching members
    if (r.other_clock_kind == trace::kClockTsc) points.push_back({r.other_clock_ts, r.trace_ts});
  }
  auto calibrated = std::make_unique<CalibratedClockMapper>(std::move(points));
  if (calibrated->pointCount() >= 2) return calibrated;
  if (note != nullptr)
    *note = "instrumentation clock (" + std::to_string(context.streamClockHz) +
            " Hz) differs from the trace clock (" + std::to_string(context.traceClockHz) +
            " Hz) and the trace has no usable TSC calibration; timestamps were scaled by the frequency ratio";
  return std::make_unique<ScaledClockMapper>(context.streamClockHz, context.traceClockHz);
}

}  // namespace tachyon::inst
