// WP-53 against the tracegen fixtures: `scopes` (blended rows and placements against expected.json's
// `blended` section, computed by tracegen's independent per-sample reference) and `fibers` (per-fiber lanes
// and pieces against expected `fibers`, scope statistics, and blending by fiber against a brute-force sweep).
#include "analysis/fibers.h"
#include "analysis/query.h"
#include "analysis/scope_blend.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace tachyon::analysis {
namespace {

using model::TraceModel;

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

struct Loaded {
  explicit Loaded(const std::string& name) : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    std::ifstream f(generated(name + ".expected.json"));
    expected = nlohmann::json::parse(f);
    auto r = TraceModel::load(generated(name + ".tachyon"));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(generated(name + ".symbols.json")).ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
  }
  TraceModel m;
  nlohmann::json expected;
  symbols::SymbolService svc;
};

// FunctionKey of an expected function / scope / placement-parent description.
FunctionKey keyOf(const TraceModel& m, const nlohmann::json& fn) {
  const std::string kind = fn["kind"].get<std::string>();
  if (kind == "scope") {
    const StringIdx id = m.strings.find(fn["id"].get<std::string>());
    EXPECT_TRUE(id.valid()) << fn["id"];
    return FunctionKey{symbols::kScopeModule, id.get()};
  }
  if (kind == "unmapped") return FunctionKey{ModuleIdx::invalid(), fn["start_rva"].get<uint64_t>()};
  return FunctionKey{ModuleIdx(fn["module"].get<uint32_t>()), fn["start_rva"].get<uint64_t>()};
}

AggregationOptions blendOn() {
  AggregationOptions o;
  o.blendScopes = true;
  return o;
}

// Sums the inclusive ticks and sample counts of every scope node of a tree by (scope key, parent key).
void collectPlacements(const NodeAgg& root,
                       std::map<std::pair<FunctionKey, FunctionKey>, std::pair<uint64_t, uint64_t>>& out) {
  std::vector<std::pair<const NodeAgg*, FunctionKey>> work;
  work.emplace_back(&root, kRootKey);
  while (!work.empty()) {
    auto [node, parent] = work.back();
    work.pop_back();
    if (node->key.isScope()) {
      auto& acc = out[{node->key, parent}];
      acc.first += node->inclusiveTicks;
      acc.second += node->sampleCount;
    }
    for (const NodeAgg& c : node->children) work.emplace_back(&c, node->key);
  }
}

TEST(ScopesFixture, BlendedFunctionRowsMatchExpected) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc, blendOn());
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok()) << rows.error().toString();
  std::map<FunctionKey, FunctionRow> got;
  for (const FunctionRow& r : *rows) got[r.key] = r;
  size_t expectedWithSamples = 0;
  for (const auto& fn : l.expected["blended"]["functions"]) {
    if (fn["inclusive_samples"].get<uint64_t>() == 0) continue;
    ++expectedWithSamples;
    const FunctionKey k = keyOf(l.m, fn);
    const auto it = got.find(k);
    ASSERT_NE(it, got.end()) << fn["name"];
    EXPECT_EQ(it->second.inclusiveTicks, fn["inclusive_ticks"].get<uint64_t>()) << fn["name"];
    EXPECT_EQ(it->second.exclusiveTicks, fn["exclusive_ticks"].get<uint64_t>()) << fn["name"];
    EXPECT_EQ(it->second.instances, fn["inclusive_samples"].get<uint32_t>()) << fn["name"];
  }
  EXPECT_EQ(got.size(), expectedWithSamples);
}

TEST(ScopesFixture, BlendedPerThreadRowsMatchExpected) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc, blendOn());
  for (const auto& fn : l.expected["blended"]["functions"]) {
    for (const auto& pt : fn["per_thread"]) {
      Filter f;
      f.threads = std::vector<ThreadIdx>{l.m.tidToThread.at(pt["tid"].get<uint32_t>())};
      auto b = butterfly(ctx, f, keyOf(l.m, fn));
      ASSERT_TRUE(b.ok());
      EXPECT_EQ(b->self.inclusiveTicks, pt["inclusive_ticks"].get<uint64_t>()) << fn["name"];
      EXPECT_EQ(b->self.exclusiveTicks, pt["exclusive_ticks"].get<uint64_t>()) << fn["name"];
    }
  }
}

TEST(ScopesFixture, EveryScopeIsPlacedDirectlyAboveTheExpectedFrame) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc, blendOn());
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok());
  std::map<std::pair<FunctionKey, FunctionKey>, std::pair<uint64_t, uint64_t>> got;
  collectPlacements(*tree, got);
  std::set<std::pair<FunctionKey, FunctionKey>> seen;
  for (const auto& p : l.expected["blended"]["placements"]) {
    const FunctionKey scope = keyOf(l.m, nlohmann::json{{"kind", "scope"}, {"id", p["scope"]}});
    const FunctionKey parent = keyOf(l.m, p["parent"]);
    const auto it = got.find({scope, parent});
    ASSERT_NE(it, got.end()) << p["scope"] << " above " << p["parent"]["name"];
    EXPECT_EQ(it->second.first, p["ticks"].get<uint64_t>()) << p["scope"] << " above " << p["parent"]["name"];
    EXPECT_EQ(it->second.second, p["samples"].get<uint64_t>())
        << p["scope"] << " above " << p["parent"]["name"];
    seen.insert({scope, parent});
  }
  EXPECT_EQ(seen.size(), got.size()) << "placements the reference does not know";
}

