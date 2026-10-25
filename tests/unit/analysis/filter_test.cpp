// Filters (time range, threads, subtree, function) against a brute-force evaluation on the `tiny` fixture,
// on `medium`, and on hand-built models.
#include "analysis/query.h"

#include "aggregation_test_util.h"
#include "analysis/attribution.h"
#include "analysis/thread_states.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <set>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using model::TraceModel;
using testutil::HandModel;

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

struct Loaded {
  explicit Loaded(const std::string& name) : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    auto r = TraceModel::load(generated(name + ".tachyon"));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(generated(name + ".symbols.json")).ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
  }
  TraceModel m;
  symbols::SymbolService svc;
};

// ---- brute force: one pass over every sample, no binary search, no cache ----

struct Brute {
  std::map<FunctionKey, FunctionRow> rows;
  uint64_t totalTicks = 0;
  uint64_t totalSamples = 0;
};

Brute bruteForce(const QueryContext& ctx, const Filter& f, std::optional<FunctionKey> fn) {
  const TraceModel& m = ctx.model();
  Brute out;
  for (uint32_t t = 0; t < m.threads.size(); ++t) {
    if (f.threads && std::find(f.threads->begin(), f.threads->end(), ThreadIdx(t)) == f.threads->end())
      continue;
    const model::SampleColumns& cols = m.samplesByThread[t];
    for (size_t i = 0; i < cols.ts.size(); ++i) {
      const uint64_t ts = cols.ts[i];
      if (ts < f.t0 || ts >= f.t1) continue;
      // Weight: rule 1.
      uint64_t weight = 0;
      const model::ThreadStateInterval* iv = stateAt(m, ThreadIdx(t), ts);
      if (iv != nullptr && iv->state == ThreadState::Running)
        weight = std::min(m.sampleIntervalTicks, iv->end - ts);
      CallstackIdx leaf = cols.callstack[i];
      if (!leaf.valid() || leaf.get() >= m.nodes.size()) leaf = CallstackIdx(0);
      const bool kernelLeaf = (cols.flags[i] & trace::kSampleFlagKernelLeaf) != 0;
      std::vector<uint32_t> chain;  // leaf first
      for (uint32_t n = leaf.get(); n != 0; n = m.nodes[n].parent.get()) chain.push_back(n);
      size_t depth = chain.size();
      if (f.subtreeRoot && f.subtreeRoot->valid() && f.subtreeRoot->get() != 0) {
        const auto it = std::find(chain.begin(), chain.end(), f.subtreeRoot->get());
        if (it == chain.end()) continue;
        depth = static_cast<size_t>(it - chain.begin()) + 1;  // keep the subtree node and everything below
      }
      std::vector<FunctionKey> keys;  // leaf first
      for (size_t p = 0; p < depth; ++p) keys.push_back(frameKeyAt(ctx, leaf, p, kernelLeaf));
      if (fn && std::find(keys.begin(), keys.end(), *fn) == keys.end()) continue;
      out.totalTicks += weight;
      ++out.totalSamples;
      std::set<FunctionKey> seen;
      for (const FunctionKey& k : keys) {
        if (!seen.insert(k).second) continue;
        FunctionRow& r = out.rows[k];
        r.key = k;
        r.inclusiveTicks += weight;
        ++r.instances;
      }
      if (!keys.empty()) {
        FunctionRow& r = out.rows[keys.front()];
        r.exclusiveTicks += weight;
      }
    }
  }
  return out;
}

