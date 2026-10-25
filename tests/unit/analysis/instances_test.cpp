// WP-47: functionInstances / scopeInstances equal the statistics of the same query (count, sum, min, max,
// histogram) for every function of the `medium` fixture and every scope of the `scopes` fixture; order,
// caching, filters and cancellation on hand-built models.
#include "analysis/instances.h"

#include "aggregation_test_util.h"
#include "analysis/fibers.h"
#include "analysis/query_cache.h"
#include "analysis/thread_states.h"  // fixtures
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <set>
#include <thread>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::TestFunction;

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

const std::vector<TestFunction> kFns = {{"A", 0x1000, 0x100}, {"B", 0x1100, 0x100}};
constexpr uint64_t kA = 0x1000, kB = 0x1100;

FunctionKey fk(uint64_t start) {
  return FunctionKey{ModuleIdx(0), start};
}

trace::ScopeEventRec ev(uint64_t ts, uint32_t tid, uint8_t kind, StringIdx id = {}) {
  trace::ScopeEventRec e{};
  e.ts = ts;
  e.tid = tid;
  e.kind = kind;
  e.id = id;
  return e;
}

void expectMatchesStats(const std::vector<Instance>& inst, const Stats& s, const std::string& what) {
  ASSERT_EQ(inst.size(), s.count) << what;
  if (inst.empty()) return;
  uint64_t sum = 0, lo = UINT64_MAX, hi = 0;
  std::vector<uint32_t> hist;
  for (const Instance& i : inst) {
    ASSERT_LE(i.begin, i.end) << what;
    const uint64_t d = i.end - i.begin;
    sum += d;
    lo = std::min(lo, d);
    hi = std::max(hi, d);
    const uint32_t b = statsBucketOf(d);
    if (hist.size() <= b) hist.resize(b + 1, 0);
    ++hist[b];
  }
  EXPECT_EQ(sum, s.totalTicks) << what;
  EXPECT_EQ(static_cast<double>(lo), s.min) << what;
  EXPECT_EQ(static_cast<double>(hi), s.max) << what;
  EXPECT_EQ(hist, s.histogram) << what;
}

bool sortedByBegin(const std::vector<Instance>& v) {
  return std::is_sorted(v.begin(), v.end(), [](const Instance& a, const Instance& b) {
    if (a.begin != b.begin) return a.begin < b.begin;
    if (a.thread != b.thread) return a.thread < b.thread;
    return a.end < b.end;
  });
}

// ---- hand-built ------------------------------------------------------------------------------------------

struct Hand {
  HandModel hm{2};
  CallstackIdx a, b;
  std::unique_ptr<QueryContext> ctx;
  Hand() {
    a = hm.stack({kA});
    b = hm.stack({kB});
    hm.state(0, 0, 100000, ThreadState::Running);
    hm.state(1, 0, 100000, ThreadState::Running);
    // Thread 1 first (later thread index, earlier time) to check the global order by begin.
    for (uint64_t t = 1000; t < 1500; t += 100) hm.sample(1, t, a);
    for (uint64_t t = 5000; t < 5300; t += 100) hm.sample(0, t, a);
    hm.sample(0, 6000, b);
    for (uint64_t t = 7000; t < 7200; t += 100) hm.sample(0, t, a);
    hm.finish(kFns);
    ctx = std::make_unique<QueryContext>(hm.m, *hm.svc);
  }
};

TEST(Instances, SortedByBeginAcrossThreadsAndEqualToStats) {
  Hand h;
  auto inst = functionInstances(*h.ctx, {}, fk(kA));
  ASSERT_TRUE(inst.ok()) << inst.error().toString();
  ASSERT_EQ((*inst)->size(), 3u);
  EXPECT_TRUE(sortedByBegin(**inst));
  EXPECT_EQ((**inst)[0].thread, ThreadIdx(1));
  EXPECT_EQ((**inst)[0].begin, 1000u);
  EXPECT_EQ((**inst)[0].end, 1400u + 100u);
  EXPECT_EQ((**inst)[1].thread, ThreadIdx(0));
  EXPECT_EQ((**inst)[1].begin, 5000u);
  EXPECT_EQ((**inst)[1].end, 5300u);
  EXPECT_EQ((**inst)[2].begin, 7000u);
  auto st = functionStats(*h.ctx, {}, fk(kA));
  ASSERT_TRUE(st.ok());
  expectMatchesStats(**inst, *st, "A");
}

