// WP-47: the pure logic of the Function Statistics panel: search index, formatting, histogram and box plot
// geometry, instance stepping and navigation windows, the per-thread instance index with its overlay rects,
// and the bus' instance highlight.
#include "viewer/stats/stats_model.h"
#include "analysis/query.h"
#include "trace/trace_format.h"
#include "viewer/selection_bus.h"
#include "viewer/stats/instance_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::stats;
using analysis::Instance;

namespace {

analysis::Stats statsOf(std::vector<uint64_t> d) {
  analysis::Stats s;
  if (d.empty()) return s;
  std::sort(d.begin(), d.end());
  s.count = static_cast<uint32_t>(d.size());
  s.min = static_cast<double>(d.front());
  s.max = static_cast<double>(d.back());
  s.p25 = static_cast<double>(d[static_cast<size_t>(0.25 * static_cast<double>(d.size() - 1))]);
  s.median = static_cast<double>(d[static_cast<size_t>(0.5 * static_cast<double>(d.size() - 1))]);
  s.p75 = static_cast<double>(d[static_cast<size_t>(0.75 * static_cast<double>(d.size() - 1))]);
  uint64_t total = 0;
  for (uint64_t v : d) {
    total += v;
    const uint32_t b = analysis::statsBucketOf(v);
    if (s.histogram.size() <= b) s.histogram.resize(b + 1, 0);
    ++s.histogram[b];
  }
  s.totalTicks = total;
  s.mean = static_cast<double>(total) / static_cast<double>(d.size());
  return s;
}

common::NameParts namer(const symbols::FunctionKey& k) {
  common::NameParts p;
  p.module = "game.exe";
  p.function = "fn" + std::to_string(k.value);
  p.full = p.module + "!" + p.function;
  return p;
}

}  // namespace

// ---- search index
// ------------------------------------------------------------------------------------------

TEST(StatsModel, CollectsScopeIdsWithBeginCounts) {
  model::TraceModel m;
  const StringIdx a = m.strings.intern("Frame");
  const StringIdx b = m.strings.intern("Update");
  const auto ev = [&](uint8_t kind, StringIdx id) {
    trace::ScopeEventRec e{};
    e.kind = kind;
    e.id = id;
    e.tid = 100;
    m.scopeEvents.push_back(e);
  };
  ev(trace::kScopeBegin, b);
  ev(trace::kScopeBegin, a);
  ev(trace::kScopeEnd, StringIdx());
  ev(trace::kScopeBegin, b);
  ev(trace::kScopeEnd, StringIdx());
  ev(trace::kScopeEnd, StringIdx());
  const auto ids = collectScopeIds(m);
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0].first, b);  // first seen
  EXPECT_EQ(ids[0].second, 2u);
  EXPECT_EQ(ids[1].first, a);
  EXPECT_EQ(ids[1].second, 1u);
}

TEST(StatsModel, SearchIndexHoldsFunctionsThenScopesAndFindsByKey) {
  model::TraceModel m;
  const StringIdx frame = m.strings.intern("Frame");
  const StringIdx apple = m.strings.intern("Apple");
  std::vector<analysis::FunctionRow> rows(3);
  for (uint32_t i = 0; i < 3; ++i) {
    rows[i].key = symbols::FunctionKey{ModuleIdx(0), 0x1000u + i * 0x100};
    rows[i].instances = 10 * (i + 1);
  }
  const std::vector<std::pair<StringIdx, uint32_t>> scopes = {{frame, 7}, {apple, 3}};
  const auto index = buildSearchIndex(rows, namer, scopes, m);
  ASSERT_EQ(index->entries.size(), 5u);
  EXPECT_EQ(index->entries[0].kind, EntryKind::Function);
  EXPECT_EQ(index->entries[0].name, "game.exe!fn4096");
  EXPECT_EQ(index->entries[0].module, "game.exe");
  EXPECT_EQ(index->entries[2].count, 30u);
  EXPECT_EQ(index->entries[3].kind, EntryKind::Scope);  // scopes sorted by name: Apple, Frame
  EXPECT_EQ(index->entries[3].name, "Apple");
  EXPECT_EQ(index->entries[3].count, 3u);
  EXPECT_EQ(index->entries[3].module, "");
  EXPECT_EQ(index->entries[4].name, "Frame");
  EXPECT_EQ(index->entries[4].key, symbols::FunctionKey::scope(frame));
  EXPECT_EQ(index->fuzzy.size(), 5u);
  ASSERT_NE(index->find(symbols::FunctionKey::scope(apple)), nullptr);
  EXPECT_EQ(index->find(symbols::FunctionKey::scope(apple))->name, "Apple");
  EXPECT_EQ(index->find(symbols::FunctionKey{ModuleIdx(9), 1}), nullptr);

  // Searching finds both kinds, best first, top 200 at most.
  const auto hits = searchEntries(*index, "fra");
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(index->entries[hits[0].entry].name, "Frame");
  EXPECT_EQ(hits[0].tier, MatchTier::Prefix);
  const auto all = searchEntries(*index, "");
  EXPECT_EQ(all.size(), 5u);
}