void expectMatches(const QueryContext& ctx, const Filter& filter, const std::string& what) {
  std::optional<FunctionKey> fn;
  if (filter.function) fn = ctx.functionKey(*filter.function);
  const Brute want = bruteForce(ctx, filter, fn);
  auto rows = bottomUpFunctions(ctx, filter);
  ASSERT_TRUE(rows.ok()) << what << ": " << rows.error().toString();
  ASSERT_EQ(rows->size(), want.rows.size()) << what;
  for (const FunctionRow& r : *rows) {
    const auto it = want.rows.find(r.key);
    ASSERT_NE(it, want.rows.end()) << what;
    EXPECT_EQ(r.inclusiveTicks, it->second.inclusiveTicks) << what;
    EXPECT_EQ(r.exclusiveTicks, it->second.exclusiveTicks) << what;
    EXPECT_EQ(r.instances, it->second.instances) << what;
  }
  // The tree sees the same samples. (The empty-stack samples are in the root of the tree only; a subtree
  // filter returns the subtree root, whose totals are the same.)
  auto tree = topDownTree(ctx, filter);
  ASSERT_TRUE(tree.ok()) << what;
  EXPECT_EQ(tree->inclusiveTicks, want.totalTicks) << what;
  EXPECT_EQ(tree->sampleCount, want.totalSamples) << what;
  // Butterfly of the heaviest function.
  if (!rows->empty()) {
    const FunctionKey top = (*rows)[0].key;
    auto bf = butterfly(ctx, filter, top);
    ASSERT_TRUE(bf.ok()) << what;
    EXPECT_EQ(bf->self.inclusiveTicks, want.rows.at(top).inclusiveTicks) << what;
    EXPECT_EQ(bf->self.exclusiveTicks, want.rows.at(top).exclusiveTicks) << what;
    EXPECT_EQ(bf->self.instances, want.rows.at(top).instances) << what;
  }
}

// A deterministic spread of filters over the model.
std::vector<Filter> makeFilters(QueryContext& ctx, uint32_t seed, int count) {
  const TraceModel& m = ctx.model();
  std::mt19937_64 rng(seed);
  const uint64_t lo = m.header.start_ts, hi = m.header.end_ts;
  auto anyTs = [&] { return lo + rng() % (hi - lo + 1); };
  // Callstack nodes of real samples, for subtree roots.
  std::vector<CallstackIdx> leaves;
  for (const auto& cols : m.samplesByThread)
    for (size_t i = 0; i < cols.callstack.size(); i += 1 + cols.callstack.size() / 64)
      if (cols.callstack[i].valid() && cols.callstack[i].get() != 0) leaves.push_back(cols.callstack[i]);
  auto rows = bottomUpFunctions(ctx, {});
  EXPECT_TRUE(rows.ok());
  std::vector<Filter> out;
  for (int i = 0; i < count; ++i) {
    Filter f;
    switch (i % 8) {
      case 0: break;  // everything
      case 1:
        f.t0 = anyTs();
        f.t1 = anyTs();
        break;  // maybe empty
      case 2:
        f.t0 = lo + (hi - lo) / 4;
        f.t1 = lo + (hi - lo) / 2;
        break;
      case 3: {
        f.threads.emplace();
        for (uint32_t t = 0; t < m.threads.size(); ++t)
          if (rng() % 2) f.threads->push_back(ThreadIdx(t));
        break;
      }
      case 4: {
        f.t0 = anyTs();
        f.t1 = f.t0 + (hi - lo) / 10;
        f.threads = std::vector<ThreadIdx>{ThreadIdx(static_cast<uint32_t>(rng() % m.threads.size()))};
        break;
      }
      case 5:
      case 6: {
        if (leaves.empty()) break;
        CallstackIdx n = leaves[rng() % leaves.size()];
        for (uint64_t up = rng() % 4; up > 0 && m.nodes[n.get()].parent.get() != 0; --up)
          n = m.nodes[n.get()].parent;
        f.subtreeRoot = n;
        if (i % 8 == 6) f.t1 = lo + (hi - lo) / 2;
        break;
      }
      case 7: {
        if (!rows.ok() || rows->empty()) break;
        const FunctionKey k = (*rows)[rng() % std::min<size_t>(rows->size(), 12)].key;
        f.function = ctx.functionIdx(k);
        if (rng() % 2) f.t0 = anyTs();
        break;
      }
    }
    out.push_back(f);
  }
  return out;
}

