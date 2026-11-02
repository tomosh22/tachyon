// WP-54: pure logic of the frame-time graph and the plot / marker lanes (no ImGui): per-pixel bar merging,
// budget highlighting, the slowest-frame jump target, the plot min / max index against a brute force (spikes
// are never lost), column queries, marker thinning and the layout of the lower section.
#include "model/model.h"
#include "viewer/timeline/frame_graph.h"
#include "viewer/timeline/plot_lanes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

using namespace tachyon;
using namespace tachyon::viewer::timeline;

namespace {

PlotArea makeArea(double span, float width = 1000.0f, uint64_t traceStart = 1000, double viewStart = 0.0) {
  PlotArea a;
  a.x0 = 100.0f;
  a.width = width;
  a.limits.traceStart = traceStart;
  a.limits.traceEnd = traceStart + 4000000000ULL;
  a.view.start = viewStart;
  a.view.span = span;
  a.clockHz = 1e9;
  return a;
}

// Contiguous frames with the given durations, starting at `t0`.
FrameBars makeBars(const std::vector<uint64_t>& durations, uint64_t t0 = 1000) {
  FrameBars bars;
  bars.series.thread = ThreadIdx(2);
  bars.series.name = StringIdx(3);
  uint64_t t = t0;
  uint64_t n = 0;
  for (const uint64_t d : durations) {
    bars.start.push_back(t);
    bars.end.push_back(t + d);
    bars.number.push_back(n++);
    bars.maxDuration = std::max(bars.maxDuration, d);
    t += d;
  }
  return bars;
}

}  // namespace

// ---- frame bars
// ----------------------------------------------------------------------------------------------

TEST(InstFrames, WideFramesAreOneBarEachWithTheirOwnHeight) {
  const FrameBars bars = makeBars({100000, 200000, 100000, 600000});  // 1 px = 1000 ticks on a 1000 px area
  std::vector<FrameBarItem> items;
  layoutFrameBars(bars, makeArea(1000000.0), /*budget*/ 150000, items);
  ASSERT_EQ(items.size(), 4u);
  EXPECT_FLOAT_EQ(items[0].x0, 100.0f);
  EXPECT_FLOAT_EQ(items[1].x0, 200.0f);
  EXPECT_FLOAT_EQ(items[1].x1, 400.0f);
  EXPECT_EQ(items[3].duration, 600000u);
  EXPECT_FALSE(items[0].overBudget);
  EXPECT_TRUE(items[1].overBudget);
  EXPECT_FALSE(items[2].overBudget);
  EXPECT_TRUE(items[3].overBudget);
}

TEST(InstFrames, SubPixelFramesMergePerPixelAndShowTheTallest) {
  std::vector<uint64_t> d(10000, 10);
  d[4321] = 90;  // a hitch among 10000 frames of 10 ticks
  const FrameBars bars = makeBars(d);
  std::vector<FrameBarItem> items;
  layoutFrameBars(bars, makeArea(static_cast<double>(bars.end.back() - 1000)), 50,
                  items);          // ~100 ticks per pixel
  EXPECT_LE(items.size(), 1010u);  // a pixel per bar, plus the edges
  uint64_t frames = 0;
  uint32_t overBudget = 0;
  for (const FrameBarItem& it : items) {
    frames += it.count;
    if (it.overBudget) {
      ++overBudget;
      EXPECT_EQ(it.tallest, 4321u);
      EXPECT_EQ(it.duration, 90u);
      EXPECT_GE(it.first, 4300u);
      EXPECT_LE(it.first, 4321u);
    }
  }
  EXPECT_EQ(frames, 10000u);
  EXPECT_EQ(overBudget, 1u);  // the single hitch is not lost in the merge
}

TEST(InstFrames, ABigTraceDrawsAtMostOneBarPerPixelPlusWideFrames) {
  std::mt19937_64 rng(3);
  std::vector<uint64_t> d(100000);
  for (uint64_t& v : d) v = 15000 + rng() % 3000;
  const FrameBars bars = makeBars(d);
  const double total = static_cast<double>(bars.end.back() - 1000);
  std::vector<FrameBarItem> items;
  layoutFrameBars(bars, makeArea(total, 1900.0f), 16670, items);
  EXPECT_LE(items.size(), 1900u + 2u);
  uint64_t frames = 0;
  for (const FrameBarItem& it : items) frames += it.count;
  EXPECT_EQ(frames, 100000u);
  // Zoomed in to a few frames, every frame is its own bar again.
  layoutFrameBars(bars, makeArea(60000.0, 1900.0f, 1000, 500000.0), 16670, items);
  EXPECT_LE(items.size(), 6u);
  for (const FrameBarItem& it : items) EXPECT_EQ(it.count, 1u);
}