TEST(StatsModel, SearchReturnsAtMostTwoHundredResults) {
  model::TraceModel m;
  std::vector<analysis::FunctionRow> rows(1000);
  for (uint32_t i = 0; i < rows.size(); ++i) rows[i].key = symbols::FunctionKey{ModuleIdx(0), i};
  const auto index = buildSearchIndex(rows, namer, {}, m);
  EXPECT_EQ(searchEntries(*index, "fn").size(), kMaxSearchResults);
  EXPECT_EQ(searchEntries(*index, "").size(), kMaxSearchResults);
  EXPECT_EQ(kMaxSearchResults, 200u);
}

TEST(StatsModel, StepSelectionClamps) {
  EXPECT_EQ(stepSelection(-1, 1, 5), 0);
  EXPECT_EQ(stepSelection(-1, -1, 5), 4);
  EXPECT_EQ(stepSelection(0, -1, 5), 0);
  EXPECT_EQ(stepSelection(4, 1, 5), 4);
  EXPECT_EQ(stepSelection(2, 1, 5), 3);
  EXPECT_EQ(stepSelection(2, 10, 5), 4);
  EXPECT_EQ(stepSelection(2, 1, 0), -1);
}

// ---- formatting -------------------------------------------------------------------------------------------

TEST(StatsModel, FormatsDurationsInTheUnitOfTheirMagnitude) {
  EXPECT_EQ(formatDuration(1.5e7, 1e7), "1.500 s");  // 10 MHz clock: 1.5e7 ticks = 1.5 s
  EXPECT_EQ(formatDuration(12340.0, 1e7), "1.234 ms");
  EXPECT_EQ(formatDuration(50.0, 1e7), "5.00 \xC2\xB5s");
  EXPECT_EQ(formatDuration(5.0, 1e7), "500 ns");
  EXPECT_EQ(formatDuration(120.0, 1e9), "120 ns");
  EXPECT_EQ(formatDuration(0.0, 1e9), "0 ns");
  EXPECT_EQ(formatDuration(2'000'000.0, 1e9), "2.000 ms");
  EXPECT_EQ(formatDuration(1000.0, 0.0), "1.00 \xC2\xB5s");  // zero clock counts as 1 GHz
}

// ---- histogram --------------------------------------------------------------------------------------------

TEST(StatsModel, HistogramBarsCoverTheFirstToTheLastNonEmptyBucket) {
  // 3 -> bucket 2, 5,6 -> bucket 3, 100 -> bucket 7
  const analysis::Stats s = statsOf({3, 5, 6, 100});
  ASSERT_EQ(s.histogram.size(), 8u);
  const PlotRect area{10, 20, 110, 120};
  const HistogramLayout h = layoutHistogram(s, area);
  EXPECT_EQ(h.firstBucket, 2u);
  EXPECT_EQ(h.lastBucket, 7u);
  EXPECT_EQ(h.maxCount, 2u);
  ASSERT_EQ(h.bars.size(), 6u);
  for (size_t i = 0; i < h.bars.size(); ++i) {
    const HistogramBar& b = h.bars[i];
    EXPECT_EQ(b.bucket, 2u + i);
    EXPECT_EQ(b.count, s.histogram[2 + i]);
    EXPECT_FLOAT_EQ(b.y1, area.y1);
    EXPECT_GE(b.x0, area.x0);
    EXPECT_LE(b.x1, area.x1);
    EXPECT_GT(b.x1, b.x0);
    if (i > 0) EXPECT_GE(b.x0, h.bars[i - 1].x1);
  }
  // The tallest bar fills the height; a single-count bar is half of it; empty buckets have no height.
  EXPECT_FLOAT_EQ(h.bars[1].y0, area.y0);  // bucket 3: count 2
  EXPECT_FLOAT_EQ(h.bars[0].y1 - h.bars[0].y0, area.height() / 2);
  EXPECT_FLOAT_EQ(h.bars[2].y1 - h.bars[2].y0, 0.0f);  // bucket 4: empty
  EXPECT_FLOAT_EQ(h.bars[5].y1 - h.bars[5].y0, area.height() / 2);
  // Bars are in bucket order and equally wide.
  EXPECT_NEAR(h.bars[0].x1 - h.bars[0].x0, h.bars[5].x1 - h.bars[5].x0, 1e-4);
}