TEST(Instances, FiltersAndCache) {
  Hand h;
  Filter onlyT0;
  onlyT0.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  auto a = functionInstances(*h.ctx, onlyT0, fk(kA));
  ASSERT_TRUE(a.ok());
  EXPECT_EQ((*a)->size(), 2u);
  for (const Instance& i : **a) EXPECT_EQ(i.thread, ThreadIdx(0));

  Filter late;
  late.t0 = 6500;
  auto b = functionInstances(*h.ctx, late, fk(kA));
  ASSERT_TRUE(b.ok());
  ASSERT_EQ((*b)->size(), 1u);
  EXPECT_EQ((**b)[0].begin, 7000u);

  // Second call is a cache hit returning the same object.
  const uint64_t hits = h.ctx->cache().counters(QueryKind::Instances).hits;
  auto a2 = functionInstances(*h.ctx, onlyT0, fk(kA));
  ASSERT_TRUE(a2.ok());
  EXPECT_EQ(a2->get(), a->get());
  EXPECT_EQ(h.ctx->cache().counters(QueryKind::Instances).hits, hits + 1);

  // Empty result for a function that is not on any stack.
  auto none = functionInstances(*h.ctx, {}, fk(0x9000));
  ASSERT_TRUE(none.ok());
  EXPECT_TRUE((*none)->empty());
}

TEST(Instances, InvalidArguments) {
  Hand h;
  EXPECT_FALSE(scopeInstances(*h.ctx, {}, StringIdx::invalid()).ok());
  Filter bad;
  bad.threads = std::vector<ThreadIdx>{ThreadIdx(99)};
  EXPECT_FALSE(functionInstances(*h.ctx, bad, fk(kA)).ok());
}

TEST(Instances, CancelledIsNotCached) {
  Hand h;
  std::atomic<bool> flag{true};
  Cancel c{&flag};
  auto r = functionInstances(*h.ctx, {}, fk(kB), c);
  // A tiny scan may finish before the first cancellation check; if it reports cancellation nothing is cached.
  if (!r.ok()) {
    EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(h.ctx->cache().counters(QueryKind::Instances).hits, 0u);
  }
  EXPECT_TRUE(functionInstances(*h.ctx, {}, fk(kB)).ok());
}

TEST(ScopeInstances, MatchScopeStatsAndAreSorted) {
  HandModel hm(2);
  const StringIdx frame = hm.m.strings.intern("Frame");
  const StringIdx upd = hm.m.strings.intern("Update");
  hm.state(0, 0, 100000, ThreadState::Running);
  hm.state(1, 0, 100000, ThreadState::Running);
  auto& ev_ = hm.m.scopeEvents;
  ev_.push_back(ev(100, 101, trace::kScopeBegin, upd));
  ev_.push_back(ev(150, 101, trace::kScopeEnd));
  ev_.push_back(ev(200, 100, trace::kScopeBegin, frame));
  ev_.push_back(ev(210, 100, trace::kScopeBegin, upd));
  ev_.push_back(ev(260, 100, trace::kScopeEnd));
  ev_.push_back(ev(900, 100, trace::kScopeEnd));
  ev_.push_back(ev(1000, 100, trace::kScopeBegin, upd));  // unterminated
  ev_.push_back(ev(1200, 101, trace::kScopeEnd));         // orphan
  std::sort(ev_.begin(), ev_.end(), [](const auto& a, const auto& b) { return a.ts < b.ts; });
  hm.finish();
  QueryContext ctx(hm.m, *hm.svc);
  auto inst = scopeInstances(ctx, {}, upd);
  ASSERT_TRUE(inst.ok()) << inst.error().toString();
  ASSERT_EQ((*inst)->size(), 3u);
  EXPECT_TRUE(sortedByBegin(**inst));
  EXPECT_EQ((**inst)[0].thread, ThreadIdx(1));
  EXPECT_EQ((**inst)[0].begin, 100u);
  EXPECT_EQ((**inst)[0].end, 150u);
  EXPECT_EQ((**inst)[2].begin, 1000u);
  EXPECT_EQ((**inst)[2].end, hm.m.header.end_ts);
  auto st = scopeStats(ctx, {}, upd);
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(st->unterminated, 1u);
  expectMatchesStats(**inst, *st, "Update");
  // functionInstances with a scope key is the same list.
  auto viaFn = functionInstances(ctx, {}, FunctionKey::scope(upd));
  ASSERT_TRUE(viaFn.ok());
  EXPECT_EQ(viaFn->get(), inst->get());
  // The Begin selects the instance: a window starting after it excludes it.
  Filter f;
  f.t0 = 120;
  f.t1 = 1000;
  auto w = scopeInstances(ctx, f, upd);
  ASSERT_TRUE(w.ok());
  ASSERT_EQ((*w)->size(), 1u);
  EXPECT_EQ((**w)[0].begin, 210u);
}

