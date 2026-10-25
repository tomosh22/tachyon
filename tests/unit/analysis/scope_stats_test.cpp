// WP-35: scope pairing and scopeStats (exact), on hand-built events and on the `scopes` fixture.
#include "analysis/query.h"

#include "aggregation_test_util.h"
#include "analysis/query_cache.h"
#include "analysis/scope_pairing.h"
#include "analysis/stats.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;

trace::ScopeEventRec ev(uint64_t ts, uint32_t tid, uint8_t kind, StringIdx id = {}, uint64_t caller = 0) {
  trace::ScopeEventRec e{};
  e.ts = ts;
  e.tid = tid;
  e.kind = kind;
  e.id = id;
  e.caller_addr = caller;
  return e;
}

struct Events {
  HandModel hm;
  StringIdx frame, update, other;
  explicit Events(uint32_t threads = 1) : hm(threads) {
    frame = hm.m.strings.intern("Frame");
    update = hm.m.strings.intern("Update");
    other = hm.m.strings.intern("Other");
    for (uint32_t t = 0; t < threads; ++t) hm.state(t, 0, 100000, ThreadState::Running);
  }
  void begin(uint64_t ts, StringIdx id, uint32_t tid = 100) {
    hm.m.scopeEvents.push_back(ev(ts, tid, trace::kScopeBegin, id, 0x1000 + ts));
  }
  void end(uint64_t ts, uint32_t tid = 100) { hm.m.scopeEvents.push_back(ev(ts, tid, trace::kScopeEnd)); }
  void finish() { hm.finish(); }
};

TEST(ScopePairing, PairsWithStackDisciplineAndRecordsDepth) {
  Events e;
  e.begin(100, e.frame);
  e.begin(200, e.update);
  e.end(300);
  e.begin(400, e.update);
  e.end(500);
  e.end(1000);
  auto r = pairScopes(e.hm.m);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->byThread.size(), 1u);
  const auto& list = r->byThread[0];
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list[0].id, e.frame);
  EXPECT_EQ(list[0].begin, 100u);
  EXPECT_EQ(list[0].end, 1000u);
  EXPECT_EQ(list[0].depth, 0u);
  EXPECT_EQ(list[0].callerAddr, 0x1000u + 100u);
  EXPECT_FALSE(list[0].unterminated);
  EXPECT_EQ(list[1].id, e.update);
  EXPECT_EQ(list[1].begin, 200u);
  EXPECT_EQ(list[1].end, 300u);
  EXPECT_EQ(list[1].depth, 1u);
  EXPECT_EQ(list[2].begin, 400u);
  EXPECT_EQ(list[2].end, 500u);
  EXPECT_EQ(r->unterminatedCount, 0u);
  EXPECT_EQ(r->droppedEvents, 0u);
  EXPECT_TRUE(r->orphanEndTs[0].empty());
  EXPECT_GT(r->heapBytes(), sizeof(PairedScopes));
}

TEST(ScopePairing, UnterminatedOrphanAndUnknownThreadEvents) {
  Events e;
  e.hm.m.header.end_ts = 5000;
  e.end(50);  // orphan
  e.begin(100, e.frame);
  e.begin(200, e.update);
  e.end(300);
  e.end(400);
  e.end(450);                                                               // orphan
  e.begin(600, e.other);                                                    // never ended
  e.begin(700, e.update);                                                   // never ended, nested
  e.hm.m.scopeEvents.push_back(ev(800, 999, trace::kScopeBegin, e.frame));  // unknown tid
  e.hm.m.scopeEvents.push_back(ev(810, 0, trace::kScopeEnd));               // tid 0
  auto r = pairScopes(e.hm.m);
  ASSERT_TRUE(r.ok());
  const auto& list = r->byThread[0];
  ASSERT_EQ(list.size(), 4u);
  EXPECT_EQ(r->orphanEndTs[0], (std::vector<uint64_t>{50, 450}));
  EXPECT_EQ(r->droppedEvents, 2u);
  EXPECT_EQ(r->unterminatedCount, 2u);
  EXPECT_TRUE(list[2].unterminated);
  EXPECT_EQ(list[2].end, 5000u);
  EXPECT_EQ(list[2].depth, 0u);
  EXPECT_TRUE(list[3].unterminated);
  EXPECT_EQ(list[3].depth, 1u);
  EXPECT_EQ(list[3].end, 5000u);
  EXPECT_FALSE(list[0].unterminated);
}

TEST(ScopePairing, CancelledPairingReturnsCancelled) {
  Events e;
  e.begin(1, e.frame);
  std::atomic<bool> flag{true};
  auto r = pairScopes(e.hm.m, Cancel{&flag});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}

