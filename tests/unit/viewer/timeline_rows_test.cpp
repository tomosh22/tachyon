// WP-42: row sorting / layout / hit-testing of the threads timeline.
#include "viewer/timeline/timeline_rows.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <random>
#include <set>

using namespace tachyon;
using namespace tachyon::viewer::timeline;

namespace {

std::vector<ThreadSortInfo> infos() {
  std::vector<ThreadSortInfo> v(5);
  const char* names[] = {"main", "worker1", "render", "worker0", "audio"};
  const uint64_t running[] = {500, 900, 100, 900, 0};
  const uint64_t samples[] = {10, 20, 30, 5, 5};
  const uint32_t tids[] = {40, 30, 20, 10, 50};
  for (uint32_t i = 0; i < 5; ++i) {
    v[i].thread = ThreadIdx(i);
    v[i].name = names[i];
    v[i].runningTicks = running[i];
    v[i].sampleCount = samples[i];
    v[i].tid = tids[i];
  }
  return v;
}

std::vector<uint32_t> ids(const std::vector<ThreadIdx>& v) {
  std::vector<uint32_t> out;
  for (const ThreadIdx t : v) out.push_back(t.get());
  return out;
}

}  // namespace

TEST(TimelineRows, SortsByRunningTimeMostFirstWithIndexTieBreak) {
  EXPECT_EQ(ids(orderRows(infos(), {}, SortKey::RunningTime, false)), (std::vector<uint32_t>{1, 3, 0, 2, 4}));
  EXPECT_EQ(ids(orderRows(infos(), {}, SortKey::RunningTime, true)),
            (std::vector<uint32_t>{4, 2, 0, 1, 3}));  // ties stay in index order
}

TEST(TimelineRows, OtherSortKeys) {
  EXPECT_EQ(ids(orderRows(infos(), {}, SortKey::SampleCount, false)), (std::vector<uint32_t>{2, 1, 0, 3, 4}));
  EXPECT_EQ(ids(orderRows(infos(), {}, SortKey::Name, false)), (std::vector<uint32_t>{4, 0, 2, 3, 1}));
  EXPECT_EQ(ids(orderRows(infos(), {}, SortKey::ThreadId, false)), (std::vector<uint32_t>{3, 2, 1, 0, 4}));
  EXPECT_EQ(ids(orderRows(infos(), {}, SortKey::Natural, false)), (std::vector<uint32_t>{0, 1, 2, 3, 4}));
  EXPECT_EQ(ids(orderRows(infos(), {}, SortKey::Natural, true)), (std::vector<uint32_t>{0, 1, 2, 3, 4}));
}

TEST(TimelineRows, PinnedFirstHiddenOmitted) {
  std::vector<RowFlags> flags(5);
  flags[2].pinned = true;
  flags[4].pinned = true;
  flags[1].hidden = true;
  EXPECT_EQ(ids(orderRows(infos(), flags, SortKey::RunningTime, false)), (std::vector<uint32_t>{2, 4, 3, 0}));
  flags[2].hidden = true;  // hidden wins over pinned
  EXPECT_EQ(ids(orderRows(infos(), flags, SortKey::RunningTime, false)), (std::vector<uint32_t>{4, 3, 0}));
  // Short flag vectors are padded with defaults.
  EXPECT_EQ(orderRows(infos(), std::vector<RowFlags>(1), SortKey::Natural, false).size(), 5u);
}

TEST(TimelineRows, SortIsAPermutationForRandomInput) {
  std::mt19937 rng(9);
  std::vector<ThreadSortInfo> v(64);
  std::vector<std::string> names(64);
  for (uint32_t i = 0; i < 64; ++i) {
    names[i] = "t" + std::to_string(rng() % 20);
    v[i].thread = ThreadIdx(i);
    v[i].name = names[i];
    v[i].runningTicks = rng() % 10;
    v[i].tid = rng() % 100;
  }
  std::vector<RowFlags> flags(64);
  for (auto& f : flags) f.pinned = rng() % 5 == 0;
  for (const SortKey k : {SortKey::RunningTime, SortKey::SampleCount, SortKey::Name, SortKey::ThreadId}) {
    const auto order = orderRows(v, flags, k, false);
    ASSERT_EQ(order.size(), 64u);
    const std::vector<uint32_t> orderIds = ids(order);
    std::set<uint32_t> seen(orderIds.begin(), orderIds.end());
    EXPECT_EQ(seen.size(), 64u);
    bool inPinned = true;
    for (const ThreadIdx t : order) {
      if (!flags[t.get()].pinned) inPinned = false;
      EXPECT_EQ(flags[t.get()].pinned, inPinned) << "pinned rows must form a prefix";
    }
  }
}

