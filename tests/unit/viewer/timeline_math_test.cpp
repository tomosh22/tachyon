// WP-42: pure geometry of the threads timeline (time<->pixel mapping, zoom/pan/fit, ruler, drag-select,
// overview strip).
#include "viewer/timeline/timeline_math.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <set>

using namespace tachyon::viewer::timeline;

namespace {

TimeLimits limits(uint64_t start = 1000, uint64_t length = 1'000'000) {
  TimeLimits l;
  l.traceStart = start;
  l.traceEnd = start + length;
  l.minSpan = 16.0;
  return l;
}

}  // namespace

TEST(TimelineMath, FitCoversTheWholeTrace) {
  const TimeLimits l = limits();
  const TimeView v = fitView(l);
  EXPECT_EQ(v.start, 0.0);
  EXPECT_EQ(v.span, 1'000'000.0);
  EXPECT_EQ(clampView(v, l), v);
}

TEST(TimelineMath, ClampKeepsTheViewInsideTheTrace) {
  const TimeLimits l = limits();
  EXPECT_EQ(clampView({-50, 100}, l).start, 0.0);
  EXPECT_EQ(clampView({999'990, 100}, l).start, 999'900.0);
  EXPECT_EQ(clampView({0, 5e9}, l).span, 1'000'000.0);
  EXPECT_EQ(clampView({0, 0.001}, l).span, 16.0);
  EXPECT_EQ(clampView({std::nan(""), std::nan("")}, l), fitView(l));
}

TEST(TimelineMath, DegenerateTraceDoesNotBreak) {
  TimeLimits l;
  l.traceStart = 500;
  l.traceEnd = 500;  // empty trace
  const TimeView v = fitView(l);
  EXPECT_GT(v.span, 0.0);
  const TickRange w = queryWindow(v, l);
  EXPECT_GT(w.t1, w.t0);
  EXPECT_EQ(zoomAtCursor(v, l, 0.5, 2.0), clampView(v, l));
}

TEST(TimelineMath, ZoomAtCursorKeepsTheTickUnderTheCursor) {
  const TimeLimits l = limits();
  std::mt19937 rng(7);
  for (int i = 0; i < 2000; ++i) {
    TimeView v{std::uniform_real_distribution<double>(0, 900'000)(rng),
               std::uniform_real_distribution<double>(100, 100'000)(rng)};
    v = clampView(v, l);
    const double frac = std::uniform_real_distribution<double>(0, 1)(rng);
    const double factor = std::exp(std::uniform_real_distribution<double>(-1.5, 1.5)(rng));
    const double anchor = v.start + frac * v.span;
    const TimeView z = zoomAtCursor(v, l, frac, factor);
    // The anchor stays under the cursor unless the clamp to the trace edge moved the view.
    const bool atEdge = z.start == 0.0 || z.end() >= l.total() - 1e-6;
    if (!atEdge) EXPECT_NEAR(z.start + frac * z.span, anchor, 1e-6 * v.span) << i;
    EXPECT_GE(z.span, l.minSpan);
    EXPECT_LE(z.span, l.total());
    EXPECT_GE(z.start, 0.0);
    EXPECT_LE(z.end(), l.total() + 1e-6);
  }
}

TEST(TimelineMath, ZoomInThenOutIsIdentityAwayFromTheEdges) {
  const TimeLimits l = limits();
  const TimeView v{400'000, 100'000};
  const TimeView z = zoomAtCursor(zoomAtCursor(v, l, 0.3, 4.0), l, 0.3, 0.25);
  EXPECT_NEAR(z.start, v.start, 1e-6);
  EXPECT_NEAR(z.span, v.span, 1e-6);
}

TEST(TimelineMath, ZoomStopsAtMinimumAndMaximumSpan) {
  const TimeLimits l = limits();
  TimeView v = fitView(l);
  for (int i = 0; i < 200; ++i) v = zoomAtCursor(v, l, 0.5, 2.0);
  EXPECT_EQ(v.span, l.minSpan);
  for (int i = 0; i < 200; ++i) v = zoomAtCursor(v, l, 0.5, 0.5);
  EXPECT_EQ(v.span, l.total());
  EXPECT_EQ(v.start, 0.0);
  EXPECT_EQ(zoomAtCursor(v, l, 0.5, 0.0), v);  // a non-positive factor is ignored
}

TEST(TimelineMath, PanClampsAndDragDirectionIsNatural) {
  const TimeLimits l = limits();
  const TimeView v{100'000, 200'000};
  EXPECT_EQ(panTicks(v, l, 5'000).start, 105'000.0);
  EXPECT_EQ(panTicks(v, l, -1e9).start, 0.0);
  EXPECT_EQ(panTicks(v, l, 1e9).start, 800'000.0);
  // Dragging right by 100 of 1000 px moves earlier time into view: start decreases by 10% of the span.
  EXPECT_NEAR(panPixels(v, l, 100, 1000).start, 100'000 - 20'000, 1e-9);
  EXPECT_NEAR(panPixels(v, l, -100, 1000).start, 100'000 + 20'000, 1e-9);
  EXPECT_EQ(panPixels(v, l, 100, 0), v);
}

TEST(TimelineMath, CenterOnAndViewFromRange) {
  const TimeLimits l = limits(1000, 1'000'000);
  const TimeView c = centerOn({0, 1000}, l, 500'000);
  EXPECT_NEAR(c.start + c.span / 2, 500'000, 1e-9);
  EXPECT_EQ(centerOn({0, 1000}, l, -5).start, 0.0);

  const TimeView r = viewFromRange(l, {1000 + 200'000, 1000 + 300'000}, 0.1);
  EXPECT_NEAR(r.start, 190'000, 1e-6);
  EXPECT_NEAR(r.span, 120'000, 1e-6);
  // Reversed ranges are accepted.
  EXPECT_EQ(viewFromRange(l, {1000 + 300'000, 1000 + 200'000}, 0.1), r);
}

TEST(TimelineMath, PixelMappingRoundTrips) {
  const TimeView v{12'345, 67'890};
  for (const double x : {100.0, 100.5, 350.0, 999.0, 1100.0}) {
    const double t = xToRelTick(v, x, 100.0, 1000.0);
    EXPECT_NEAR(relTickToX(v, t, 100.0, 1000.0), x, 1e-9);
  }
  EXPECT_EQ(xToRelTick(v, 100.0, 100.0, 1000.0), v.start);
  EXPECT_NEAR(xToRelTick(v, 1100.0, 100.0, 1000.0), v.end(), 1e-9);
  EXPECT_EQ(xToRelTick(v, 5.0, 0.0, 0.0), v.start);  // zero-width plot
}

TEST(TimelineMath, AbsoluteConversionsClampToTheTrace) {
  const TimeLimits l = limits(1000, 5000);
  EXPECT_EQ(toAbsolute(l, -10), 1000u);
  EXPECT_EQ(toAbsolute(l, 0.0), 1000u);
  EXPECT_EQ(toAbsolute(l, 123.9), 1123u);
  EXPECT_EQ(toAbsolute(l, 5000.0), 6000u);
  EXPECT_EQ(toAbsolute(l, 9e9), 6000u);
  EXPECT_EQ(toRelative(l, 500), 0.0);
  EXPECT_EQ(toRelative(l, 1500), 500.0);
  EXPECT_EQ(toRelative(l, 99999), 5000.0);
}

TEST(TimelineMath, QueryWindowIsAlwaysAValidLodQuery) {
  const TimeLimits l = limits(1'000'000, 10'000'000);
  std::mt19937 rng(3);
  for (int i = 0; i < 2000; ++i) {
    const TimeView v{std::uniform_real_distribution<double>(-1e6, 2e7)(rng),
                     std::exp(std::uniform_real_distribution<double>(-2, 17)(rng))};
    const TickRange w = queryWindow(v, l);
    EXPECT_GT(w.t1, w.t0);
    EXPECT_GE(w.t0, l.traceStart);
    EXPECT_LE(w.t1, l.traceEnd + 1);
    EXPECT_LE(static_cast<double>(w.t1 - w.t0), 140737488355328.0);
  }
  // The fitted view queries the whole trace.
  const TickRange all = queryWindow(fitView(l), l);
  EXPECT_EQ(all.t0, l.traceStart);
  EXPECT_EQ(all.t1, l.traceEnd);
}

// ---- ruler ----

TEST(TimelineRuler, StepIsAOneTwoFiveSeriesNeverBelowTheMinimumSpacing) {
  std::mt19937 rng(11);
  for (int i = 0; i < 3000; ++i) {
    const double hz = (i % 3 == 0) ? 1e9 : (i % 3 == 1) ? 1e7 : 3.5e6;
    const double spanTicks =
        std::exp(std::uniform_real_distribution<double>(std::log(16.0), std::log(1e12))(rng));
    const double width = std::uniform_real_distribution<double>(200, 3000)(rng);
    const double minPx = 90;
    const RulerSpec s = chooseRulerStep(spanTicks, hz, width, minPx);
    const double spanNs = spanTicks * 1e9 / hz;
    const double stepPx = s.majorStepNs / spanNs * width;
    EXPECT_GE(stepPx, minPx * (1 - 1e-9)) << spanTicks;
    EXPECT_LT(stepPx, minPx * 2.5 + 1e-6) << spanTicks;  // the next smaller 1-2-5 step would be too dense
    const double mantissa = s.majorStepNs / std::pow(10.0, std::floor(std::log10(s.majorStepNs) + 1e-9));
    EXPECT_TRUE(std::fabs(mantissa - 1) < 1e-6 || std::fabs(mantissa - 2) < 1e-6 ||
                std::fabs(mantissa - 5) < 1e-6)
        << s.majorStepNs;
  }
}

TEST(TimelineRuler, KnownSteps) {
  // 1 s over 1000 px with at least 100 px per tick: 100 ms.
  EXPECT_DOUBLE_EQ(chooseRulerStep(1e7, 1e7, 1000, 100).majorStepNs, 1e8);
  // 1 ms over 1000 px, 100 px: 100 us.
  EXPECT_DOUBLE_EQ(chooseRulerStep(1e4, 1e7, 1000, 100).majorStepNs, 1e5);
  // 250 ms over 1000 px, 100 px: min 25 ms -> 50 ms.
  EXPECT_DOUBLE_EQ(chooseRulerStep(2.5e6, 1e7, 1000, 100).majorStepNs, 5e7);
  // Degenerate input falls back to a valid step.
  EXPECT_GT(chooseRulerStep(0, 1e7, 1000, 100).majorStepNs, 0.0);
  EXPECT_GT(chooseRulerStep(100, 0, 0, 100).majorStepNs, 0.0);
}

TEST(TimelineRuler, TicksCoverTheViewAndLabelsAreDistinct) {
  std::mt19937 rng(5);
  for (int i = 0; i < 500; ++i) {
    const double hz = 1e7;
    const double span = std::exp(std::uniform_real_distribution<double>(std::log(50.0), std::log(1e9))(rng));
    const double start = std::uniform_real_distribution<double>(0, 1e9)(rng);
    const TimeView v{start, span};
    const auto ticks = buildRuler(v, hz, 1200, 90);
    ASSERT_FALSE(ticks.empty());
    std::set<std::string> labels;
    size_t majors = 0;
    for (const RulerTick& t : ticks) {
      EXPECT_GE(t.relTick, v.start - 1e-6 * v.span);
      EXPECT_LE(t.relTick, v.end() + 1e-6 * v.span);
      if (t.major) {
        ++majors;
        EXPECT_FALSE(t.label.empty());
        EXPECT_TRUE(labels.insert(t.label).second) << "duplicate label " << t.label << " span " << span;
      } else {
        EXPECT_TRUE(t.label.empty());
      }
    }
    EXPECT_GE(majors, 1u);
    EXPECT_LE(majors, 14u);  // 1200 px / 90 px
    for (size_t k = 1; k < ticks.size(); ++k) EXPECT_GT(ticks[k].relTick, ticks[k - 1].relTick);
  }
}

TEST(TimelineRuler, FormatsUnitsByMagnitude) {
  EXPECT_EQ(formatTimeNs(0, 1e8), "0 ns");
  EXPECT_EQ(formatTimeNs(800, 100), "800 ns");
  EXPECT_EQ(formatTimeNs(12'000, 1000), "12 us");
  EXPECT_EQ(formatTimeNs(12'500, 100), "12.5 us");
  EXPECT_EQ(formatTimeNs(350e6, 50e6), "350 ms");
  EXPECT_EQ(formatTimeNs(1.25e9, 0.05e9), "1.25 s");
  EXPECT_EQ(formatTimeNs(2e9, 1e9), "2 s");
  EXPECT_EQ(formatTimeNs(1.5e6, 0.5e6), "1.5 ms");
}

TEST(TimelineRuler, FormatsDurations) {
  EXPECT_EQ(formatDurationTicks(10'000'000, 1e7), "1.00 s");
  EXPECT_EQ(formatDurationTicks(12'345, 1e7), "1.23 ms");
  EXPECT_EQ(formatDurationTicks(5, 1e7), "500 ns");
  EXPECT_EQ(formatDurationTicks(0, 1e7), "0 ns");
}

TEST(TimelineRuler, MinorTicksOnlyWhenTheyAreFarEnoughApart) {
  const auto dense = buildRuler({0, 1e6}, 1e7, 1000, 90);
  size_t minors = 0;
  for (const auto& t : dense) minors += t.major ? 0 : 1;
  EXPECT_GT(minors, 0u);
  // 2 ms majors are 20 px apart here, so the four minors would be 5 px apart (< 6): none are produced.
  const auto sparse = buildRuler({0, 1e6}, 1e7, 1000, 11);
  EXPECT_FALSE(sparse.empty());
  for (const auto& t : sparse) {
    if (!t.major) {
      ADD_FAILURE() << "unexpected minor tick";
      break;
    }
  }
}

// ---- drag select ----

TEST(TimelineSelect, DragBecomesAnAbsoluteRange) {
  const TimeLimits l = limits(1000, 1'000'000);
  const TimeView v{100'000, 100'000};
  const auto r = dragRange(v, l, 300, 600, 100, 1000);  // plot starts at x = 100, 1000 px wide
  ASSERT_TRUE(r);
  EXPECT_EQ(r->t0, 1000u + 100'000u + 20'000u);
  EXPECT_EQ(r->t1, 1000u + 100'000u + 50'000u);
  // Drag direction does not matter.
  EXPECT_EQ(dragRange(v, l, 600, 300, 100, 1000), r);
}

TEST(TimelineSelect, ShortDragsAreClicks) {
  const TimeLimits l = limits();
  EXPECT_FALSE(dragRange(fitView(l), l, 500, 502, 100, 1000));
  EXPECT_TRUE(dragRange(fitView(l), l, 500, 504, 100, 1000));
  EXPECT_FALSE(dragRange(fitView(l), l, 500, 900, 100, 0));
}

TEST(TimelineSelect, RangeIsClampedToTheTrace) {
  const TimeLimits l = limits(1000, 1'000'000);
  const auto r = dragRange(fitView(l), l, -500, 5000, 100, 1000);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->t0, l.traceStart);
  EXPECT_EQ(r->t1, l.traceEnd);
  // Entirely outside the trace: empty after clamping, so no range.
  EXPECT_FALSE(dragRange(fitView(l), l, -900, -500, 100, 1000));
}

// ---- overview ----

TEST(TimelineOverview, ViewportIndicatorMatchesTheView) {
  const TimeLimits l = limits(0, 1'000'000);
  const OverviewRect r = overviewViewport({250'000, 500'000}, l, 50, 1000);
  EXPECT_NEAR(r.x0, 300.0f, 1e-3);
  EXPECT_NEAR(r.x1, 800.0f, 1e-3);
  const OverviewRect whole = overviewViewport(fitView(l), l, 50, 1000);
  EXPECT_NEAR(whole.x0, 50.0f, 1e-3);
  EXPECT_NEAR(whole.x1, 1050.0f, 1e-3);
}

TEST(TimelineOverview, IndicatorHasAMinimumWidthAndStaysInside) {
  const TimeLimits l = limits(0, 1'000'000);
  for (const double start : {0.0, 500'000.0, 999'984.0}) {
    const OverviewRect r = overviewViewport({start, 16}, l, 50, 1000, 4.0f);
    EXPECT_NEAR(r.x1 - r.x0, 4.0f, 1e-3);
    EXPECT_GE(r.x0, 50.0f);
    EXPECT_LE(r.x1, 1050.0f);
  }
}

TEST(TimelineOverview, DraggingCentersTheViewOnTheCursor) {
  const TimeLimits l = limits(0, 1'000'000);
  const TimeView v = overviewDragTo({0, 100'000}, l, 550, 50, 1000);
  EXPECT_NEAR(v.start + v.span / 2, 500'000, 1e-6);
  EXPECT_EQ(v.span, 100'000.0);
  EXPECT_EQ(overviewDragTo({0, 100'000}, l, 10, 50, 1000).start, 0.0);          // clamped left
  EXPECT_EQ(overviewDragTo({0, 100'000}, l, 5000, 50, 1000).start, 900'000.0);  // clamped right
  EXPECT_EQ(overviewDragTo({10, 100'000}, l, 5000, 50, 0).span, 100'000.0);     // no width
}

TEST(TimelineOverview, ColumnRangesTileTheTrace) {
  const TimeLimits l = limits(777, 1'000'003);
  uint64_t expected = l.traceStart;
  for (uint32_t c = 0; c < 333; ++c) {
    const TickRange r = overviewColumnRange(l, c, 333);
    EXPECT_EQ(r.t0, expected);
    EXPECT_GT(r.t1, r.t0);
    expected = r.t1;
  }
  EXPECT_EQ(expected, l.traceEnd);
}

TEST(TimelineRuler, ExtremeAndNonFiniteViewsDoNotOverflowTheTickIndex) {
  // start / step beyond int64: previously an undefined double -> int64 conversion.
  for (const double start : {1e300, 1e30, 9.3e18, -1e30, std::nan(""), HUGE_VAL}) {
    for (const double span : {1.0, 1e9, 1e300, std::nan("")}) {
      const auto ticks = buildRuler({start, span}, 1e7, 1000, 90);
      EXPECT_LE(ticks.size(), 4097u);
    }
  }
  // Ordinary views are unaffected by the guard.
  EXPECT_FALSE(buildRuler({1e9, 1e6}, 1e7, 1000, 90).empty());
  EXPECT_TRUE(buildRuler({1e300, 1.0}, 1e7, 1000, 90).empty());
}