TEST(ScopesFixture, BlendingOffEqualsTheUnblendedExpectedFunctions) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  std::map<FunctionKey, FunctionRow> got;
  for (const FunctionRow& r : *rows) got[r.key] = r;
  for (const auto& fn : l.expected["functions"]) {
    if (fn["inclusive_samples"].get<uint64_t>() == 0) continue;
    const auto it = got.find(keyOf(l.m, fn));
    ASSERT_NE(it, got.end());
    EXPECT_EQ(it->second.inclusiveTicks, fn["inclusive_ticks"].get<uint64_t>());
    EXPECT_EQ(it->second.exclusiveTicks, fn["exclusive_ticks"].get<uint64_t>());
  }
  for (const FunctionRow& r : *rows) EXPECT_FALSE(r.key.isScope());
}

TEST(ScopesFixture, BlendedTreeAndRowsAgreeAndTotalsAreUnchanged) {
  Loaded l("scopes");
  QueryContext on(l.m, l.svc, blendOn());
  QueryContext off(l.m, l.svc);
  auto a = topDownTree(on, {});
  auto b = topDownTree(off, {});
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->inclusiveTicks, b->inclusiveTicks);
  EXPECT_EQ(a->sampleCount, b->sampleCount);
  auto rows = bottomUpFunctions(on, {});
  ASSERT_TRUE(rows.ok());
  uint64_t exclusive = 0;
  for (const FunctionRow& r : *rows) exclusive += r.exclusiveTicks;
  EXPECT_EQ(exclusive, a->inclusiveTicks);  // every sample has exactly one leaf
}

TEST(ScopesFixture, ScopeStatsMatchExpected) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc);
  for (const auto& sc : l.expected["scopes"]) {
    const StringIdx id = l.m.strings.find(sc["id"].get<std::string>());
    auto s = scopeStats(ctx, {}, id);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->count, sc["instances"].get<uint32_t>()) << sc["id"];
    EXPECT_EQ(s->min, sc["min_ticks"].get<double>()) << sc["id"];
    EXPECT_EQ(s->max, sc["max_ticks"].get<double>()) << sc["id"];
    EXPECT_EQ(s->totalTicks, sc["total_ticks"].get<uint64_t>()) << sc["id"];
  }
}

// ----------------------------------------------------------------------------------------------------------

struct ExpectedSegment {
  uint32_t tid;
  std::string scope;
  uint32_t depth;
  uint64_t scopeBegin, begin, end;
  bool operator==(const ExpectedSegment&) const = default;
  auto key() const { return std::tie(begin, tid, depth, scope, end, scopeBegin); }
};

TEST(FibersFixture, LanesAndPiecesMatchExpected) {
  Loaded l("fibers");
  auto paired = pairScopesByFiber(l.m);
  ASSERT_TRUE(paired.ok()) << paired.error().toString();
  ASSERT_TRUE(paired->fiberAware);
  std::map<uint64_t, const FiberLane*> lanes;
  for (const FiberLane& lane : paired->lanes)
    if (lane.fiberId != 0) lanes[lane.fiberId] = &lane;
  size_t expectedFibers = 0;
  for (const auto& fib : l.expected["fibers"]) {
    ++expectedFibers;
    const uint64_t id = fib["fiber_id"].get<uint64_t>();
    ASSERT_TRUE(lanes.count(id)) << "fiber " << id;
    const FiberLane& lane = *lanes.at(id);
    std::vector<ExpectedSegment> want, got;
    for (const auto& s : fib["segments"])
      want.push_back({s["tid"].get<uint32_t>(), s["scope"].get<std::string>(), s["depth"].get<uint32_t>(),
                      s["scope_begin"].get<uint64_t>(), s["begin"].get<uint64_t>(),
                      s["end"].get<uint64_t>()});
    std::map<std::string, uint64_t> ticks;
    for (const ScopePiece& p : lane.pieces) {
      const ScopeInstance& si = lane.scopes[p.scope];
      got.push_back({l.m.threads[p.thread.get()].tid, std::string(l.m.strings.get(si.id)), si.depth, si.begin,
                     p.begin, p.end});
      ticks[std::string(l.m.strings.get(si.id))] += p.end - p.begin;
    }
    std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.key() < b.key(); });
    std::sort(got.begin(), got.end(), [](const auto& a, const auto& b) { return a.key() < b.key(); });
    EXPECT_EQ(got, want) << "fiber " << id;
    // The order of pieces is the documented one (begin, depth, scope order), so per begin/depth it agrees.
    for (size_t i = 1; i < lane.pieces.size(); ++i) {
      const ScopePiece &a = lane.pieces[i - 1], &b = lane.pieces[i];
      EXPECT_LE(a.begin, b.begin);
    }
    for (const auto& st : fib["scope_ticks"])
      EXPECT_EQ(ticks[st["scope"].get<std::string>()], st["ticks"].get<uint64_t>()) << "fiber " << id;
  }
  EXPECT_EQ(lanes.size(), expectedFibers);
  EXPECT_EQ(paired->scopes.unterminatedCount, 0u);
  EXPECT_EQ(paired->scopes.droppedEvents, 0u);
}

