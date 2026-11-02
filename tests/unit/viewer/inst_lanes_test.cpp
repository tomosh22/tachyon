// WP-54: pure logic of the instrumentation scope lanes (no ImGui): item layout with per-pixel merging, hit
// testing, depth packing into lines, fiber lanes built from the real `fibers` fixture, colours, labels and
// tooltips.
#include "analysis/fibers.h"
#include "analysis/scope_pairing.h"
#include "model/model.h"
#include "viewer/timeline/scope_lanes.h"
#include "viewer/timeline/timeline_rows.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <set>

using namespace tachyon;
using namespace tachyon::viewer::timeline;
namespace fs = std::filesystem;

namespace {

PlotArea makeArea(double span, float width = 1000.0f, uint64_t traceStart = 1000, double viewStart = 0.0) {
  PlotArea a;
  a.x0 = 100.0f;
  a.width = width;
  a.limits.traceStart = traceStart;
  a.limits.traceEnd = traceStart + 1000000000ULL;
  a.view.start = viewStart;
  a.view.span = span;
  a.clockHz = 1e9;
  return a;
}

// Owns the instances a track points at.
struct TrackBuilder {
  std::vector<analysis::ScopeInstance> store;
  ScopeTrack track;
  void add(uint64_t begin, uint64_t end, uint32_t depth = 0, uint32_t color = 0xFFFFFFFFu, uint32_t id = 1) {
    analysis::ScopeInstance s;
    s.id = StringIdx(id);
    s.begin = begin;
    s.end = end;
    s.depth = depth;
    s.color = color;
    store.push_back(s);
  }
  const ScopeTrack& build() {
    track = {};
    for (const auto& s : store) {
      track.begin.push_back(s.begin);
      track.end.push_back(s.end);
      track.instance.push_back(&s);
    }
    return track;
  }
};

uint64_t totalCount(const std::vector<LaneItem>& items) {
  uint64_t n = 0;
  for (const LaneItem& i : items) n += i.count;
  return n;
}

}  // namespace

TEST(InstLanes, WideIntervalsAreTheirOwnItemsAtTheRightPixels) {
  TrackBuilder tb;
  for (uint64_t i = 0; i < 10; ++i) tb.add(1000 + i * 100000, 1000 + i * 100000 + 50000);
  const PlotArea area = makeArea(1000000.0);  // 1000 px for 1e6 ticks: 1 px = 1000 ticks
  std::vector<LaneItem> items;
  ASSERT_TRUE(layoutTrack(tb.build(), area, 100, items));
  ASSERT_EQ(items.size(), 10u);
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(items[i].count, 1u);
    EXPECT_EQ(items[i].first, i);
    EXPECT_NEAR(items[i].x0, 100.0f + 100.0f * static_cast<float>(i), 0.01f);
    EXPECT_NEAR(items[i].x1 - items[i].x0, 50.0f, 0.01f);
  }
}

TEST(InstLanes, SubPixelIntervalsMergePerPixelAndCountsAreConserved) {
  TrackBuilder tb;
  // 10000 one-tick scopes every 10 ticks over 100000 ticks drawn on 1000 px (100 ticks per pixel): ten per
  // pixel, contiguous, so they join into one run.
  for (uint64_t i = 0; i < 10000; ++i) tb.add(1000 + i * 10, 1000 + i * 10 + 1);
  const PlotArea area = makeArea(100000.0);
  std::vector<LaneItem> items;
  ASSERT_TRUE(layoutTrack(tb.build(), area, 2000, items));
  EXPECT_EQ(totalCount(items), 10000u);
  EXPECT_LE(items.size(), 1000u);
  EXPECT_EQ(items.size(), 1u);
  EXPECT_NEAR(items[0].x0, 100.0f, 0.5f);
  EXPECT_NEAR(items[0].x1, 1100.0f, 1.5f);

  // The same scopes spread out (one per 10 px): separate items of one scope, each at least one pixel wide.
  TrackBuilder sparse;
  for (uint64_t i = 0; i < 100; ++i) sparse.add(1000 + i * 1000, 1000 + i * 1000 + 1);
  items.clear();
  ASSERT_TRUE(layoutTrack(sparse.build(), area, 2000, items));
  EXPECT_EQ(items.size(), 100u);
  for (const LaneItem& it : items) {
    EXPECT_EQ(it.count, 1u);
    EXPECT_GE(it.x1 - it.x0, kMinItemWidth - 0.001f);
  }
}