TEST(ScopeStats, ExactStatisticsOfNestedScopes) {
  Events e;
  // Update: 100, 100, 50 (nested in Frame); Frame: 900 and 100.
  e.begin(100, e.frame);
  e.begin(200, e.update);
  e.end(300);
  e.begin(400, e.update);
  e.end(500);
  e.begin(600, e.update);
  e.end(650);
  e.end(1000);
  e.begin(2000, e.frame);
  e.end(2100);
  e.finish();
  QueryContext ctx(e.hm.m, *e.hm.svc);

  auto u = scopeStats(ctx, {}, e.update);
  ASSERT_TRUE(u.ok()) << u.error().toString();
  EXPECT_EQ(u->count, 3u);
  EXPECT_EQ(u->min, 50);
  EXPECT_EQ(u->max, 100);
  EXPECT_EQ(u->median, 100);
  EXPECT_DOUBLE_EQ(u->p25, 50);
  EXPECT_EQ(u->p75, 100);
  EXPECT_EQ(u->totalTicks, 250u);
  EXPECT_DOUBLE_EQ(u->mean, 250.0 / 3);
  ASSERT_EQ(u->histogram.size(), 8u);
  EXPECT_EQ(u->histogram[6], 1u);  // 50 in [32, 64)
  EXPECT_EQ(u->histogram[7], 2u);  // 100 in [64, 128)
  EXPECT_EQ(u->unterminated, 0u);
  EXPECT_EQ(u->orphanEnds, 0u);

  auto f = scopeStats(ctx, {}, e.frame);
  ASSERT_TRUE(f.ok());
  EXPECT_EQ(f->count, 2u);
  EXPECT_EQ(f->min, 100);
  EXPECT_EQ(f->max, 900);
  EXPECT_EQ(f->median, 100);

  auto none = scopeStats(ctx, {}, e.other);
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->count, 0u);
  EXPECT_EQ(scopeStats(ctx, {}, StringIdx::invalid()).error().code, ErrorCode::InvalidArgument);

  // functionStats with a scope key is the same query.
  auto viaFn = functionStats(ctx, {}, FunctionKey::scope(e.update));
  ASSERT_TRUE(viaFn.ok());
  EXPECT_EQ(viaFn->count, 3u);
  EXPECT_EQ(viaFn->totalTicks, 250u);
}

TEST(ScopeStats, UnterminatedAndOrphansAreCountedNotHidden) {
  Events e;
  e.hm.m.header.end_ts = 10000;
  e.end(40);  // orphan
  e.begin(100, e.update);
  e.end(200);
  e.begin(500, e.update);  // unterminated: ends at the trace end
  e.end(7000, 100);        // pairs with the begin at 500
  e.begin(8000, e.update);
  e.end(8010);
  e.end(8020);              // orphan
  e.begin(9000, e.update);  // unterminated
  e.finish();
  QueryContext ctx(e.hm.m, *e.hm.svc);
  auto s = scopeStats(ctx, {}, e.update);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s->count, 4u);
  EXPECT_EQ(s->unterminated, 1u);
  EXPECT_EQ(s->orphanEnds, 2u);
  EXPECT_EQ(s->max, 6500);  // 7000 - 500
  EXPECT_EQ(s->min, 10);
  EXPECT_EQ(s->totalTicks, 100u + 6500u + 10u + 1000u);  // the unterminated one runs 9000..10000

  // orphans are filtered by time like instances
  Filter late;
  late.t0 = 8000;
  auto l = scopeStats(ctx, late, e.update);
  ASSERT_TRUE(l.ok());
  EXPECT_EQ(l->count, 2u);
  EXPECT_EQ(l->orphanEnds, 1u);
  EXPECT_EQ(l->unterminated, 1u);
}