// ---- fixtures --------------------------------------------------------------------------------------------

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

struct Loaded {
  explicit Loaded(const std::string& name) : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    auto r = model::TraceModel::load(generated(name + ".tachyon"));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(generated(name + ".symbols.json")).ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
  }
  model::TraceModel m;
  symbols::SymbolService svc;
};

// Runs fn(row index) for every index of [0, n) on all hardware threads; fn must be thread-safe and report
// failures through the returned strings (gtest assertions are not used off the main thread).
std::vector<std::string> parallelFor(size_t n, const std::function<std::string(size_t)>& fn) {
  std::vector<std::string> failures(n);
  std::atomic<size_t> next{0};
  const unsigned workers = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (unsigned w = 0; w < workers; ++w) {
    pool.emplace_back([&] {
      for (size_t i = next++; i < n; i = next++) failures[i] = fn(i);
    });
  }
  for (std::thread& t : pool) t.join();
  return failures;
}

// Empty when the instance list agrees with the statistics, else a description.
std::string compareToStats(const std::vector<Instance>& inst, const Stats& s) {
  if (inst.size() != s.count)
    return "count " + std::to_string(inst.size()) + " != " + std::to_string(s.count);
  if (inst.empty()) return {};
  uint64_t sum = 0, lo = UINT64_MAX, hi = 0;
  std::vector<uint32_t> hist;
  for (const Instance& i : inst) {
    if (i.begin > i.end) return "begin after end";
    const uint64_t d = i.end - i.begin;
    sum += d;
    lo = std::min(lo, d);
    hi = std::max(hi, d);
    const uint32_t b = statsBucketOf(d);
    if (hist.size() <= b) hist.resize(b + 1, 0);
    ++hist[b];
  }
  if (sum != s.totalTicks) return "total ticks differ";
  if (static_cast<double>(lo) != s.min) return "min differs";
  if (static_cast<double>(hi) != s.max) return "max differs";
  if (hist != s.histogram) return "histogram differs";
  if (!sortedByBegin(inst)) return "not sorted";
  return {};
}

TEST(InstancesFixture, EveryFunctionOfMediumEqualsItsStats) {
  Loaded l("medium");
  QueryContext ctx(l.m, l.svc);
  auto allRows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(allRows.ok());
  ASSERT_GT(allRows->size(), 100u);
  // Every function takes a sample scan per query (~20 s for the ~400 functions in a release build). Under a
  // sanitizer that is far beyond the test timeout, so only the first rows (by exclusive time) and every 60th
  // after them are checked there.
  std::vector<FunctionRow> rowStorage;
  const std::vector<FunctionRow>* rowsPtr = &*allRows;
  if (kSanitized) {
    for (size_t i = 0; i < allRows->size(); ++i)
      if (i < 8 || i % 60 == 0) rowStorage.push_back((*allRows)[i]);
    rowsPtr = &rowStorage;
  }
  const std::vector<FunctionRow>* rows = rowsPtr;

  const auto check = [&](const Filter& f, const char* what) {
    std::atomic<size_t> withInstances{0};
    const auto failures = parallelFor(rows->size(), [&](size_t i) -> std::string {
      const FunctionKey key = (*rows)[i].key;
      auto inst = functionInstances(ctx, f, key);
      auto st = functionStats(ctx, f, key);
      if (!inst.ok() || !st.ok()) return "query failed";
      if (!(*inst)->empty()) ++withInstances;
      if (f.threads) {
        for (const Instance& in : **inst)
          if (in.thread != (*f.threads)[0]) return "instance on another thread";
      }
      return compareToStats(**inst, *st);
    });
    for (size_t i = 0; i < failures.size(); ++i)
      EXPECT_TRUE(failures[i].empty())
          << what << ": " << functionKeyName(ctx, (*rows)[i].key) << ": " << failures[i];
    EXPECT_GT(withInstances.load(), 0u) << what;
  };
  check(Filter{}, "whole trace");

  // Under a thread and time filter as well.
  Filter f;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  f.t0 = l.m.header.start_ts + (l.m.header.end_ts - l.m.header.start_ts) / 4;
  f.t1 = l.m.header.start_ts + (l.m.header.end_ts - l.m.header.start_ts) / 2;
  check(f, "thread 0, second quarter");
}