TEST(InstLanes, ItemCountIsBoundedByThePixelsNotByTheScopes) {
  TrackBuilder tb;
  std::mt19937_64 rng(5);
  uint64_t t = 1000;
  for (int i = 0; i < 200000; ++i) {
    const uint64_t gap = rng() % 6, len = rng() % 4;
    tb.add(t + gap, t + gap + len);
    t += gap + len;
  }
  const PlotArea area = makeArea(static_cast<double>(t - 1000));
  std::vector<LaneItem> items;
  ASSERT_TRUE(layoutTrack(tb.build(), area, 5000, items));
  EXPECT_LE(items.size(), 1000u);
  EXPECT_EQ(totalCount(items), 200000u);
}

TEST(InstLanes, CountsMatchABruteForceOverRandomViews) {
  std::mt19937_64 rng(11);
  TrackBuilder tb;
  uint64_t t = 1000;
  for (int i = 0; i < 5000; ++i) {
    const uint64_t gap = rng() % 400, len = rng() % 900;  // some zero-length, some wide
    tb.add(t + gap, t + gap + len);
    t += gap + len;
  }
  const ScopeTrack& track = tb.build();
  for (int trial = 0; trial < 60; ++trial) {
    const double total = static_cast<double>(t - 1000);
    const double span = std::max(50.0, total * std::pow(0.5, static_cast<double>(rng() % 9)));
    const double start = static_cast<double>(rng() % 1000) / 1000.0 * (total - span);
    const PlotArea area = makeArea(span, 640.0f, 1000, start);
    std::vector<LaneItem> items;
    ASSERT_TRUE(layoutTrack(track, area, 100000, items));
    const double tA = 1000.0 + start, tB = 1000.0 + start + span;
    uint64_t expected = 0;
    for (size_t i = 0; i < track.size(); ++i) {
      if (track.end[i] >= static_cast<uint64_t>(std::floor(tA)) && static_cast<double>(track.begin[i]) < tB)
        ++expected;
    }
    EXPECT_EQ(totalCount(items), expected) << "trial " << trial;
    EXPECT_LE(items.size(), 2000u);
    for (size_t i = 1; i < items.size(); ++i) EXPECT_LE(items[i - 1].x0, items[i].x0);
    for (const LaneItem& it : items) {
      EXPECT_GE(it.x0, 100.0f - 0.01f);
      EXPECT_LE(it.x0, 740.0f + 0.01f);
    }
  }
}

TEST(InstLanes, ItemCapReportsTruncation) {
  TrackBuilder tb;
  for (uint64_t i = 0; i < 100; ++i) tb.add(1000 + i * 10000, 1000 + i * 10000 + 5000);
  const PlotArea area = makeArea(1000000.0);
  std::vector<LaneItem> items;
  EXPECT_FALSE(layoutTrack(tb.build(), area, 10, items));
  EXPECT_EQ(items.size(), 10u);
}

TEST(InstLanes, IntervalsBeforeTheViewAreSkippedAndOnesCrossingItAreClipped) {
  TrackBuilder tb;
  tb.add(1000, 6000);                                              // ends before the view
  tb.add(7000, 20000);                                             // crosses the left edge
  tb.add(30000, 31000);                                            // inside
  tb.add(95000, 200000);                                           // crosses the right edge
  tb.add(300000, 310000);                                          // after the view
  const PlotArea area = makeArea(90000.0, 900.0f, 1000, 10000.0);  // ticks 11000 .. 101000, 0.01 px/tick
  std::vector<LaneItem> items;
  ASSERT_TRUE(layoutTrack(tb.build(), area, 100, items));
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].first, 1u);
  EXPECT_FLOAT_EQ(items[0].x0, 100.0f);  // clipped to the plot area
  EXPECT_EQ(items[2].first, 3u);
  EXPECT_FLOAT_EQ(items[2].x1, 1000.0f);
}