TEST(FibersFixture, ScopeSpanningAFiberSwitchIsSplitAroundTheSwitchedOutTime) {
  Loaded l("fibers");
  auto paired = pairScopesByFiber(l.m);
  ASSERT_TRUE(paired.ok());
  const FiberLane* fiber1 = nullptr;
  for (const FiberLane& lane : paired->lanes)
    if (lane.fiberId == 1) fiber1 = &lane;
  ASSERT_NE(fiber1, nullptr);
  // "Outer" runs 50 ms..450 ms of the fiber's logical time; fiber 1 is switched out for [100 ms, 200 ms) and
  // [300 ms, 400 ms) of the trace (switch gap 20 us): three pieces.
  std::vector<const ScopePiece*> outer;
  for (const ScopePiece& p : fiber1->pieces)
    if (l.m.strings.get(fiber1->scopes[p.scope].id) == "Outer") outer.push_back(&p);
  ASSERT_EQ(outer.size(), 3u);
  const ScopeInstance& inst = fiber1->scopes[outer[0]->scope];
  EXPECT_EQ(inst.end - inst.begin, 4000000u);  // wall: 400 ms at 10 MHz
  uint64_t ran = 0;
  for (const ScopePiece* p : outer) ran += p->end - p->begin;
  EXPECT_EQ(ran, 1999600u);  // expected scope_ticks: the wall time minus the switched-out time
}

TEST(FibersFixture, ScopeStatsAreTheSamePerThreadAndPerFiber) {
  Loaded l("fibers");
  AggregationOptions grouped;
  grouped.groupByFiber = true;
  QueryContext plain(l.m, l.svc), byFiber(l.m, l.svc, grouped);
  for (const auto& sc : l.expected["scopes"]) {
    const StringIdx id = l.m.strings.find(sc["id"].get<std::string>());
    for (const QueryContext* ctx : {&plain, &byFiber}) {
      auto s = scopeStats(*ctx, {}, id);
      ASSERT_TRUE(s.ok());
      EXPECT_EQ(s->count, sc["instances"].get<uint32_t>()) << sc["id"];
      EXPECT_EQ(s->min, sc["min_ticks"].get<double>()) << sc["id"];
      EXPECT_EQ(s->max, sc["max_ticks"].get<double>()) << sc["id"];
      EXPECT_EQ(s->totalTicks, sc["total_ticks"].get<uint64_t>()) << sc["id"];
    }
  }
}

// Brute force: for every scope id, the weight of the samples that lie in a piece of a scope with that id on
// the sample's thread, and the weight of all samples.
TEST(FibersFixture, BlendByFiberMatchesABruteForceSweepOfThePieces) {
  Loaded l("fibers");
  AggregationOptions o;
  o.blendScopes = true;
  o.groupByFiber = true;
  QueryContext ctx(l.m, l.svc, o);
  auto paired = pairScopesByFiber(l.m);
  ASSERT_TRUE(paired.ok());
  std::map<StringIdx, uint64_t> wantTicks, wantSamples;
  uint64_t total = 0;
  for (uint32_t t = 0; t < l.m.threads.size(); ++t) {
    forEachWeightedSample(l.m, ThreadIdx(t), 0, UINT64_MAX, {}, [&](uint64_t ts, CallstackIdx, uint64_t w) {
      total += w;
      std::set<StringIdx> ids;  // a scope id counts once per sample
      for (const FiberLane& lane : paired->lanes)
        for (const ScopePiece& p : lane.pieces)
          if (p.thread.get() == t && p.begin <= ts && ts < p.end) ids.insert(lane.scopes[p.scope].id);
      for (StringIdx id : ids) {
        wantTicks[id] += w;
        ++wantSamples[id];
      }
    });
  }
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  std::map<StringIdx, FunctionRow> got;
  uint64_t exclusive = 0;
  for (const FunctionRow& r : *rows) {
    exclusive += r.exclusiveTicks;
    if (r.key.isScope()) got[StringIdx(static_cast<uint32_t>(r.key.value))] = r;
  }
  EXPECT_EQ(exclusive, total);
  EXPECT_EQ(got.size(), wantTicks.size());
  for (const auto& [id, ticks] : wantTicks) {
    ASSERT_TRUE(got.count(id)) << l.m.strings.get(id);
    EXPECT_EQ(got.at(id).inclusiveTicks, ticks) << l.m.strings.get(id);
    EXPECT_EQ(got.at(id).instances, wantSamples.at(id)) << l.m.strings.get(id);
  }
  EXPECT_GE(got.size(), 6u);  // the fixture has scopes on both threads
}

}  // namespace
}  // namespace tachyon::analysis