TEST(InstancesFixture, EveryScopeOfScopesEqualsItsStats) {
  Loaded l("scopes");
  QueryContext ctx(l.m, l.svc);
  std::set<uint32_t> ids;
  for (const trace::ScopeEventRec& rec : l.m.scopeEvents) {
    const trace::ScopeEventRec e = rec;
    if (e.kind == trace::kScopeBegin) ids.insert(e.id.get());
  }
  ASSERT_FALSE(ids.empty());
  for (uint32_t id : ids) {
    const StringIdx sid(id);
    auto inst = scopeInstances(ctx, {}, sid);
    auto st = scopeStats(ctx, {}, sid);
    ASSERT_TRUE(inst.ok() && st.ok());
    expectMatchesStats(**inst, *st, std::string(l.m.strings.get(sid)));
    EXPECT_TRUE(sortedByBegin(**inst));
    EXPECT_GT((*inst)->size(), 0u);
  }
}

// WP-53 options: blending scopes into the stacks must not change the instances (or statistics) of sampled
// functions, and the fiber-aware pairing must give instance lists equal to the scope statistics.
TEST(InstancesFixture, BlendScopesDoesNotChangeSampledFunctionInstances) {
  Loaded l("scopes");
  QueryContext off(l.m, l.svc);
  AggregationOptions on;
  on.blendScopes = true;
  QueryContext blended(l.m, l.svc, on);
  auto rows = bottomUpFunctions(off, {});
  ASSERT_TRUE(rows.ok());
  size_t checked = 0, nonEmpty = 0;
  for (const FunctionRow& row : *rows) {
    if (row.key.isScope()) continue;
    auto a = functionInstances(off, {}, row.key);
    auto b = functionInstances(blended, {}, row.key);
    auto sa = functionStats(off, {}, row.key);
    auto sb = functionStats(blended, {}, row.key);
    ASSERT_TRUE(a.ok() && b.ok() && sa.ok() && sb.ok());
    EXPECT_EQ(**a, **b) << functionKeyName(off, row.key);
    EXPECT_EQ(sa->count, sb->count);
    EXPECT_EQ(sa->totalTicks, sb->totalTicks);
    EXPECT_EQ(sa->histogram, sb->histogram);
    EXPECT_EQ(compareToStats(**b, *sb), "");
    ++checked;
    if (!(*a)->empty()) ++nonEmpty;
  }
  EXPECT_GT(checked, 3u);
  EXPECT_GT(nonEmpty, 0u);
}

TEST(InstancesFixture, GroupByFiberScopeInstancesEqualScopeStats) {
  Loaded l("fibers");
  AggregationOptions opt;
  opt.groupByFiber = true;
  QueryContext ctx(l.m, l.svc, opt);
  QueryContext plain(l.m, l.svc);
  std::set<uint32_t> ids;
  for (const trace::ScopeEventRec& rec : l.m.scopeEvents) {
    const trace::ScopeEventRec e = rec;
    if (e.kind == trace::kScopeBegin) ids.insert(e.id.get());
  }
  ASSERT_FALSE(ids.empty());
  for (uint32_t id : ids) {
    const StringIdx sid(id);
    auto inst = scopeInstances(ctx, {}, sid);
    auto st = scopeStats(ctx, {}, sid);
    ASSERT_TRUE(inst.ok() && st.ok());
    EXPECT_EQ(compareToStats(**inst, *st), "") << l.m.strings.get(sid);
    // The pairing the statistics use is the fiber-aware one: the list is built from it.
    auto paired = pairedScopesFor(ctx, {});
    ASSERT_TRUE(paired.ok());
    size_t expected = 0;
    for (const auto& list : (*paired)->byThread)
      for (const ScopeInstance& si : list) expected += si.id == sid ? 1 : 0;
    EXPECT_EQ((*inst)->size(), expected) << l.m.strings.get(sid);
    // Without fiber grouping the cache entries are separate and still self-consistent.
    auto pi = scopeInstances(plain, {}, sid);
    auto ps = scopeStats(plain, {}, sid);
    ASSERT_TRUE(pi.ok() && ps.ok());
    EXPECT_EQ(compareToStats(**pi, *ps), "");
  }
}

}  // namespace
}  // namespace tachyon::analysis