TEST(InstLanes, HitTrackPrefersTheContainingIntervalThenTheNeighbourhood) {
  TrackBuilder tb;
  tb.add(1000, 1010);
  tb.add(2000, 3000);
  tb.add(3002, 3003);
  tb.add(3004, 3005);
  tb.add(3006, 3007);
  const ScopeTrack& track = tb.build();
  const PlotArea area = makeArea(10000.0, 1000.0f);  // 10 ticks per pixel
  const auto xOf = [&](double tick) { return static_cast<float>(100.0 + (tick - 1000.0) / 10.0); };
  const auto hit = hitTrack(track, area, xOf(2500), 2.0f);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->first, 1u);
  EXPECT_EQ(hit->count, 1u);
  // Between the intervals, where no one contains the cursor: the wide one and the three sub-pixel ones all
  // lie within 2 px (20 ticks).
  const auto group = hitTrack(track, area, xOf(3001), 2.0f);
  ASSERT_TRUE(group);
  EXPECT_EQ(group->first, 1u);
  EXPECT_EQ(group->count, 4u);
  const auto contained = hitTrack(track, area, xOf(3004.5), 2.0f);  // inside the third sub-pixel scope
  ASSERT_TRUE(contained);
  EXPECT_EQ(contained->count, 1u);
  EXPECT_EQ(contained->first, 3u);
  EXPECT_FALSE(hitTrack(track, area, xOf(5000), 2.0f));
  EXPECT_FALSE(hitTrack(track, area, xOf(1500), 2.0f));
}

TEST(InstLanes, DepthPackingGivesOneTrackPerDepthAndLinesFollowExpansion) {
  auto paired = std::make_shared<analysis::PairedScopes>();
  paired->byThread.resize(3);
  const auto mk = [](uint64_t b, uint64_t e, uint32_t depth) {
    analysis::ScopeInstance s;
    s.id = StringIdx(7);
    s.begin = b;
    s.end = e;
    s.depth = depth;
    return s;
  };
  paired->byThread[1] = {mk(0, 100, 0), mk(10, 50, 1), mk(20, 30, 2), mk(60, 90, 1), mk(120, 200, 0)};
  paired->byThread[2] = {mk(5, 6, 0)};
  const auto set = buildScopeLanes(paired, 3);
  ASSERT_EQ(set->lanes.size(), 2u);
  EXPECT_TRUE(set->lanesFor(ThreadIdx(0)).empty());
  const ScopeLane& lane = set->lanes[set->lanesFor(ThreadIdx(1))[0]];
  ASSERT_EQ(lane.depths.size(), 3u);
  EXPECT_EQ(lane.depths[0].size(), 2u);
  EXPECT_EQ(lane.depths[1].size(), 2u);
  EXPECT_EQ(lane.depths[2].size(), 1u);
  for (const ScopeTrack& t : lane.depths) {
    for (size_t i = 1; i < t.size(); ++i) {
      EXPECT_LE(t.end[i - 1], t.begin[i]);  // disjoint within a depth
      EXPECT_TRUE(std::is_sorted(t.end.begin(), t.end.end()));
    }
  }
  EXPECT_EQ(set->lines(ThreadIdx(1), false).size(), 1u);  // collapsed: depth 0 only
  EXPECT_EQ(set->lineCount(ThreadIdx(1), false), 1u);
  EXPECT_EQ(set->lineCount(ThreadIdx(1), true), 3u);
  EXPECT_EQ(set->lineCount(ThreadIdx(0), true), 0u);
  const auto expanded = set->lines(ThreadIdx(1), true);
  ASSERT_EQ(expanded.size(), 3u);
  EXPECT_EQ(expanded[2].depth, 2u);
  EXPECT_TRUE(set->lines(ThreadIdx(0), true).empty());
  EXPECT_EQ(set->intervalCount, 6u);
}

TEST(InstLanes, DeepNestingIsCappedPerLaneAndPerRow) {
  auto paired = std::make_shared<analysis::PairedScopes>();
  paired->byThread.resize(1);
  for (uint32_t d = 0; d < 200; ++d) {
    analysis::ScopeInstance s;
    s.id = StringIdx(1);
    s.begin = d;
    s.end = 1000 - d;
    s.depth = d;
    paired->byThread[0].push_back(s);
  }
  const auto set = buildScopeLanes(paired, 1);
  EXPECT_EQ(set->droppedDeep, 200u - kMaxScopeDepth);
  EXPECT_EQ(set->lanes[0].depths.size(), kMaxScopeDepth);
  EXPECT_EQ(set->lines(ThreadIdx(0), true).size(), kMaxLinesPerLane);
}