TEST(StatsModel, HistogramMinimumHeightAndEmptyStats) {
  std::vector<uint64_t> d(10000, 4);
  d.push_back(1000);  // one instance among 10000: proportionally 0.01 px high, drawn at least 1 px
  const analysis::Stats s = statsOf(d);
  const HistogramLayout h = layoutHistogram(s, {0, 0, 400, 100});
  ASSERT_FALSE(h.bars.empty());
  EXPECT_GE(h.bars.back().y1 - h.bars.back().y0, 1.0f);
  EXPECT_TRUE(layoutHistogram(analysis::Stats{}, {0, 0, 100, 100}).bars.empty());
  // One bucket only: one bar over the whole width.
  const HistogramLayout one = layoutHistogram(statsOf({8, 9, 10}), {0, 0, 90, 50});
  ASSERT_EQ(one.bars.size(), 1u);
  EXPECT_NEAR(one.bars[0].x1 - one.bars[0].x0, 89.0f, 1.01f);
}

TEST(StatsModel, BucketLowerEdgesFollowTheLog2Scheme) {
  EXPECT_EQ(bucketLowerTicks(0), 0.0);
  EXPECT_EQ(bucketLowerTicks(1), 1.0);
  EXPECT_EQ(bucketLowerTicks(2), 2.0);
  EXPECT_EQ(bucketLowerTicks(10), 512.0);
  EXPECT_EQ(bucketLowerTicks(64), std::ldexp(1.0, 63));
}

// ---- box plot ---------------------------------------------------------------------------------------------

TEST(StatsModel, BoxPlotPositionsAreLinearBetweenMinAndMax) {
  analysis::Stats s;
  s.count = 5;
  s.min = 10;
  s.p25 = 20;
  s.median = 30;
  s.p75 = 60;
  s.max = 110;
  s.mean = 40;
  const PlotRect area{100, 0, 300, 50};
  const BoxPlotLayout b = layoutBoxPlot(s, area);
  ASSERT_TRUE(b.valid);
  EXPECT_FLOAT_EQ(b.xMin, 100.0f);
  EXPECT_FLOAT_EQ(b.xMax, 300.0f);
  EXPECT_FLOAT_EQ(b.xQ1, 120.0f);
  EXPECT_FLOAT_EQ(b.xMedian, 140.0f);
  EXPECT_FLOAT_EQ(b.xQ3, 200.0f);
  EXPECT_FLOAT_EQ(b.xMean, 160.0f);
  EXPECT_LE(b.xMin, b.xQ1);
  EXPECT_LE(b.xQ1, b.xMedian);
  EXPECT_LE(b.xMedian, b.xQ3);
  EXPECT_LE(b.xQ3, b.xMax);
  EXPECT_LT(b.yTop, b.yMid);
  EXPECT_LT(b.yMid, b.yBottom);
  EXPECT_GE(b.yTop, area.y0);
  EXPECT_LE(b.yBottom, area.y1);
}

TEST(StatsModel, BoxPlotOfEqualDurationsIsCentredAndEmptyIsInvalid) {
  const analysis::Stats s = statsOf({7, 7, 7});
  const BoxPlotLayout b = layoutBoxPlot(s, {0, 0, 100, 40});
  ASSERT_TRUE(b.valid);
  for (float x : {b.xMin, b.xQ1, b.xMedian, b.xQ3, b.xMax, b.xMean}) EXPECT_FLOAT_EQ(x, 50.0f);
  EXPECT_FALSE(layoutBoxPlot(analysis::Stats{}, {0, 0, 100, 40}).valid);
  // The mean is clamped into the plot even for odd inputs.
  analysis::Stats odd = statsOf({1, 2, 3});
  odd.mean = 1e9;
  EXPECT_FLOAT_EQ(layoutBoxPlot(odd, {0, 0, 100, 40}).xMean, 100.0f);
}