TEST(InstFrames, BudgetHighlightingFollowsTheBudgetAndAZeroBudgetHighlightsNothing) {
  const FrameBars bars = makeBars({16000, 16670, 16671, 40000});
  std::vector<FrameBarItem> items;
  layoutFrameBars(bars, makeArea(80000.0), 16670, items);
  ASSERT_EQ(items.size(), 4u);
  EXPECT_FALSE(items[0].overBudget);
  EXPECT_FALSE(items[1].overBudget);  // equal to the budget is within it
  EXPECT_TRUE(items[2].overBudget);
  EXPECT_TRUE(items[3].overBudget);
  layoutFrameBars(bars, makeArea(80000.0), 0, items);
  for (const FrameBarItem& it : items) EXPECT_FALSE(it.overBudget);
}

TEST(InstFrames, BudgetsHaveADefaultAndPerSeriesOverrides) {
  FrameBudgets b(16.67);
  analysis::FrameSeries main_{ThreadIdx(1), StringIdx(5), 10}, render{ThreadIdx(2), StringIdx(5), 10};
  EXPECT_DOUBLE_EQ(b.msFor(main_), 16.67);
  b.set(render, 33.33);
  EXPECT_DOUBLE_EQ(b.msFor(render), 33.33);
  EXPECT_DOUBLE_EQ(b.msFor(main_), 16.67);
  EXPECT_TRUE(b.overridden(render));
  EXPECT_FALSE(b.overridden(main_));
  b.set(render, 0.0);  // removes the override
  EXPECT_DOUBLE_EQ(b.msFor(render), 16.67);
  EXPECT_EQ(msToTicks(16.67, 1e7), 166700u);
  EXPECT_EQ(msToTicks(-1.0, 1e7), 0u);
  EXPECT_DOUBLE_EQ(FrameBudgets(0.0).defaultMs(), kDefaultFrameBudgetMs);
}

TEST(InstFrames, AxisAlwaysHoldsTheBudgetLine) {
  EXPECT_EQ(yAxisMax(100, 1000), 1250u);   // short frames: the budget line is at 80%
  EXPECT_EQ(yAxisMax(5000, 1000), 5000u);  // a long frame sets the scale
  EXPECT_EQ(yAxisMax(0, 0), 1u);
}

TEST(InstFrames, BarAtFindsTheBarUnderTheCursor) {
  const FrameBars bars = makeBars({100000, 200000, 100000});
  std::vector<FrameBarItem> items;
  layoutFrameBars(bars, makeArea(400000.0), 1, items);
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(barAt(items, 150.0f), std::optional<size_t>(0));
  EXPECT_EQ(barAt(items, 350.0f), std::optional<size_t>(1));
  EXPECT_EQ(barAt(items, 999.0f), std::optional<size_t>(2));
  EXPECT_FALSE(barAt(items, 50.0f));
  EXPECT_FALSE(barAt(items, 1200.0f));
}

TEST(InstFrames, JumpTargetsAreTheFrameExtentOnTheMarkingThread) {
  const FrameBars bars = makeBars({100, 300, 200});
  const FrameJump j = jumpTargetOf(bars, 1);
  EXPECT_EQ(j.thread, ThreadIdx(2));
  EXPECT_EQ(j.t0, 1100u);
  EXPECT_EQ(j.t1, 1400u);
  EXPECT_EQ(jumpTargetOf(bars, 9).t1, 0u);
  analysis::FrameSummary s;
  s.markThread = ThreadIdx(7);
  s.start = 50;
  s.end = 90;
  const FrameJump k = jumpTargetOf(s);
  EXPECT_EQ(k.thread, ThreadIdx(7));
  EXPECT_EQ(k.t0, 50u);
  EXPECT_EQ(k.t1, 90u);
}

TEST(InstFrames, TooltipSaysHowFarOverBudget) {
  const FrameBars bars = makeBars({10000000, 30000000});
  FrameBarItem item;
  item.tallest = 1;
  item.duration = 30000000;
  item.overBudget = true;
  const auto lines = frameBarTooltipLines(bars, item, 1e9, 16670000, "Frame");
  ASSERT_GE(lines.size(), 2u);
  EXPECT_NE(lines[0].find("Frame frame 1"), std::string::npos);
  EXPECT_NE(lines[1].find("over budget"), std::string::npos);
}