TEST(InstLanes, FiberLanesFollowTheFibersAndTheirPiecesNeverOverlapOnAThread) {
  const fs::path file = fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "fibers.tachyon";
  auto loaded = model::TraceModel::load(file);
  ASSERT_TRUE(loaded.ok());
  auto model = std::make_shared<model::TraceModel>(std::move(loaded.value()));
  auto paired = analysis::pairScopesByFiber(*model);
  ASSERT_TRUE(paired.ok());
  auto shared = std::make_shared<const analysis::PairedFibers>(std::move(paired.value()));
  ASSERT_TRUE(shared->fiberAware);
  const auto set = buildScopeLanes(shared, model->threads.size());
  EXPECT_TRUE(set->grouped);

  const auto threadOf = [&](uint32_t tid) { return model->tidToThread.at(tid); };
  std::map<uint32_t, std::set<uint64_t>> fibersOfThread;
  for (const ScopeLane& l : set->lanes) fibersOfThread[l.thread.get()].insert(l.fiberId);
  EXPECT_TRUE(fibersOfThread[threadOf(401).get()].count(1));
  EXPECT_TRUE(fibersOfThread[threadOf(401).get()].count(2));
  EXPECT_TRUE(fibersOfThread[threadOf(401).get()].count(3));
  EXPECT_TRUE(fibersOfThread[threadOf(402).get()].count(10));
  EXPECT_TRUE(fibersOfThread[threadOf(402).get()].count(11));
  EXPECT_FALSE(fibersOfThread[threadOf(401).get()].count(10));

  // Fiber 1 has a nested scope: two depths. Its Outer scope is cut into pieces by the switches away.
  const ScopeLane* fiber1 = nullptr;
  for (const ScopeLane& l : set->lanes)
    if (l.fiberId == 1 && l.thread == threadOf(401)) fiber1 = &l;
  ASSERT_NE(fiber1, nullptr);
  EXPECT_GE(fiber1->depths.size(), 2u);
  EXPECT_GE(fiber1->depths[0].size(), 2u);

  // Across all fiber lanes of one thread the pieces of one depth line never overlap another fiber's pieces.
  for (const auto& [thread, fibers] : fibersOfThread) {
    std::vector<std::pair<uint64_t, uint64_t>> all;
    for (const ScopeLane& l : set->lanes) {
      if (l.thread.get() != thread || l.fiberId == 0) continue;
      // Only the outermost pieces: a fiber's nested scopes lie inside them.
      for (size_t i = 0; i < l.depths[0].size(); ++i)
        all.emplace_back(l.depths[0].begin[i], l.depths[0].end[i]);
    }
    std::sort(all.begin(), all.end());
    for (size_t i = 1; i < all.size(); ++i) EXPECT_LE(all[i - 1].second, all[i].first) << "thread " << thread;
  }
  // The lines of thread 401: one per fiber lane when collapsed.
  EXPECT_GE(set->lines(threadOf(401), false).size(), 3u);
}

TEST(InstLanes, NoFiberEventsFallBackToThePlainLanes) {
  const fs::path file = fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "scopes.tachyon";
  auto loaded = model::TraceModel::load(file);
  ASSERT_TRUE(loaded.ok());
  auto model = std::make_shared<model::TraceModel>(std::move(loaded.value()));
  auto fibers = analysis::pairScopesByFiber(*model);
  ASSERT_TRUE(fibers.ok());
  ASSERT_FALSE(fibers->fiberAware);
  const auto grouped = buildScopeLanes(std::make_shared<const analysis::PairedFibers>(std::move(*fibers)),
                                       model->threads.size());
  auto plain = analysis::pairScopes(*model);
  ASSERT_TRUE(plain.ok());
  const auto flat = buildScopeLanes(std::make_shared<const analysis::PairedScopes>(std::move(*plain)),
                                    model->threads.size());
  EXPECT_FALSE(grouped->grouped);
  EXPECT_EQ(grouped->lanes.size(), flat->lanes.size());
  EXPECT_EQ(grouped->intervalCount, flat->intervalCount);
  for (const ScopeLane& l : flat->lanes) EXPECT_EQ(l.fiberId, 0u);
}

TEST(InstLanes, ColoursComeFromTheScopeOrAreHashedFromTheId) {
  EXPECT_EQ(scopeColor(0x0A141EFFu, StringIdx(1)), makeColor(10, 20, 30));
  const Color32 a = scopeColor(0xFFFFFFFFu, StringIdx(5));
  EXPECT_EQ(a, scopeColor(0xFFFFFFFFu, StringIdx(5)));  // deterministic
  EXPECT_NE(a, scopeColor(0xFFFFFFFFu, StringIdx(6)));
  EXPECT_EQ(scopeColor(analysis::kLockColorSentinel, StringIdx(5)), kLockScopeColor);
  EXPECT_NE(a, kLockScopeColor);
}