TEST(FilterTest, TinyMatchesBruteForce) {
  Loaded l("tiny");
  QueryContext ctx(l.m, l.svc);
  const std::vector<Filter> filters = makeFilters(ctx, 7, 64);
  int n = 0;
  for (const Filter& f : filters) expectMatches(ctx, f, "tiny filter #" + std::to_string(n++));
}

TEST(FilterTest, TinyMatchesBruteForceWithEveryFilterCombined) {
  Loaded l("tiny");
  QueryContext ctx(l.m, l.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  ASSERT_GE(rows->size(), 3u);
  std::mt19937_64 rng(99);
  const uint64_t lo = l.m.header.start_ts, hi = l.m.header.end_ts;
  std::vector<CallstackIdx> leaves;
  for (const auto& cols : l.m.samplesByThread)
    for (size_t i = 0; i < cols.callstack.size(); i += 17)
      if (cols.callstack[i].get() != 0) leaves.push_back(cols.callstack[i]);
  for (int i = 0; i < 24; ++i) {
    Filter f;
    f.t0 = lo + rng() % (hi - lo) / 2;
    f.t1 = f.t0 + (hi - lo) / 2;
    f.threads = std::vector<ThreadIdx>{ThreadIdx(0), ThreadIdx(1)};
    if (i % 2) f.threads = std::vector<ThreadIdx>{ThreadIdx(static_cast<uint32_t>(i / 2 % 2))};
    f.subtreeRoot = leaves[rng() % leaves.size()];
    f.function = ctx.functionIdx((*rows)[rng() % rows->size()].key);
    expectMatches(ctx, f, "combined #" + std::to_string(i));
  }
}

TEST(FilterTest, MediumMatchesBruteForce) {
  Loaded l("medium");
  QueryContext ctx(l.m, l.svc);
  // The brute force is quadratic in the stack depth per sample: keep every range to ~2.5% of the trace.
  std::vector<Filter> filters = makeFilters(ctx, 3, 16);
  const uint64_t span = l.m.header.end_ts - l.m.header.start_ts;
  int n = 0;
  for (Filter& f : filters) {
    if (f.t1 == UINT64_MAX) f.t1 = std::max<uint64_t>(f.t0, l.m.header.start_ts) + span / 40;
    if (f.t1 > f.t0 && f.t1 - f.t0 > span / 40) f.t1 = f.t0 + span / 40;
    expectMatches(ctx, f, "medium filter #" + std::to_string(n++));
  }
}

// ---- time range edge cases on a hand-built model ----

struct Hand {
  HandModel h{2};
  CallstackIdx s1, s2;
  Hand() {
    h.state(0, 0, 100000, ThreadState::Running);
    h.state(1, 0, 100000, ThreadState::Running);
    s1 = h.stack({0x1000, 0x2000});
    s2 = h.stack({0x1000, 0x3000});
    for (uint64_t i = 0; i < 100; ++i) {
      h.sample(0, i * 100, s1);
      h.sample(1, i * 100 + 50, s2);
    }
    h.finish();
  }
};

TEST(FilterTest, TimeRangeIsHalfOpenOnSampleTimestamps) {
  Hand hand;
  QueryContext ctx(hand.h.m, *hand.h.svc);
  Filter f;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(0)};
  f.t0 = 1000;  // sample at exactly t0 is in
  f.t1 = 2000;  // sample at exactly t1 is out
  auto rows = bottomUpFunctions(ctx, f);
  ASSERT_TRUE(rows.ok());
  uint32_t instances = 0;
  for (const auto& r : *rows)
    if (r.key == FunctionKey{ModuleIdx(0), 0x2000}) instances = r.instances;
  EXPECT_EQ(instances, 10u);
  // A weight is not re-clipped to t1: all 10 samples weigh a full interval.
  auto tree = topDownTree(ctx, f);
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(tree->inclusiveTicks, 10u * HandModel::kInterval);
  // Beyond the data, before the data, and degenerate ranges.
  Filter after;
  after.t0 = 90000;
  EXPECT_EQ(topDownTree(ctx, after)->inclusiveTicks, 0u);
  Filter empty;
  empty.t0 = 500;
  empty.t1 = 500;
  EXPECT_EQ(topDownTree(ctx, empty)->inclusiveTicks, 0u);
  EXPECT_TRUE(bottomUpFunctions(ctx, empty)->empty());
  Filter single;
  single.t0 = 1000;
  single.t1 = 1001;
  EXPECT_EQ(topDownTree(ctx, single)->sampleCount, 1u);
}