// ---- plots
// ---------------------------------------------------------------------------------------------------

namespace {

std::shared_ptr<model::TraceModel> plotModel(size_t samples, uint64_t seed, bool unsorted = false) {
  auto m = std::make_shared<model::TraceModel>();
  m->header.start_ts = 1000;
  m->header.end_ts = 1000 + 100000000;
  model::PlotSeries p;
  p.name = m->strings.intern("fps");
  std::mt19937_64 rng(seed);
  for (size_t i = 0; i < samples; ++i) {
    p.ts.push_back(1000 + i * 100);
    double v = static_cast<double>(rng() % 1000) / 10.0;
    if (i % 977 == 5) v = std::numeric_limits<double>::quiet_NaN();
    if (i % 1234 == 7) v = std::numeric_limits<double>::infinity();
    p.value.push_back(v);
  }
  if (unsorted) {
    std::reverse(p.ts.begin(), p.ts.end());
    std::reverse(p.value.begin(), p.value.end());
  }
  m->plots.push_back(std::move(p));
  return m;
}

bool bruteMinMax(const PlotSeriesIndex& s, size_t a, size_t b, double& mn, double& mx) {
  mn = std::numeric_limits<double>::infinity();
  mx = -mn;
  for (size_t i = a; i < b; ++i) {
    if (!std::isfinite(s.value[i])) continue;
    mn = std::min(mn, s.value[i]);
    mx = std::max(mx, s.value[i]);
  }
  return mn <= mx;
}

}  // namespace

TEST(InstPlots, RangeMinMaxEqualsABruteForceOnRandomRanges) {
  const auto model = plotModel(200000, 1);
  const auto idx = buildPlotIndex(model, nullptr);
  ASSERT_TRUE(idx);
  const PlotSeriesIndex& s = idx->series[0];
  std::mt19937_64 rng(9);
  for (int trial = 0; trial < 3000; ++trial) {
    size_t a = rng() % s.n, b = rng() % s.n;
    if (trial % 3 == 0) b = std::min(s.n, a + rng() % 100);  // short ranges
    if (a > b) std::swap(a, b);
    double mn = 0, mx = 0, bmn = 0, bmx = 0;
    const bool found = plotRangeMinMax(s, a, b, mn, mx);
    const bool bfound = bruteMinMax(s, a, b, bmn, bmx);
    ASSERT_EQ(found, bfound) << a << ".." << b;
    if (found) {
      ASSERT_DOUBLE_EQ(mn, bmn) << a << ".." << b;
      ASSERT_DOUBLE_EQ(mx, bmx) << a << ".." << b;
    }
  }
  EXPECT_EQ(idx->sampleCount, 200000u);
  double mn = 0, mx = 0;
  ASSERT_TRUE(bruteMinMax(s, 0, s.n, mn, mx));
  EXPECT_DOUBLE_EQ(s.minValue, mn);
  EXPECT_DOUBLE_EQ(s.maxValue, mx);
}

TEST(InstPlots, AnUnsortedSeriesIsIndexedOnASortedCopy) {
  const auto model = plotModel(5000, 2, /*unsorted=*/true);
  const auto idx = buildPlotIndex(model, nullptr);
  const PlotSeriesIndex& s = idx->series[0];
  EXPECT_FALSE(s.ownedTs.empty());
  EXPECT_TRUE(std::is_sorted(s.ts, s.ts + s.n));
  EXPECT_EQ(s.ts[0], 1000u);
  // A sorted one points at the model's own columns.
  const auto sorted = plotModel(5000, 2);
  const auto idx2 = buildPlotIndex(sorted, nullptr);
  EXPECT_EQ(idx2->series[0].ts, sorted->plots[0].ts.data());
  EXPECT_TRUE(idx2->series[0].ownedTs.empty());
}

