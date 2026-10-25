#include "viewer/timeline/timeline_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tachyon::viewer::timeline {

namespace {

constexpr double kMaxQuerySpan = 140737488355328.0;  // 2^47, analysis::kMaxQuerySpanTicks

double effectiveHz(double hz) {
  return hz > 0.0 ? hz : 1e9;
}

}  // namespace

TimeView fitView(const TimeLimits& limits) {
  return {0.0, limits.total()};
}

TimeView clampView(TimeView view, const TimeLimits& limits) {
  const double total = limits.total();
  const double minSpan = std::min(limits.minSpan, total);
  if (!(view.span == view.span)) view.span = total;  // NaN
  view.span = std::clamp(view.span, minSpan, total);
  if (!(view.start == view.start)) view.start = 0.0;
  view.start = std::clamp(view.start, 0.0, total - view.span);
  return view;
}

TimeView zoomAtCursor(TimeView view, const TimeLimits& limits, double cursorFrac, double factor) {
  if (!(factor > 0.0)) return clampView(view, limits);
  cursorFrac = std::clamp(cursorFrac, 0.0, 1.0);
  view = clampView(view, limits);
  const double anchor = view.start + cursorFrac * view.span;
  TimeView out;
  const double total = limits.total();
  out.span = std::clamp(view.span / factor, std::min(limits.minSpan, total), total);
  out.start = anchor - cursorFrac * out.span;
  return clampView(out, limits);
}

TimeView panTicks(TimeView view, const TimeLimits& limits, double deltaTicks) {
  view = clampView(view, limits);
  view.start += deltaTicks;
  return clampView(view, limits);
}

TimeView panPixels(TimeView view, const TimeLimits& limits, double dxPixels, double widthPixels) {
  if (!(widthPixels > 0.0)) return clampView(view, limits);
  view = clampView(view, limits);
  return panTicks(view, limits, -dxPixels * view.span / widthPixels);
}

TimeView centerOn(TimeView view, const TimeLimits& limits, double relTick) {
  view = clampView(view, limits);
  view.start = relTick - view.span * 0.5;
  return clampView(view, limits);
}

TimeView viewFromRange(const TimeLimits& limits, TickRange range, double margin) {
  if (range.t1 < range.t0) std::swap(range.t0, range.t1);
  const double a = toRelative(limits, range.t0), b = toRelative(limits, range.t1);
  const double pad = (b - a) * std::max(margin, 0.0);
  TimeView v;
  v.start = a - pad;
  v.span = (b - a) + 2.0 * pad;
  return clampView(v, limits);
}

double xToRelTick(const TimeView& view, double x, double areaX0, double areaWidth) {
  if (!(areaWidth > 0.0)) return view.start;
  return view.start + (x - areaX0) / areaWidth * view.span;
}

double relTickToX(const TimeView& view, double relTick, double areaX0, double areaWidth) {
  if (!(view.span > 0.0)) return areaX0;
  return areaX0 + (relTick - view.start) / view.span * areaWidth;
}

uint64_t toAbsolute(const TimeLimits& limits, double relTick) {
  const double total = limits.total();
  if (!(relTick > 0.0)) return limits.traceStart;
  if (relTick >= total) return limits.traceStart + static_cast<uint64_t>(total);
  return limits.traceStart + static_cast<uint64_t>(std::floor(relTick));
}

double toRelative(const TimeLimits& limits, uint64_t absTick) {
  if (absTick <= limits.traceStart) return 0.0;
  return std::min(static_cast<double>(absTick - limits.traceStart), limits.total());
}

TickRange queryWindow(const TimeView& view, const TimeLimits& limits) {
  const TimeView v = clampView(view, limits);
  TickRange r;
  r.t0 = toAbsolute(limits, v.start);
  r.t1 = toAbsolute(limits, std::ceil(v.end()));
  if (r.t1 <= r.t0) r.t1 = r.t0 + 1;
  if (static_cast<double>(r.t1 - r.t0) > kMaxQuerySpan) r.t1 = r.t0 + static_cast<uint64_t>(kMaxQuerySpan);
  return r;
}

RulerSpec chooseRulerStep(double spanTicks, double clockHz, double widthPixels, double minMajorPixels) {
  const double hz = effectiveHz(clockHz);
  const double spanNs = spanTicks * 1e9 / hz;
  RulerSpec spec;
  if (!(widthPixels > 0.0) || !(spanNs > 0.0)) return spec;
  const double minStepNs = spanNs * std::max(minMajorPixels, 1.0) / widthPixels;
  // Smallest 1-2-5 * 10^k that is >= minStepNs.
  const double exponent = std::floor(std::log10(std::max(minStepNs, 1e-3)));
  const double base = std::pow(10.0, exponent);
  for (const double m : {1.0, 2.0, 5.0, 10.0}) {
    if (base * m >= minStepNs * (1.0 - 1e-12)) {
      spec.majorStepNs = base * m;
      spec.minorDivisions = (m == 2.0) ? 4 : 5;
      return spec;
    }
  }
  spec.majorStepNs = base * 10.0;
  return spec;
}