TEST(FilterTest, TimeRangeSplitsAddUp) {
  Hand hand;
  QueryContext ctx(hand.h.m, *hand.h.svc);
  uint64_t total = 0;
  uint32_t samples = 0;
  for (uint64_t t = 0; t < 10000; t += 777) {
    Filter f;
    f.t0 = t;
    f.t1 = t + 777;
    auto tree = topDownTree(ctx, f);
    ASSERT_TRUE(tree.ok());
    total += tree->inclusiveTicks;
    samples += tree->sampleCount;
  }
  auto whole = topDownTree(ctx, {});
  ASSERT_TRUE(whole.ok());
  EXPECT_EQ(total, whole->inclusiveTicks);
  EXPECT_EQ(samples, whole->sampleCount);
  EXPECT_EQ(samples, 200u);
}

TEST(FilterTest, ThreadFilterSelectsAndRejectsDuplicates) {
  Hand hand;
  QueryContext ctx(hand.h.m, *hand.h.svc);
  Filter one;
  one.threads = std::vector<ThreadIdx>{ThreadIdx(1), ThreadIdx(1)};
  auto tree = topDownTree(ctx, one);
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(tree->sampleCount, 100u);  // duplicates in the list do not double count
  Filter none;
  none.threads = std::vector<ThreadIdx>{};
  EXPECT_EQ(topDownTree(ctx, none)->sampleCount, 0u);
}

TEST(FilterTest, FunctionFilterKeepsOnlyStacksContainingTheFunction) {
  Hand hand;
  QueryContext ctx(hand.h.m, *hand.h.svc);
  const FunctionKey f2{ModuleIdx(0), 0x2000}, f3{ModuleIdx(0), 0x3000}, fmain{ModuleIdx(0), 0x1000};
  Filter f;
  f.function = ctx.functionIdx(f2);
  auto tree = topDownTree(ctx, f);
  ASSERT_TRUE(tree.ok());
  EXPECT_EQ(tree->sampleCount, 100u);
  ASSERT_EQ(tree->children.size(), 1u);
  EXPECT_EQ(tree->children[0].key, fmain);
  ASSERT_EQ(tree->children[0].children.size(), 1u);
  EXPECT_EQ(tree->children[0].children[0].key, f2);
  auto rows = bottomUpFunctions(ctx, f);
  ASSERT_TRUE(rows.ok());
  EXPECT_EQ(rows->size(), 2u);  // main and f2; f3 is gone
  auto bf = butterfly(ctx, f, fmain);
  ASSERT_TRUE(bf.ok());
  EXPECT_EQ(bf->self.instances, 100u);
  ASSERT_EQ(bf->callees.size(), 1u);
  EXPECT_EQ(bf->callees[0].key, f2);
  // The function everyone has keeps everything; combined with a thread filter it composes.
  Filter all;
  all.function = ctx.functionIdx(fmain);
  EXPECT_EQ(topDownTree(ctx, all)->sampleCount, 200u);
  all.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  EXPECT_EQ(topDownTree(ctx, all)->sampleCount, 100u);
  // A function no stack contains leaves an empty result.
  Filter missing;
  missing.function = ctx.functionIdx(FunctionKey{ModuleIdx(0), 0x9999});
  EXPECT_EQ(topDownTree(ctx, missing)->sampleCount, 0u);
  EXPECT_TRUE(bottomUpFunctions(ctx, missing)->empty());
  (void)f3;
}