TEST(ScopeStats, FiltersSelectByBeginAndThread) {
  Events e(2);
  for (uint64_t b = 1000; b < 5000; b += 1000) {
    e.begin(b, e.update, 100);
    e.end(b + 10 * (b / 1000), 100);  // durations 10, 20, 30, 40
    e.begin(b + 5, e.update, 101);
    e.end(b + 105, 101);  // 100 each
  }
  e.finish();
  QueryContext ctx(e.hm.m, *e.hm.svc);
  auto all = scopeStats(ctx, {}, e.update);
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all->count, 8u);
  EXPECT_EQ(all->totalTicks, 100u + 400u);

  Filter t0only;
  t0only.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  auto a = scopeStats(ctx, t0only, e.update);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->count, 4u);
  EXPECT_EQ(a->max, 40);

  Filter range;  // half-open on the begin: [2000, 4000) takes begins 2000, 2005, 3000, 3005
  range.t0 = 2000;
  range.t1 = 4000;
  auto r = scopeStats(ctx, range, e.update);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->count, 4u);
  Filter edge;
  edge.t0 = 2005;
  edge.t1 = 3000;  // begin 3000 excluded (half-open), 2005 included
  auto g = scopeStats(ctx, edge, e.update);
  ASSERT_TRUE(g.ok());
  EXPECT_EQ(g->count, 1u);
  EXPECT_EQ(g->max, 100);

  // A scope that began before t0 is not selected even if it overlaps the range.
  Filter after;
  after.t0 = 1001;
  after.t1 = 1002;
  EXPECT_EQ(scopeStats(ctx, after, e.update)->count, 0u);

  // subtreeRoot / function do not apply to scopes: results are identical (and share the cache entry).
  Filter junk;
  junk.subtreeRoot = e.hm.stack({0x1000});
  junk.function = ctx.functionIdx(FunctionKey{ModuleIdx(0), 0x1000});
  auto j = scopeStatsShared(ctx, junk, e.update);
  auto k = scopeStatsShared(ctx, {}, e.update);
  ASSERT_TRUE(j.ok() && k.ok());
  EXPECT_EQ(j->get(), k->get());
}

TEST(ScopeStats, ZeroDurationAndEqualTimestampsKeepFileOrder) {
  Events e;
  e.begin(100, e.update);
  e.end(100);  // zero duration
  e.begin(100, e.frame);
  e.begin(100, e.update);
  e.end(100);
  e.end(100);
  e.finish();
  QueryContext ctx(e.hm.m, *e.hm.svc);
  auto u = scopeStats(ctx, {}, e.update);
  ASSERT_TRUE(u.ok());
  EXPECT_EQ(u->count, 2u);
  EXPECT_EQ(u->max, 0);
  ASSERT_EQ(u->histogram.size(), 1u);
  EXPECT_EQ(u->histogram[0], 2u);
  auto r = pairScopes(e.hm.m);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->byThread[0][2].depth, 1u);  // update inside frame
}

TEST(ScopeStats, ResultsAndPairingAreCached) {
  Events e;
  e.begin(100, e.update);
  e.end(200);
  e.begin(300, e.frame);
  e.end(500);
  e.finish();
  QueryContext ctx(e.hm.m, *e.hm.svc);
  auto a = scopeStatsShared(ctx, {}, e.update);
  ASSERT_TRUE(a.ok());
  const uint64_t pairMissesBefore = ctx.cache().counters(QueryKind::ScopePairs).misses;
  EXPECT_EQ(pairMissesBefore, 1u);
  auto b = scopeStatsShared(ctx, {}, e.update);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a->get(), b->get());
  EXPECT_EQ(ctx.cache().counters(QueryKind::ScopeStats).hits, 1u);
  // A different scope id reuses the paired events.
  auto c = scopeStatsShared(ctx, {}, e.frame);
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(ctx.cache().counters(QueryKind::ScopePairs).misses, 1u);
  EXPECT_GE(ctx.cache().counters(QueryKind::ScopePairs).hits, 1u);
  EXPECT_EQ(c->get()->count, 1u);
}

TEST(ScopeStats, CancelledQueryReturnsCancelled) {
  Events e;
  e.begin(100, e.update);
  e.end(200);
  e.finish();
  QueryContext ctx(e.hm.m, *e.hm.svc);
  std::atomic<bool> flag{true};
  auto r = scopeStats(ctx, {}, e.update, Cancel{&flag});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
  EXPECT_EQ(ctx.cacheStats().entries, 0u);
  EXPECT_TRUE(scopeStats(ctx, {}, e.update).ok());
}

// ---- fixture -----------------------------------------------------------------------------------------

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

struct Loaded {
  explicit Loaded(const std::string& name) : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    std::ifstream f(generated(name + ".expected.json"));
    expected = nlohmann::json::parse(f);
    auto r = model::TraceModel::load(generated(name + ".tachyon"));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(generated(name + ".symbols.json")).ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
  }
  model::TraceModel m;
  nlohmann::json expected;
  symbols::SymbolService svc;
};