std::string formatTimeNs(double ns, double stepNs) {
  const double a = std::fabs(ns);
  const char* unit = "ns";
  double divisor = 1.0;
  if (a >= 1e9) {
    unit = "s";
    divisor = 1e9;
  } else if (a >= 1e6) {
    unit = "ms";
    divisor = 1e6;
  } else if (a >= 1e3) {
    unit = "us";
    divisor = 1e3;
  }
  const double step = std::max(std::fabs(stepNs), 1e-3) / divisor;
  int decimals = 0;
  if (step < 1.0) decimals = std::min(9, static_cast<int>(std::ceil(-std::log10(step) - 1e-9)));
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f %s", decimals, ns / divisor, unit);
  return buf;
}

std::string formatDurationTicks(uint64_t ticks, double clockHz) {
  const double ns = static_cast<double>(ticks) * 1e9 / effectiveHz(clockHz);
  const double a = std::max(ns, 1.0);
  const double step = std::pow(10.0, std::floor(std::log10(a)) - 2.0);
  return formatTimeNs(ns, std::max(step, 1.0));
}

std::vector<RulerTick> buildRuler(const TimeView& view, double clockHz, double widthPixels,
                                  double minMajorPixels) {
  std::vector<RulerTick> out;
  const double hz = effectiveHz(clockHz);
  const RulerSpec spec = chooseRulerStep(view.span, hz, widthPixels, minMajorPixels);
  const double ticksPerNs = hz / 1e9;
  const double minorNs = spec.majorStepNs / spec.minorDivisions;
  const double startNs = view.start / ticksPerNs, endNs = view.end() / ticksPerNs;
  const double pixelsPerMinor = widthPixels * (minorNs / std::max(endNs - startNs, 1e-9));
  const bool drawMinor = pixelsPerMinor >= 6.0;
  const double stepNs = drawMinor ? minorNs : spec.majorStepNs;
  const int perMajor = drawMinor ? spec.minorDivisions : 1;
  const double firstD = std::ceil(startNs / stepNs - 1e-9);
  const double lastD = std::floor(endNs / stepNs + 1e-9);
  // Converting an out-of-range or non-finite double to int64 is undefined: a view that far out (or NaN) has
  // no sensible ticks.
  constexpr double kMaxIndex = 1e15;
  if (!(std::fabs(firstD) < kMaxIndex) || !(std::fabs(lastD) < kMaxIndex)) return out;
  int64_t first = static_cast<int64_t>(firstD);
  const int64_t last = static_cast<int64_t>(lastD);
  constexpr int64_t kMaxTicks = 4096;
  if (last - first > kMaxTicks) first = last - kMaxTicks;
  for (int64_t i = first; i <= last; ++i) {
    RulerTick t;
    const double ns = static_cast<double>(i) * stepNs;
    t.relTick = ns * ticksPerNs;
    t.major = (i % perMajor) == 0;
    if (t.major) t.label = formatTimeNs(ns, spec.majorStepNs);
    out.push_back(std::move(t));
  }
  return out;
}

std::optional<TickRange> dragRange(const TimeView& view, const TimeLimits& limits, double x0, double x1,
                                   double areaX0, double areaWidth, double minPixels) {
  if (!(areaWidth > 0.0) || std::fabs(x1 - x0) < minPixels) return std::nullopt;
  if (x1 < x0) std::swap(x0, x1);
  TickRange r;
  r.t0 = toAbsolute(limits, xToRelTick(view, x0, areaX0, areaWidth));
  r.t1 = toAbsolute(limits, std::ceil(xToRelTick(view, x1, areaX0, areaWidth)));
  if (r.t1 <= r.t0) return std::nullopt;
  return r;
}

OverviewRect overviewViewport(const TimeView& view, const TimeLimits& limits, float areaX0, float areaWidth,
                              float minPixels) {
  const TimeView v = clampView(view, limits);
  const double total = limits.total();
  OverviewRect r;
  r.x0 = areaX0 + static_cast<float>(v.start / total * areaWidth);
  r.x1 = areaX0 + static_cast<float>(v.end() / total * areaWidth);
  if (r.x1 - r.x0 < minPixels) {
    const float mid = (r.x0 + r.x1) * 0.5f;
    r.x0 = mid - minPixels * 0.5f;
    r.x1 = mid + minPixels * 0.5f;
    if (r.x0 < areaX0) {
      r.x1 += areaX0 - r.x0;
      r.x0 = areaX0;
    }
    if (r.x1 > areaX0 + areaWidth) {
      r.x0 -= r.x1 - (areaX0 + areaWidth);
      r.x1 = areaX0 + areaWidth;
    }
  }
  return r;
}

TimeView overviewDragTo(const TimeView& view, const TimeLimits& limits, float x, float areaX0,
                        float areaWidth) {
  if (!(areaWidth > 0.0f)) return clampView(view, limits);
  const double rel = static_cast<double>((x - areaX0) / areaWidth) * limits.total();
  return centerOn(view, limits, rel);
}

TickRange overviewColumnRange(const TimeLimits& limits, uint32_t column, uint32_t columns) {
  TickRange r;
  if (columns == 0) return {limits.traceStart, limits.traceEnd};
  const double total = limits.total();
  r.t0 = toAbsolute(limits, total * column / columns);
  r.t1 = toAbsolute(limits, total * (column + 1) / columns);
  if (r.t1 <= r.t0) r.t1 = r.t0 + 1;
  return r;
}

}  // namespace tachyon::viewer::timeline