TEST(FilterTest, FunctionFilterIsTestedAfterTheSubtreeCut) {
  HandModel h(1);
  h.state(0, 0, 100000, ThreadState::Running);
  const CallstackIdx a = h.stack({0x1000, 0x2000});
  const CallstackIdx b = h.push(a, 0x3000);                  // main > a > b
  const CallstackIdx c = h.push(h.stack({0x1000}), 0x3000);  // main > b
  h.sample(0, 0, b);
  h.sample(0, 100, c);
  h.finish();
  QueryContext ctx(h.m, *h.svc);
  Filter f;
  f.subtreeRoot = a;  // the cut keeps a and b, not main
  f.function = ctx.functionIdx(FunctionKey{ModuleIdx(0), 0x1000});
  EXPECT_EQ(topDownTree(ctx, f)->sampleCount, 0u);  // main is above the cut
  f.function = ctx.functionIdx(FunctionKey{ModuleIdx(0), 0x3000});
  EXPECT_EQ(topDownTree(ctx, f)->sampleCount, 1u);
}

TEST(FilterTest, FunctionFilterWithRecursionAndKernelGrouping) {
  HandModel h(1);
  h.state(0, 0, 100000, ThreadState::Running);
  const CallstackIdx main = h.stack({0x1000});
  const CallstackIdx rec = h.push(h.push(h.push(main, 0x2000), 0x2000), 0x2000);
  const CallstackIdx k = h.pushAbs(h.pushAbs(main, 0xFFFF800000001000ull), 0xFFFF800000002000ull);
  h.sample(0, 0, rec);
  h.sample(0, 100, k);
  h.sample(0, 200, main);
  h.finish();
  AggregationOptions opts;
  opts.kernelFrames = KernelFrames::Group;
  QueryContext ctx(h.m, *h.svc, opts);
  Filter f;
  f.function = ctx.functionIdx(FunctionKey{ModuleIdx(0), 0x2000});
  auto rows = bottomUpFunctions(ctx, f);
  ASSERT_TRUE(rows.ok());
  for (const auto& r : *rows)
    if (r.key.value == 0x2000) EXPECT_EQ(r.instances, 1u);  // recursion counted once
  EXPECT_EQ(topDownTree(ctx, f)->sampleCount, 1u);
  f.function = ctx.functionIdx(kKernelKey);
  EXPECT_EQ(topDownTree(ctx, f)->sampleCount, 1u);  // the merged kernel frame is a function too
}

TEST(FilterTest, FunctionHandlesAreStableAndForeignOnesRejected) {
  Hand hand;
  QueryContext ctx(hand.h.m, *hand.h.svc);
  QueryContext other(hand.h.m, *hand.h.svc);
  const FunctionKey k{ModuleIdx(0), 0x2000};
  const FunctionIdx a = ctx.functionIdx(k);
  EXPECT_EQ(ctx.functionIdx(k), a);
  EXPECT_EQ(*ctx.functionKey(a), k);
  Filter f;
  f.function = a;
  EXPECT_TRUE(topDownTree(ctx, f).ok());
  EXPECT_EQ(topDownTree(other, f).error().code, ErrorCode::InvalidArgument);  // never issued by `other`
  f.function = FunctionIdx::invalid();
  EXPECT_EQ(topDownTree(ctx, f).error().code, ErrorCode::InvalidArgument);
  ctx.clearCache();  // handles survive clearCache
  f.function = a;
  EXPECT_TRUE(topDownTree(ctx, f).ok());
}

}  // namespace
}  // namespace tachyon::analysis