// Independent reference: pairs the raw events with a plain per-tid stack, no shared code with scope_pairing.
std::map<std::string, std::vector<uint64_t>> bruteForceDurations(const model::TraceModel& m) {
  std::map<std::string, std::vector<uint64_t>> out;
  std::map<uint32_t, std::vector<std::pair<std::string, uint64_t>>> open;
  for (const trace::ScopeEventRec& rec : m.scopeEvents) {
    const trace::ScopeEventRec e = rec;
    auto& st = open[e.tid];
    if (e.kind == trace::kScopeBegin) {
      st.emplace_back(std::string(m.strings.get(e.id)), e.ts);
    } else {
      EXPECT_FALSE(st.empty());
      if (st.empty()) continue;
      out[st.back().first].push_back(e.ts - st.back().second);
      st.pop_back();
    }
  }
  for (const auto& [tid, st] : open) EXPECT_TRUE(st.empty());
  return out;
}

double refQuantile(std::vector<uint64_t> v, double q) {
  std::sort(v.begin(), v.end());
  const double h = q * static_cast<double>(v.size() - 1);
  const size_t lo = static_cast<size_t>(h);
  const size_t hi = std::min(lo + 1, v.size() - 1);
  return static_cast<double>(v[lo]) +
         (h - static_cast<double>(lo)) * (static_cast<double>(v[hi]) - static_cast<double>(v[lo]));
}

TEST(ScopeStatsFixture, ExactAgainstExpectedAndBruteForce) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc);
  const auto brute = bruteForceDurations(l.m);
  ASSERT_FALSE(l.expected["scopes"].empty());
  EXPECT_EQ(brute.size(), l.expected["scopes"].size());
  for (const auto& es : l.expected["scopes"]) {
    const std::string id = es["id"].get<std::string>();
    const StringIdx idx = l.m.strings.find(id);
    ASSERT_TRUE(idx.valid()) << id;
    auto s = scopeStats(ctx, {}, idx);
    ASSERT_TRUE(s.ok()) << s.error().toString();
    // expected.json (written by tracegen from the same events)
    EXPECT_EQ(s->count, es["instances"].get<uint32_t>()) << id;
    EXPECT_EQ(s->min, static_cast<double>(es["min_ticks"].get<uint64_t>())) << id;
    EXPECT_EQ(s->max, static_cast<double>(es["max_ticks"].get<uint64_t>())) << id;
    EXPECT_EQ(s->totalTicks, es["total_ticks"].get<uint64_t>()) << id;
    EXPECT_DOUBLE_EQ(s->mean,
                     static_cast<double>(es["total_ticks"].get<uint64_t>()) / es["instances"].get<double>())
        << id;
    EXPECT_EQ(s->median, static_cast<double>(es["median_ticks"].get<uint64_t>())) << id;
    // brute force from the raw events, full statistics
    const auto it = brute.find(id);
    ASSERT_NE(it, brute.end()) << id;
    const std::vector<uint64_t>& d = it->second;
    EXPECT_EQ(s->count, d.size()) << id;
    EXPECT_DOUBLE_EQ(s->p25, refQuantile(d, 0.25)) << id;
    EXPECT_DOUBLE_EQ(s->median, refQuantile(d, 0.5)) << id;
    EXPECT_DOUBLE_EQ(s->p75, refQuantile(d, 0.75)) << id;
    std::vector<uint32_t> hist;
    for (uint64_t v : d) {
      const uint32_t b = statsBucketOf(v);
      if (hist.size() <= b) hist.resize(b + 1, 0);
      ++hist[b];
    }
    EXPECT_EQ(s->histogram, hist) << id;
    EXPECT_EQ(s->unterminated, 0u);
    EXPECT_EQ(s->orphanEnds, 0u);
  }
}

TEST(ScopeStatsFixture, ThreadFilterAndTimeSplitsAddUp) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc);
  const StringIdx frame = l.m.strings.find("Frame");
  ASSERT_TRUE(frame.valid());
  auto all = scopeStats(ctx, {}, frame);
  ASSERT_TRUE(all.ok());
  const uint64_t mid = l.m.header.start_ts + (l.m.header.end_ts - l.m.header.start_ts) / 2;
  Filter lo, hi;
  lo.t1 = mid;
  hi.t0 = mid;
  auto a = scopeStats(ctx, lo, frame);
  auto b = scopeStats(ctx, hi, frame);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->count + b->count, all->count);
  EXPECT_EQ(a->totalTicks + b->totalTicks, all->totalTicks);
  EXPECT_GT(a->count, 0u);
  EXPECT_GT(b->count, 0u);

  // Frame scopes live on tid 301 only.
  Filter w;
  w.threads = std::vector<ThreadIdx>{l.m.tidToThread.at(302)};
  auto none = scopeStats(ctx, w, frame);
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->count, 0u);
}

}  // namespace
}  // namespace tachyon::analysis