// ---- instances --------------------------------------------------------------------------------------------

TEST(StatsModel, StepInstanceWrapsAndStartsAtTheEnds) {
  EXPECT_EQ(stepInstance(0, std::nullopt, true), std::nullopt);
  EXPECT_EQ(stepInstance(5, std::nullopt, true), 0u);
  EXPECT_EQ(stepInstance(5, std::nullopt, false), 4u);
  EXPECT_EQ(stepInstance(5, 0u, true), 1u);
  EXPECT_EQ(stepInstance(5, 4u, true), 0u);   // wraps
  EXPECT_EQ(stepInstance(5, 0u, false), 4u);  // wraps
  EXPECT_EQ(stepInstance(5, 3u, false), 2u);
  EXPECT_EQ(stepInstance(1, 0u, true), 0u);
  EXPECT_EQ(stepInstance(3, 99u, true), 0u);  // a stale index restarts
}

TEST(StatsModel, NavigationWindowIsCentredAndNeverEmpty) {
  const Instance in{ThreadIdx(1), 10000, 10600};  // duration 600, interval 10
  TickWindow w = navigationWindow(in, 10);
  EXPECT_EQ(w.t1 - w.t0, 1800u);  // three times the duration
  EXPECT_EQ((w.t0 + w.t1) / 2, 10300u);
  // A very short instance gets 16 intervals of context (at least 1000 ticks).
  w = navigationWindow(Instance{ThreadIdx(0), 50000, 50001}, 100);
  EXPECT_EQ(w.t1 - w.t0, 1600u);
  w = navigationWindow(Instance{ThreadIdx(0), 50000, 50001}, 1);
  EXPECT_EQ(w.t1 - w.t0, 1000u);
  // Clamped at tick 0, still non-empty.
  w = navigationWindow(Instance{ThreadIdx(0), 5, 6}, 100);
  EXPECT_EQ(w.t0, 0u);
  EXPECT_GT(w.t1, w.t0);
  EXPECT_GE(w.t1, 6u);
}

// ---- instance index and overlay rects
// -----------------------------------------------------------------------

namespace {

timeline::TimeLimits limits(uint64_t start, uint64_t end) {
  timeline::TimeLimits l;
  l.traceStart = start;
  l.traceEnd = end;
  return l;
}

std::shared_ptr<const std::vector<Instance>> list(std::vector<Instance> v) {
  std::sort(v.begin(), v.end(), [](const Instance& a, const Instance& b) {
    return a.begin != b.begin ? a.begin < b.begin : a.thread < b.thread;
  });
  return std::make_shared<const std::vector<Instance>>(std::move(v));
}

}  // namespace

TEST(InstanceIndexTest, SplitsPerThreadAndCountsOverlaps) {
  const auto l = list({{ThreadIdx(0), 100, 200},
                       {ThreadIdx(1), 150, 400},
                       {ThreadIdx(0), 300, 350},
                       {ThreadIdx(2), 500, 600},
                       {ThreadIdx(7), 10, 20}});
  const InstanceIndex idx(l, 3);
  EXPECT_EQ(idx.threadCount(), 3u);
  EXPECT_EQ(idx.countOn(ThreadIdx(0)), 2u);
  EXPECT_EQ(idx.countOn(ThreadIdx(1)), 1u);
  EXPECT_EQ(idx.countOn(ThreadIdx(2)), 1u);
  EXPECT_EQ(idx.countOn(ThreadIdx(7)), 0u);  // beyond the thread count: ignored
  EXPECT_EQ(idx.countOverlapping(ThreadIdx(0), 190, 310), 2u);
  EXPECT_EQ(idx.countOverlapping(ThreadIdx(0), 201, 299), 0u);
  EXPECT_EQ(InstanceIndex().instances().size(), 0u);
}