TEST(TimelineRows, LayoutStacksRowsAndExpandsWithFlameDepths) {
  RowGeometry g;
  std::vector<RowFlags> flags(3);
  flags[1].expanded = true;
  const std::vector<uint32_t> depths = {4, 6, 2};
  const auto layout = layoutRows({ThreadIdx(2), ThreadIdx(1), ThreadIdx(0)}, flags, depths, g);
  ASSERT_EQ(layout.size(), 3u);
  const float collapsed = g.rowPadding * 2 + g.bandHeight + g.stripHeight;
  EXPECT_EQ(layout[0].thread.get(), 2u);
  EXPECT_EQ(layout[0].y0, 0.0f);
  EXPECT_EQ(layout[0].height, collapsed);
  EXPECT_FALSE(layout[0].expanded);
  EXPECT_TRUE(layout[1].expanded);
  EXPECT_EQ(layout[1].y0, collapsed + g.rowSpacing);
  EXPECT_FLOAT_EQ(layout[1].height, collapsed + g.flameGap + 6 * g.flameDepthHeight);
  EXPECT_FLOAT_EQ(layout[2].y0, layout[1].y0 + layout[1].height + g.rowSpacing);
  EXPECT_FLOAT_EQ(contentHeight(layout, g), layout[2].y0 + layout[2].height + g.rowSpacing);
  EXPECT_EQ(contentHeight({}, g), 0.0f);
}

TEST(TimelineRows, VisibleRowsAndRowAt) {
  RowGeometry g;
  std::vector<RowFlags> flags(10);
  std::vector<ThreadIdx> order;
  for (uint32_t i = 0; i < 10; ++i) order.push_back(ThreadIdx(i));
  const auto layout = layoutRows(order, flags, std::vector<uint32_t>(10, 3), g);
  const float pitch = layout[1].y0 - layout[0].y0;

  RowRange all = visibleRows(layout, 0, 1e6f);
  EXPECT_EQ(all.first, 0u);
  EXPECT_EQ(all.last, 10u);
  RowRange mid = visibleRows(layout, pitch * 3 + 1, pitch * 2);
  EXPECT_EQ(mid.first, 3u);
  EXPECT_EQ(mid.last, 6u);
  RowRange beyond = visibleRows(layout, 1e6f, 100);
  EXPECT_EQ(beyond.first, beyond.last);
  EXPECT_EQ(visibleRows({}, 0, 100).last, 0u);

  EXPECT_EQ(rowAt(layout, g, 0.5f), 0u);
  EXPECT_EQ(rowAt(layout, g, pitch * 4 + 3), 4u);
  EXPECT_EQ(rowAt(layout, g, -1.0f), std::numeric_limits<size_t>::max());
  EXPECT_EQ(rowAt(layout, g, pitch * 10 + 50), std::numeric_limits<size_t>::max());
}