TEST(InstPlots, ColumnsKeepASingleSampleSpike) {
  auto model = std::make_shared<model::TraceModel>();
  model->header.start_ts = 1000;
  model::PlotSeries p;
  p.name = model->strings.intern("load");
  for (size_t i = 0; i < 1000000; ++i) {
    p.ts.push_back(1000 + i * 10);
    p.value.push_back(i == 654321 ? 900.0 : 50.0 + static_cast<double>(i % 7));
  }
  model->plots.push_back(std::move(p));
  const auto idx = buildPlotIndex(model, nullptr);
  const PlotArea area = makeArea(1e7, 1000.0f);  // 10000 samples per pixel
  PlotWindow w;
  queryPlotColumns(idx->series[0], area, 1.0f, w);
  EXPECT_LE(w.columns.size(), 1000u);
  EXPECT_EQ(w.samples, 1000000u);
  double highest = 0;
  for (const PlotColumn& c : w.columns) highest = std::max(highest, c.max);
  EXPECT_DOUBLE_EQ(highest, 900.0);
  EXPECT_DOUBLE_EQ(w.maxValue, 900.0);
  EXPECT_DOUBLE_EQ(w.minValue, 50.0);
  uint64_t counted = 0;
  for (const PlotColumn& c : w.columns) counted += c.count;
  EXPECT_EQ(counted, 1000000u);
  for (size_t i = 1; i < w.columns.size(); ++i) EXPECT_LT(w.columns[i - 1].x, w.columns[i].x);
}

TEST(InstPlots, ZoomedInColumnsAreSinglesAndNeighboursJoinTheLine) {
  const auto model = plotModel(1000, 4);
  const auto idx = buildPlotIndex(model, nullptr);
  const PlotSeriesIndex& s = idx->series[0];
  // Samples every 100 ticks; show ticks 50000..51000 on 1000 px: a sample per 100 px.
  const PlotArea area = makeArea(1000.0, 1000.0f, 1000, 49000.0);
  PlotWindow w;
  queryPlotColumns(s, area, 1.0f, w);
  EXPECT_LE(w.columns.size(), 11u);
  for (const PlotColumn& c : w.columns) {
    EXPECT_EQ(c.count, 1u);
    EXPECT_DOUBLE_EQ(c.first, c.last);
  }
  EXPECT_TRUE(w.hasBefore);
  EXPECT_TRUE(w.hasAfter);
  EXPECT_LT(w.beforeX, area.x0);
  EXPECT_GE(w.afterX, area.x0 + area.width);
  // The nearest sample under the cursor.
  EXPECT_EQ(nearestSample(s, 1000 + 4952), 50u);
  EXPECT_EQ(nearestSample(s, 1000 + 4949), 49u);
  EXPECT_EQ(nearestSample(s, 0), 0u);
  EXPECT_EQ(nearestSample(s, 1ULL << 40), s.n - 1);
}

TEST(InstPlots, WindowWithoutSamplesReportsNone) {
  const auto model = plotModel(100, 4);
  const auto idx = buildPlotIndex(model, nullptr);
  PlotWindow w;
  queryPlotColumns(idx->series[0], makeArea(100.0, 500.0f, 1000, 3000000.0), 1.0f, w);
  EXPECT_TRUE(w.columns.empty());
  EXPECT_TRUE(w.any);  // the neighbouring sample still gives the axis a range
  EXPECT_TRUE(w.hasBefore);
  EXPECT_FALSE(w.hasAfter);
}

// ---- markers
// -------------------------------------------------------------------------------------------------

TEST(InstMarkers, OneTickPerPixelWithTheCountItStandsFor) {
  std::vector<model::Marker> markers;
  for (uint64_t i = 0; i < 1000; ++i) {
    model::Marker m;
    m.ts = 1000 + i * 10;
    markers.push_back(m);
  }
  const MarkerView view{&markers, nullptr};
  // 10000 ticks on 100 px: ten markers per pixel.
  std::vector<MarkerItem> items;
  ASSERT_TRUE(layoutMarkers(view, makeArea(10000.0, 100.0f), 1000, items));
  EXPECT_EQ(items.size(), 100u);
  uint64_t total = 0;
  for (const MarkerItem& it : items) {
    EXPECT_EQ(it.count, 10u);
    total += it.count;
  }
  EXPECT_EQ(total, 1000u);
  // Zoomed in: a marker per 5 px.
  ASSERT_TRUE(layoutMarkers(view, makeArea(200.0, 100.0f, 1000, 0.0), 1000, items));
  EXPECT_EQ(items.size(), 20u);
  for (const MarkerItem& it : items) EXPECT_EQ(it.count, 1u);
  // The cap.
  EXPECT_FALSE(layoutMarkers(view, makeArea(10000.0, 100.0f), 10, items));
  EXPECT_EQ(items.size(), 10u);
}