TEST(InstanceIndexTest, RectsMatchTheVisibleInstancesOfTheThread) {
  // Trace [1000, 11000), 1000 px wide plot at x = 100: 0.1 px per tick.
  const auto lim = limits(1000, 11000);
  const timeline::TimeView full{0.0, 10000.0};
  const auto l = list({{ThreadIdx(0), 1000, 1500},
                       {ThreadIdx(0), 3000, 3400},
                       {ThreadIdx(1), 3100, 3300},
                       {ThreadIdx(0), 9000, 10500}});
  const InstanceIndex idx(l, 2);
  std::vector<OverlayRect> r;
  EXPECT_TRUE(idx.rectsFor(ThreadIdx(0), full, lim, 100.0f, 1000.0f, 100, r));
  ASSERT_EQ(r.size(), 3u);
  EXPECT_FLOAT_EQ(r[0].x0, 100.0f);
  EXPECT_FLOAT_EQ(r[0].x1, 150.0f);
  EXPECT_FLOAT_EQ(r[1].x0, 300.0f);
  EXPECT_FLOAT_EQ(r[1].x1, 340.0f);
  EXPECT_FLOAT_EQ(r[2].x0, 900.0f);
  EXPECT_FLOAT_EQ(r[2].x1, 1050.0f);
  EXPECT_EQ((*l)[r[1].instance].begin, 3000u);

  // Zoomed to [2000, 4000) of the trace: relative start 1000, span 2000 -> 0.5 px per tick.
  const timeline::TimeView zoom{1000.0, 2000.0};
  r.clear();
  EXPECT_TRUE(idx.rectsFor(ThreadIdx(0), zoom, lim, 100.0f, 1000.0f, 100, r));
  ASSERT_EQ(r.size(), 1u);  // only the instance at 3000..3400 overlaps
  EXPECT_FLOAT_EQ(r[0].x0, 100.0f + (3000.0f - 2000.0f) * 0.5f);
  EXPECT_FLOAT_EQ(r[0].x1, 100.0f + (3400.0f - 2000.0f) * 0.5f);
  r.clear();
  idx.rectsFor(ThreadIdx(1), zoom, lim, 100.0f, 1000.0f, 100, r);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ((*l)[r[0].instance].thread, ThreadIdx(1));

  // An instance that begins before the view and ends inside it is clipped to the plot's left edge.
  const timeline::TimeView late{4000.0, 1000.0};  // ticks [5000, 6000) absolute... nothing there
  r.clear();
  idx.rectsFor(ThreadIdx(0), late, lim, 100.0f, 1000.0f, 100, r);
  EXPECT_TRUE(r.empty());
  const timeline::TimeView edge{8200.0, 1000.0};  // absolute [9200, 10200): the long instance starts earlier
  r.clear();
  idx.rectsFor(ThreadIdx(0), edge, lim, 100.0f, 1000.0f, 100, r);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_FLOAT_EQ(r[0].x0, 100.0f);  // clipped
  // Unknown or out-of-range threads produce nothing.
  r.clear();
  EXPECT_TRUE(idx.rectsFor(ThreadIdx(5), full, lim, 100.0f, 1000.0f, 100, r));
  EXPECT_TRUE(r.empty());
}

TEST(InstanceIndexTest, NestedAndTinyInstancesAreBoundedAndMerge) {
  const auto lim = limits(0, 1000000);
  const timeline::TimeView full{0.0, 1000000.0};  // 1000 px: 1000 ticks per pixel
  std::vector<Instance> v;
  // 125 clusters of 200 one-tick instances (10 ticks apart: 2 px wide) every 8 px: sub-pixel instances.
  for (uint64_t c = 0; c < 125; ++c)
    for (uint64_t i = 0; i < 200; ++i) v.push_back({ThreadIdx(0), c * 8000 + i * 10, c * 8000 + i * 10 + 1});
  // An outer instance with an inner one at the same pixel.
  v.push_back({ThreadIdx(1), 1000, 100000});
  v.push_back({ThreadIdx(1), 2000, 50000});
  const auto l = list(v);
  const InstanceIndex idx(l, 2);
  std::vector<OverlayRect> r;
  EXPECT_TRUE(idx.rectsFor(ThreadIdx(0), full, lim, 0.0f, 1000.0f, 100000, r));
  EXPECT_LE(r.size(), 125u);  // one run per cluster at most, never more than the pixels
  EXPECT_GT(r.size(), 100u);
  uint32_t merged = 0;
  for (const OverlayRect& o : r) {
    EXPECT_GE(o.x1 - o.x0, kMinRectWidth);
    merged += o.count;
  }
  EXPECT_GT(merged, r.size());  // runs stand for several instances
  EXPECT_EQ(merged, 25000u);    // every instance is accounted for by some rect
  // Neighbouring sub-pixel instances merge into one run.
  std::vector<Instance> dense;
  for (uint64_t i = 0; i < 100000; ++i) dense.push_back({ThreadIdx(0), i * 10, i * 10 + 1});
  const InstanceIndex denseIdx(list(dense), 1);
  r.clear();
  EXPECT_TRUE(denseIdx.rectsFor(ThreadIdx(0), full, lim, 0.0f, 1000.0f, 100000, r));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].count, 100000u);
  EXPECT_GT(r[0].x1, 990.0f);
  r.clear();
  EXPECT_TRUE(idx.rectsFor(ThreadIdx(1), full, lim, 0.0f, 1000.0f, 100, r));
  EXPECT_EQ(r.size(), 1u);  // the inner instance lies inside the outer one
  // The cap cuts the list and says so.
  r.clear();
  EXPECT_FALSE(idx.rectsFor(ThreadIdx(0), full, lim, 0.0f, 1000.0f, 10, r));
  EXPECT_EQ(r.size(), 10u);
}