TEST(TimelineRows, HitRowSection) {
  RowGeometry g;
  RowLayout collapsed;
  collapsed.height = g.rowPadding * 2 + g.bandHeight + g.stripHeight;
  EXPECT_EQ(hitRowSection(collapsed, g, bandTop(g) + 1).section, RowSection::Band);
  EXPECT_EQ(hitRowSection(collapsed, g, 0.5f).section, RowSection::Band);
  EXPECT_EQ(hitRowSection(collapsed, g, stripTop(g) + 1).section, RowSection::Strip);
  EXPECT_EQ(hitRowSection(collapsed, g, collapsed.height - 0.5f).section, RowSection::Strip);
  EXPECT_EQ(hitRowSection(collapsed, g, collapsed.height + 1).section, RowSection::None);
  EXPECT_EQ(hitRowSection(collapsed, g, -1).section, RowSection::None);

  RowLayout expanded = collapsed;
  expanded.expanded = true;
  expanded.flameDepths = 5;
  expanded.height += g.flameGap + 5 * g.flameDepthHeight;
  EXPECT_EQ(hitRowSection(expanded, g, flameTop(g) + 1).section, RowSection::Flame);
  EXPECT_EQ(hitRowSection(expanded, g, flameTop(g) + 1).depth, 1u);
  EXPECT_EQ(hitRowSection(expanded, g, flameDepthTop(g, 3) + 1).depth, 3u);
  EXPECT_EQ(hitRowSection(expanded, g, flameDepthTop(g, 5) + g.flameDepthHeight - 0.5f).depth, 5u);
  EXPECT_EQ(hitRowSection(expanded, g, stripTop(g) + g.stripHeight + 0.5f).section,
            RowSection::Strip);  // gap
  EXPECT_FLOAT_EQ(flameDepthTop(g, 2), flameTop(g) + g.flameDepthHeight);
}

TEST(TimelineRows, FindSpanAt) {
  using analysis::TimelineSpan;
  const auto mk = [](uint32_t depth, uint64_t t0, uint64_t t1) {
    TimelineSpan s;
    s.depth = depth;
    s.t0 = t0;
    s.t1 = t1;
    s.node = CallstackIdx(depth * 100 + static_cast<uint32_t>(t0));
    return s;
  };
  const std::vector<TimelineSpan> spans = {mk(1, 0, 100), mk(1, 100, 150), mk(1, 200, 300),
                                           mk(2, 10, 20), mk(2, 50, 60),   mk(4, 0, 1000)};
  const auto at = [&](uint32_t d, uint64_t ts) { return findSpanAt(spans, d, ts); };
  ASSERT_NE(at(1, 0), nullptr);
  EXPECT_EQ(at(1, 0)->t1, 100u);
  EXPECT_EQ(at(1, 99)->t0, 0u);
  EXPECT_EQ(at(1, 100)->t0, 100u);
  EXPECT_EQ(at(1, 149)->t0, 100u);
  EXPECT_EQ(at(1, 150), nullptr);  // gap
  EXPECT_EQ(at(1, 199), nullptr);
  EXPECT_EQ(at(1, 250)->t0, 200u);
  EXPECT_EQ(at(1, 300), nullptr);
  EXPECT_EQ(at(2, 15)->t0, 10u);
  EXPECT_EQ(at(2, 25), nullptr);
  EXPECT_EQ(at(3, 5), nullptr);  // no spans at that depth
  EXPECT_EQ(at(4, 999)->t0, 0u);
  EXPECT_EQ(at(5, 0), nullptr);
  EXPECT_EQ(findSpanAt({}, 1, 0), nullptr);
}

TEST(TimelineRows, ColumnForTickInvertsPixelEdge) {
  std::mt19937_64 rng(17);
  for (int i = 0; i < 3000; ++i) {
    const uint64_t t0 = rng() % 1'000'000;
    const uint64_t span = 1 + rng() % 5'000'000;
    const uint32_t cols = 1 + static_cast<uint32_t>(rng() % 2000);
    const uint64_t ts = t0 + rng() % span;
    const uint32_t c = columnForTick(t0, t0 + span, cols, ts);
    ASSERT_LT(c, cols);
    EXPECT_LE(analysis::pixelEdge(t0, t0 + span, cols, c), ts);
    if (c + 1 < cols) {
      EXPECT_GT(analysis::pixelEdge(t0, t0 + span, cols, c + 1), ts);
    }
  }
  EXPECT_EQ(columnForTick(100, 200, 10, 50), 0u);
  EXPECT_EQ(columnForTick(100, 200, 10, 500), 9u);
  EXPECT_EQ(columnForTick(100, 200, 0, 150), 0u);
}