TEST(InstMarkers, UnsortedMarkersAreReadThroughTheIndexOrder) {
  auto model = std::make_shared<model::TraceModel>();
  model->header.start_ts = 1000;
  for (const uint64_t ts : {5000, 2000, 9000, 3000}) {
    model::Marker m;
    m.ts = ts;
    m.name = model->strings.intern("m" + std::to_string(ts));
    model->markers.push_back(m);
  }
  const auto idx = buildPlotIndex(model, nullptr);
  ASSERT_EQ(idx->markerOrder.size(), 4u);
  const MarkerView view{&model->markers, &idx->markerOrder};
  EXPECT_EQ(view.at(0).ts, 2000u);
  EXPECT_EQ(view.at(3).ts, 9000u);
  std::vector<MarkerItem> items;
  ASSERT_TRUE(layoutMarkers(view, makeArea(10000.0, 1000.0f), 100, items));
  ASSERT_EQ(items.size(), 4u);
  EXPECT_LT(items[0].x, items[1].x);
}

TEST(InstMarkers, HitsFindTheNearestTickWithinTheTolerance) {
  std::vector<MarkerItem> items(3);
  items[0].x = 100.5f;
  items[1].x = 120.5f;
  items[2].x = 300.5f;
  EXPECT_EQ(markerItemAt(items, 121.0f, 4.0f), std::optional<size_t>(1));
  EXPECT_EQ(markerItemAt(items, 110.0f, 4.0f), std::nullopt);
  EXPECT_EQ(markerItemAt(items, 303.0f, 4.0f), std::optional<size_t>(2));
  EXPECT_EQ(markerItemAt(items, 90.0f, 4.0f), std::nullopt);
}

TEST(InstMarkers, TooltipHasNameDataAndThread) {
  model::TraceModel model;
  model.header.start_ts = 1000;
  model::Thread th;
  th.tid = 501;
  th.name = "Game";
  model.threads.push_back(th);
  model::Marker m;
  m.ts = 3000;
  m.thread = ThreadIdx(0);
  m.name = model.strings.intern("Hitch");
  m.data = model.strings.intern("frame 59 takes 50 ms");
  model.markers.push_back(m);
  const MarkerView view{&model.markers, nullptr};
  MarkerItem item;
  const auto lines = markerTooltipLines(model, view, item, 1e9);
  ASSERT_GE(lines.size(), 4u);
  EXPECT_EQ(lines[0], "Marker: Hitch");
  EXPECT_EQ(lines[1], "data: frame 59 takes 50 ms");
  EXPECT_NE(lines.back().find("Game"), std::string::npos);
}

// ---- section layout
// ------------------------------------------------------------------------------------------

TEST(InstSection, LanesStackMarkersThenPlotGroupThenOneLanePerPlot) {
  const GlobalGeometry g;
  const GlobalLayout all = layoutGlobalLanes(true, 3, false, g);
  ASSERT_EQ(all.lanes.size(), 5u);  // markers, header, 3 plots
  EXPECT_EQ(all.lanes[0].kind, GlobalLane::Kind::Markers);
  EXPECT_EQ(all.lanes[1].kind, GlobalLane::Kind::PlotsHeader);
  EXPECT_EQ(all.lanes[2].kind, GlobalLane::Kind::Plot);
  EXPECT_EQ(all.lanes[4].index, 2u);
  for (size_t i = 1; i < all.lanes.size(); ++i) {
    EXPECT_GE(all.lanes[i].y0, all.lanes[i - 1].y0 + all.lanes[i - 1].height);  // no overlap
  }
  EXPECT_GT(all.height, all.lanes.back().y0 + all.lanes.back().height - 0.001f);

  const GlobalLayout collapsed = layoutGlobalLanes(true, 3, true, g);
  EXPECT_EQ(collapsed.lanes.size(), 2u);  // markers + the header; the lanes are folded away
  EXPECT_LT(collapsed.height, all.height);
  EXPECT_TRUE(layoutGlobalLanes(false, 0, false, g).lanes.empty());
  EXPECT_EQ(layoutGlobalLanes(false, 0, false, g).height, 0.0f);
  const GlobalLayout plotsOnly = layoutGlobalLanes(false, 1, false, g);
  EXPECT_EQ(plotsOnly.lanes.size(), 2u);

  ASSERT_NE(globalLaneAt(all, all.lanes[3].y0 + 1.0f), nullptr);
  EXPECT_EQ(globalLaneAt(all, all.lanes[3].y0 + 1.0f)->index, 1u);
  EXPECT_EQ(globalLaneAt(all, -1.0f), nullptr);
  EXPECT_EQ(globalLaneAt(all, all.height + 10.0f), nullptr);
}