TEST(InstLanes, LabelsAreIdAndDataAndClipToTheRect) {
  EXPECT_EQ(scopeLabel("Update", ""), "Update");
  EXPECT_EQ(scopeLabel("Load", "level1.pak"), "Load [level1.pak]");
  EXPECT_EQ(clipLabel("Update", 100.0f, 7.0f), "Update");
  EXPECT_EQ(clipLabel("Update", 6.0f * 7.0f, 7.0f), "Update");           // exactly fits
  EXPECT_EQ(clipLabel("Physics::step", 8.0f * 7.0f, 7.0f), "Physic..");  // 8 chars: 6 + ".."
  EXPECT_EQ(clipLabel("Physics::step", 2.0f * 7.0f, 7.0f), "");          // too narrow for anything
  EXPECT_EQ(clipLabel("abc", 0.0f, 7.0f), "");
  // UTF-8: never cut inside a character.
  const std::string text = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";  // six two-byte characters
  const std::string clipped = clipLabel(text, 5.0f * 7.0f, 7.0f);
  EXPECT_EQ(clipped, "\xC3\xA9\xC3\xA9\xC3\xA9..");
}

namespace {

struct TooltipFixture {
  model::TraceModel model;
  std::vector<analysis::ScopeInstance> store;
  std::shared_ptr<ScopeLaneSet> set = std::make_shared<ScopeLaneSet>();
  ScopeLane lane;
  ScopeTrack track;
  TooltipFixture() {
    model.header.start_ts = 1000;
    model::Thread th;
    th.tid = 77;
    th.name = "Main";
    model.threads.push_back(th);
    lane.thread = ThreadIdx(0);
  }
  void add(const char* id, const char* data, uint64_t b, uint64_t e, uint32_t color = 0xFFFFFFFFu,
           bool unterminated = false) {
    analysis::ScopeInstance s;
    s.id = model.strings.intern(id);
    s.data = data ? model.strings.intern(data) : StringIdx();
    s.begin = b;
    s.end = e;
    s.color = color;
    s.unterminated = unterminated;
    store.push_back(s);
  }
  void finish() {
    track = {};
    for (const auto& s : store) {
      track.begin.push_back(s.begin);
      track.end.push_back(s.end);
      track.instance.push_back(&s);
    }
  }
  ScopeTooltipInput input(uint32_t first, uint32_t count = 1) {
    ScopeTooltipInput in;
    in.model = &model;
    in.set = set.get();
    in.lane = &lane;
    in.track = &track;
    in.hit = {first, count};
    in.clockHz = 1e9;
    return in;
  }
};

bool hasLine(const std::vector<std::string>& lines, const std::string& needle) {
  return std::any_of(lines.begin(), lines.end(),
                     [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

}  // namespace

TEST(InstLanes, TooltipDescribesAScope) {
  TooltipFixture f;
  f.add("Update", "level 3", 2000, 5000);
  f.finish();
  const auto lines = scopeTooltipLines(f.input(0));
  EXPECT_TRUE(hasLine(lines, "Scope: Update"));
  EXPECT_TRUE(hasLine(lines, "data: level 3"));
  EXPECT_TRUE(hasLine(lines, "duration: "));
  EXPECT_TRUE(hasLine(lines, "depth: 0"));
  EXPECT_TRUE(hasLine(lines, "thread: Main (tid 77)"));
  EXPECT_FALSE(hasLine(lines, "fiber"));
  EXPECT_FALSE(hasLine(lines, "never ended"));
}

TEST(InstLanes, TooltipOfALockWaitHasTheAddressAndTheContention) {
  TooltipFixture f;
  f.add("Tachyon.LockAcquire", "0x7ffd1000", 2000, 9000, analysis::kLockColorSentinel);
  f.finish();
  auto summary = std::make_shared<analysis::LockSummary>();
  analysis::LockRow row;
  row.address = 0x7ffd1000;
  row.contentionCount = 12;
  row.totalWaitTicks = 50000;
  row.maxWaitTicks = 9000;
  analysis::LockWaiter w;
  w.thread = ThreadIdx(0);
  w.waits = 12;
  w.waitTicks = 50000;
  row.topThreads.push_back(w);
  summary->locks.push_back(row);
  f.set->locks = summary;
  f.set->lockByAddress.emplace(0x7ffd1000, 0u);
  const auto lines = scopeTooltipLines(f.input(0));
  EXPECT_TRUE(hasLine(lines, "Lock wait: Tachyon.LockAcquire"));
  EXPECT_TRUE(hasLine(lines, "lock address: 0x7ffd1000"));
  EXPECT_TRUE(hasLine(lines, "contention: 12 waits"));
  EXPECT_TRUE(hasLine(lines, "waiter: Main"));

  // An unparsable address is said so.
  TooltipFixture g;
  g.add("Tachyon.LockAcquire", "not an address", 2000, 9000, analysis::kLockColorSentinel);
  g.finish();
  EXPECT_TRUE(hasLine(scopeTooltipLines(g.input(0)), "lock address: unknown"));
}

TEST(InstLanes, TooltipFlagsPiecesFibersOpenEndsAndMergedGroups) {
  TooltipFixture f;
  f.add("Outer", nullptr, 2000, 9000, 0xFFFFFFFFu, true);
  f.add("S", nullptr, 10000, 10001);
  f.add("S", nullptr, 10002, 10003);
  f.finish();
  f.lane.fiberId = 0x2a;
  f.track.begin[0] = 2000;
  f.track.end[0] = 3000;  // a piece of the scope
  auto lines = scopeTooltipLines(f.input(0));
  EXPECT_TRUE(hasLine(lines, "this piece"));
  EXPECT_TRUE(hasLine(lines, "fiber: 0x2a"));
  EXPECT_TRUE(hasLine(lines, "never ended"));
  lines = scopeTooltipLines(f.input(1, 2));
  EXPECT_TRUE(hasLine(lines, "2 scopes"));
}

// ---- row layout with scope lines
// -----------------------------------------------------------------------------

TEST(InstLanes, RowLayoutAddsTheScopeLinesBelowTheSampledPart) {
  const RowGeometry g;
  const std::vector<ThreadIdx> order{ThreadIdx(0), ThreadIdx(1), ThreadIdx(2)};
  std::vector<RowFlags> flags(3);
  flags[1].expanded = true;
  const std::vector<uint32_t> flame{4, 4, 4};
  std::vector<RowExtra> extras(3);
  extras[0] = {RowMode::Sampled, 5};  // lines are ignored without the scopes part
  extras[1] = {RowMode::Both, 3};
  extras[2] = {RowMode::Scopes, 2};
  const auto layout = layoutRows(order, flags, flame, g, extras);
  ASSERT_EQ(layout.size(), 3u);
  EXPECT_EQ(layout[0].scopeLines, 0u);
  const float sampled = g.rowPadding * 2 + g.bandHeight + g.stripHeight;
  EXPECT_FLOAT_EQ(layout[0].height, sampled);
  // Expanded + both: the flame chart, then the scope lines.
  EXPECT_FLOAT_EQ(layout[1].height,
                  sampled + g.flameGap + 4 * g.flameDepthHeight + g.scopeGap + 3 * g.scopeLineHeight);
  // Scopes only: no strip, no flame chart.
  EXPECT_FLOAT_EQ(layout[2].height, g.rowPadding * 2 + g.bandHeight + g.scopeGap + 2 * g.scopeLineHeight);
  EXPECT_GT(layout[2].y0, layout[1].y0 + layout[1].height);

  // Hit testing finds the same sections.
  const RowLayout& both = layout[1];
  EXPECT_EQ(hitRowSection(both, g, bandTop(g) + 1).section, RowSection::Band);
  EXPECT_EQ(hitRowSection(both, g, stripTop(g) + 2).section, RowSection::Strip);
  EXPECT_EQ(hitRowSection(both, g, flameDepthTop(g, 2) + 1).section, RowSection::Flame);
  const RowHit line2 = hitRowSection(both, g, scopeLineTop(both, g, 2) + 1);
  EXPECT_EQ(line2.section, RowSection::Scope);
  EXPECT_EQ(line2.depth, 2u);  // zero-based scope line
  EXPECT_EQ(hitRowSection(both, g, scopeLineTop(both, g, 3) + 1).section, RowSection::None);
  const RowLayout& scopes = layout[2];
  EXPECT_EQ(hitRowSection(scopes, g, scopeLineTop(scopes, g, 0) + 1).section, RowSection::Scope);
  EXPECT_FLOAT_EQ(scopeTop(scopes, g), stripTop(g) + g.scopeGap);  // right below the band
  // Without extras the layout is exactly what it was before WP-54.
  const auto plain = layoutRows(order, flags, flame, g);
  EXPECT_FLOAT_EQ(plain[0].height, sampled);
  EXPECT_EQ(plain[1].mode, RowMode::Sampled);
}
