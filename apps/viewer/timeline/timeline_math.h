// Pure geometry of the threads timeline (WP-42): time <-> pixel mapping, zoom/pan/fit, the adaptive time
// ruler, drag-select and the overview strip. No ImGui and no model access, so everything here is unit-tested.
//
// Conventions: a view is expressed in ticks relative to the trace start (doubles, so sub-tick zooming and
// fractional pan are exact enough for any realistic trace length); absolute uint64_t ticks appear only at the
// edges (queries, selection bus).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer::timeline {

struct TimeLimits {
  uint64_t traceStart = 0;
  uint64_t traceEnd = 1;
  // Smallest visible span in ticks (stops the zoom before a query would degenerate).
  double minSpan = 16.0;

  // Thread-safe (value type).
  double total() const { return static_cast<double>(traceEnd > traceStart ? traceEnd - traceStart : 1); }
};

// The visible window [start, start + span) in ticks relative to TimeLimits::traceStart.
struct TimeView {
  double start = 0.0;
  double span = 1.0;

  double end() const { return start + span; }
  bool operator==(const TimeView&) const = default;
};

struct TickRange {
  uint64_t t0 = 0, t1 = 0;  // absolute ticks, [t0, t1)
  bool operator==(const TickRange&) const = default;
};

// All functions in this header: thread-safe, pure.

// The whole trace.
TimeView fitView(const TimeLimits& limits);
// Span clamped to [min(minSpan, total), total], start clamped so the view stays inside the trace.
TimeView clampView(TimeView view, const TimeLimits& limits);
// Zooms by `factor` (> 1 zooms in) keeping the tick under `cursorFrac` (0 = left edge .. 1 = right edge of
// the plot area) fixed on screen.
TimeView zoomAtCursor(TimeView view, const TimeLimits& limits, double cursorFrac, double factor);
// Moves the view by `deltaTicks` (positive = later in time), clamped.
TimeView panTicks(TimeView view, const TimeLimits& limits, double deltaTicks);
// Drag by `dxPixels` in a plot area `widthPixels` wide: dragging right moves the content right (earlier time
// comes into view).
TimeView panPixels(TimeView view, const TimeLimits& limits, double dxPixels, double widthPixels);
// Keeps the span, centres the view on the relative tick.
TimeView centerOn(TimeView view, const TimeLimits& limits, double relTick);
// Shows an absolute range with a small margin (fraction of the range on each side).
TimeView viewFromRange(const TimeLimits& limits, TickRange range, double margin = 0.05);

// Relative tick under pixel x of a plot area starting at `areaX0` and `areaWidth` wide.
double xToRelTick(const TimeView& view, double x, double areaX0, double areaWidth);
double relTickToX(const TimeView& view, double relTick, double areaX0, double areaWidth);
uint64_t toAbsolute(const TimeLimits& limits, double relTick);  // clamped to the trace, rounded down
double toRelative(const TimeLimits& limits, uint64_t absTick);

// Integer query window for TimelineLod: t1 > t0, within the trace, span <= 2^47.
TickRange queryWindow(const TimeView& view, const TimeLimits& limits);

// ---- ruler ----
struct RulerTick {
  double relTick = 0.0;  // relative to the trace start
  bool major = false;
  std::string label;  // only for major ticks
};
struct RulerSpec {
  double majorStepNs = 1.0;
  int minorDivisions = 5;  // minor ticks per major step
};
// Smallest 1-2-5 step of at least `minMajorPixels` per major tick for a view `widthPixels` wide.
RulerSpec chooseRulerStep(double spanTicks, double clockHz, double widthPixels, double minMajorPixels);
// Ticks with relTick inside the view; minor ticks are only produced when they are at least 6 px apart.
std::vector<RulerTick> buildRuler(const TimeView& view, double clockHz, double widthPixels,
                                  double minMajorPixels = 90.0);
// "1.25 s", "350 ms", "12 us", "800 ns": unit by magnitude, decimals by `stepNs` so neighbouring labels
// differ.
std::string formatTimeNs(double ns, double stepNs);
// Same for a duration in ticks (unit and precision picked from the value itself, three significant digits).
std::string formatDurationTicks(uint64_t ticks, double clockHz);

// ---- drag select ----
// Converts a drag from pixel x0 to x1 into an absolute range clamped to the trace. Drags shorter than
// `minPixels` are clicks and return nullopt.
std::optional<TickRange> dragRange(const TimeView& view, const TimeLimits& limits, double x0, double x1,
                                   double areaX0, double areaWidth, double minPixels = 3.0);

// ---- overview strip ----
struct OverviewRect {
  float x0 = 0, x1 = 0;
};
// Pixel extent of the viewport indicator on an overview strip covering the whole trace (at least minPixels
// wide).
OverviewRect overviewViewport(const TimeView& view, const TimeLimits& limits, float areaX0, float areaWidth,
                              float minPixels = 4.0f);
// View obtained by dragging the indicator so that its centre is at pixel x.
TimeView overviewDragTo(const TimeView& view, const TimeLimits& limits, float x, float areaX0,
                        float areaWidth);
// Absolute range covered by overview column c of `columns` (for building the overview contents).
TickRange overviewColumnRange(const TimeLimits& limits, uint32_t column, uint32_t columns);

}  // namespace tachyon::viewer::timeline