TEST(InstanceIndexTest, OverlappingInstancesOfOneThreadAreFoundFromTheRunningMaximum) {
  // A long instance that began long before the window must still be found; the ones after it are in order.
  const auto lim = limits(0, 100000);
  const auto l = list({{ThreadIdx(0), 100, 90000}, {ThreadIdx(0), 200, 300}, {ThreadIdx(0), 50000, 50100}});
  const InstanceIndex idx(l, 1);
  const timeline::TimeView view{40000.0, 20000.0};  // [40000, 60000)
  std::vector<OverlayRect> r;
  idx.rectsFor(ThreadIdx(0), view, lim, 0.0f, 2000.0f, 100, r);
  ASSERT_EQ(r.size(), 1u);  // the long one; the short one at 50000 lies inside its rect
  EXPECT_EQ((*l)[r[0].instance].begin, 100u);
  EXPECT_EQ(idx.countOverlapping(ThreadIdx(0), 40000, 60000), 2u);
}

// ---- the bus
// ------------------------------------------------------------------------------------------------

TEST(SelectionBusHighlight, PublishesGenerationsAndTheCurrentInstance) {
  SelectionBus bus;
  std::vector<SelectionField> fields;
  bus.subscribe([&](const SelectionState&, SelectionField f) { fields.push_back(f); });
  EXPECT_FALSE(bus.state().instanceHighlight.active());
  EXPECT_EQ(bus.setInstanceHighlight(std::nullopt, nullptr), 0u);  // already clear: nothing published
  EXPECT_TRUE(fields.empty());

  const auto l = list({{ThreadIdx(0), 1, 2}, {ThreadIdx(0), 5, 9}});
  const symbols::FunctionKey key = symbols::FunctionKey::scope(StringIdx(3));
  const uint64_t g1 = bus.setInstanceHighlight(key, l);
  EXPECT_GT(g1, 0u);
  ASSERT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0], SelectionField::InstanceHighlight);
  EXPECT_TRUE(bus.state().instanceHighlight.active());
  EXPECT_EQ(bus.state().instanceHighlight.key, std::optional(key));
  EXPECT_EQ(bus.state().instanceHighlight.instances, l);
  EXPECT_FALSE(bus.state().instanceHighlight.current.has_value());

  bus.setHighlightCurrent(1u);
  EXPECT_EQ(fields.size(), 2u);
  EXPECT_EQ(bus.state().instanceHighlight.generation, g1);  // unchanged by the current instance
  EXPECT_EQ(bus.state().instanceHighlight.current, std::optional<uint32_t>(1u));
  bus.setHighlightCurrent(1u);  // no change: no event
  EXPECT_EQ(fields.size(), 2u);

  const uint64_t g2 = bus.setInstanceHighlight(key, l);  // an event even for the same list
  EXPECT_GT(g2, g1);
  EXPECT_FALSE(bus.state().instanceHighlight.current.has_value());  // reset with the new list
  EXPECT_EQ(fields.size(), 3u);

  bus.setInstanceHighlight(std::nullopt, nullptr);
  EXPECT_FALSE(bus.state().instanceHighlight.active());
  EXPECT_EQ(fields.size(), 4u);
  bus.setHighlightCurrent(0u);  // nothing highlighted: ignored
  EXPECT_EQ(fields.size(), 4u);

  bus.setInstanceHighlight(key, l);
  bus.clear();
  EXPECT_FALSE(bus.state().instanceHighlight.active());
}
