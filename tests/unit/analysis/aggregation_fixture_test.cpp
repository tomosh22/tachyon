// topDownTree / bottomUpFunctions / butterfly against the tracegen fixtures' expected.json.
#include "analysis/query.h"

#include "analysis/thread_states.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace tachyon::analysis {
namespace {

using model::TraceModel;

const char* const kFixtures[] = {"tiny",   "medium",    "scopes",  "fibers",
                                 "frames", "recursion", "partial", "partial_torn"};

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

nlohmann::json readJson(const std::string& file) {
  std::ifstream f(generated(file));
  return nlohmann::json::parse(f);
}

// A loaded fixture with thread states built and symbols resolved from the fixture's symbols.json.
struct Loaded {
  explicit Loaded(const std::string& name)
      : expected(readJson(name + ".expected.json")),
        svc(std::vector<std::string>{}, std::filesystem::path{}) {
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

FunctionKey keyOf(const nlohmann::json& fn) {
  return FunctionKey{ModuleIdx(fn["module"].get<uint32_t>()), fn["start_rva"].get<uint64_t>()};
}

std::map<FunctionKey, FunctionRow> byKey(const std::vector<FunctionRow>& rows) {
  std::map<FunctionKey, FunctionRow> out;
  for (const FunctionRow& r : rows) out[r.key] = r;
  return out;
}

class AggregationFixture : public ::testing::TestWithParam<const char*> {};

TEST_P(AggregationFixture, BottomUpMatchesExpectedFunctions) {
  Loaded l(GetParam());
  QueryContext ctx(l.m, l.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok()) << rows.error().toString();
  const auto got = byKey(*rows);
  size_t expectedWithSamples = 0;
  for (const auto& fn : l.expected["functions"]) {
    if (fn["inclusive_samples"].get<uint64_t>() == 0) continue;
    ++expectedWithSamples;
    const FunctionKey k = keyOf(fn);
    const auto it = got.find(k);
    ASSERT_NE(it, got.end()) << fn["name"];
    EXPECT_EQ(it->second.inclusiveTicks, fn["inclusive_ticks"].get<uint64_t>()) << fn["name"];
    EXPECT_EQ(it->second.exclusiveTicks, fn["exclusive_ticks"].get<uint64_t>()) << fn["name"];
    EXPECT_EQ(it->second.instances, fn["inclusive_samples"].get<uint32_t>()) << fn["name"];
  }
  EXPECT_EQ(got.size(), expectedWithSamples);
}

TEST_P(AggregationFixture, PerThreadFilterMatchesExpected) {
  Loaded l(GetParam());
  QueryContext ctx(l.m, l.svc);
  for (const auto& fn : l.expected["functions"]) {
    for (const auto& pt : fn["per_thread"]) {
      const auto it = l.m.tidToThread.find(pt["tid"].get<uint32_t>());
      ASSERT_NE(it, l.m.tidToThread.end());
      Filter f;
      f.threads = std::vector<ThreadIdx>{it->second};
      auto b = butterfly(ctx, f, keyOf(fn));
      ASSERT_TRUE(b.ok());
      EXPECT_EQ(b->self.inclusiveTicks, pt["inclusive_ticks"].get<uint64_t>())
          << fn["name"] << " tid " << pt["tid"];
      EXPECT_EQ(b->self.exclusiveTicks, pt["exclusive_ticks"].get<uint64_t>())
          << fn["name"] << " tid " << pt["tid"];
    }
  }
}

TEST_P(AggregationFixture, TopDownRootsMatchThreadSampleTotals) {
  Loaded l(GetParam());
  QueryContext ctx(l.m, l.svc);
  uint64_t totalTicks = 0, totalSamples = 0;
  for (const auto& et : l.expected["threads"]) {
    Filter f;
    f.threads = std::vector<ThreadIdx>{l.m.tidToThread.at(et["tid"].get<uint32_t>())};
    auto tree = topDownTree(ctx, f);
    ASSERT_TRUE(tree.ok());
    EXPECT_EQ(tree->inclusiveTicks, et["sample_ticks"].get<uint64_t>()) << "tid " << et["tid"];
    EXPECT_EQ(tree->sampleCount, et["samples"].get<uint32_t>()) << "tid " << et["tid"];
    totalTicks += tree->inclusiveTicks;
    totalSamples += tree->sampleCount;
  }
  auto all = topDownTree(ctx, {});
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all->inclusiveTicks, totalTicks);
  EXPECT_EQ(all->sampleCount, totalSamples);
}

// Every node: inclusive = exclusive + sum(children); sample counts likewise; functions' exclusive time equals
// the sum of exclusive over all their tree nodes; the outermost nodes of a function sum to its inclusive
// time.
void walk(const NodeAgg& n, std::vector<FunctionKey>& path, std::map<FunctionKey, uint64_t>& excl,
          std::map<FunctionKey, uint64_t>& outerIncl, size_t& nodes) {
  ++nodes;
  uint64_t ci = 0, cs = 0;
  for (const NodeAgg& c : n.children) {
    ci += c.inclusiveTicks;
    cs += c.sampleCount;
  }
  EXPECT_EQ(n.inclusiveTicks, n.exclusiveTicks + ci);
  EXPECT_LE(cs, n.sampleCount);
  if (!path.empty()) {
    excl[n.key] += n.exclusiveTicks;
    bool nested = false;
    for (size_t i = 0; i + 1 < path.size(); ++i) nested |= path[i] == n.key;
    if (!nested) outerIncl[n.key] += n.inclusiveTicks;
  }
  for (const NodeAgg& c : n.children) {
    path.push_back(c.key);
    walk(c, path, excl, outerIncl, nodes);
    path.pop_back();
  }
}

TEST_P(AggregationFixture, TopDownIsConsistentWithBottomUp) {
  Loaded l(GetParam());
  QueryContext ctx(l.m, l.svc);
  auto tree = topDownTree(ctx, {});
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(tree.ok() && rows.ok());
  std::map<FunctionKey, uint64_t> excl, outer;
  std::vector<FunctionKey> path;
  size_t nodes = 0;
  walk(*tree, path, excl, outer, nodes);
  EXPECT_GT(nodes, 1u);
  for (const FunctionRow& r : *rows) {
    EXPECT_EQ(excl[r.key], r.exclusiveTicks) << functionKeyName(ctx, r.key);
    EXPECT_EQ(outer[r.key], r.inclusiveTicks) << functionKeyName(ctx, r.key);
  }
}

TEST_P(AggregationFixture, ButterflyCallersSumToSelf) {
  Loaded l(GetParam());
  QueryContext ctx(l.m, l.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  for (const FunctionRow& r : *rows) {
    auto b = butterfly(ctx, {}, r.key);
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b->self.inclusiveTicks, r.inclusiveTicks);
    EXPECT_EQ(b->self.exclusiveTicks, r.exclusiveTicks);
    EXPECT_EQ(b->self.instances, r.instances);
    uint64_t incl = 0, excl = 0, inst = 0, calleeIncl = 0;
    for (const FunctionRow& c : b->callers) {
      incl += c.inclusiveTicks;
      excl += c.exclusiveTicks;
      inst += c.instances;
    }
    for (const FunctionRow& c : b->callees) calleeIncl += c.inclusiveTicks;
    EXPECT_EQ(incl, r.inclusiveTicks) << functionKeyName(ctx, r.key);
    EXPECT_EQ(excl, r.exclusiveTicks) << functionKeyName(ctx, r.key);
    EXPECT_EQ(inst, r.instances) << functionKeyName(ctx, r.key);
    EXPECT_LE(calleeIncl, r.inclusiveTicks);
  }
}

TEST_P(AggregationFixture, TimeSplitIsAdditive) {
  Loaded l(GetParam());
  QueryContext ctx(l.m, l.svc);
  const uint64_t mid = l.m.header.start_ts + (l.m.header.end_ts - l.m.header.start_ts) / 3;
  Filter a, b;
  a.t1 = mid;
  b.t0 = mid;
  auto all = bottomUpFunctions(ctx, {});
  auto ra = bottomUpFunctions(ctx, a);
  auto rb = bottomUpFunctions(ctx, b);
  ASSERT_TRUE(all.ok() && ra.ok() && rb.ok());
  const auto ma = byKey(*ra), mb = byKey(*rb);
  for (const FunctionRow& r : *all) {
    uint64_t incl = 0, excl = 0;
    if (auto it = ma.find(r.key); it != ma.end())
      incl += it->second.inclusiveTicks, excl += it->second.exclusiveTicks;
    if (auto it = mb.find(r.key); it != mb.end())
      incl += it->second.inclusiveTicks, excl += it->second.exclusiveTicks;
    EXPECT_EQ(incl, r.inclusiveTicks);
    EXPECT_EQ(excl, r.exclusiveTicks);
  }
}

TEST_P(AggregationFixture, DeterministicAcrossWorkerCounts) {
  Loaded l(GetParam());
  AggregationOptions one, many;
  one.threads = 1;
  many.threads = 4;
  QueryContext c1(l.m, l.svc, one), c4(l.m, l.svc, many);
  auto a = bottomUpFunctions(c1, {});
  auto b = bottomUpFunctions(c4, {});
  ASSERT_TRUE(a.ok() && b.ok());
  ASSERT_EQ(a->size(), b->size());
  for (size_t i = 0; i < a->size(); ++i) {
    EXPECT_EQ((*a)[i].key, (*b)[i].key);
    EXPECT_EQ((*a)[i].inclusiveTicks, (*b)[i].inclusiveTicks);
    EXPECT_EQ((*a)[i].exclusiveTicks, (*b)[i].exclusiveTicks);
  }
}

INSTANTIATE_TEST_SUITE_P(Fixtures, AggregationFixture, ::testing::ValuesIn(kFixtures),
                         [](const auto& info) { return std::string(info.param); });

TEST(AggregationRecursion, RecursiveFunctionIsCountedOncePerSample) {
  Loaded l("recursion");
  QueryContext ctx(l.m, l.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  const uint64_t total = [&] {
    uint64_t t = 0;
    for (const auto& et : l.expected["threads"]) t += et["sample_ticks"].get<uint64_t>();
    return t;
  }();
  bool sawRecursive = false;
  for (const auto& fn : l.expected["functions"]) {
    const auto got = byKey(*rows).at(keyOf(fn));
    EXPECT_LE(got.inclusiveTicks, total);
    if (fn["name"].get<std::string>() == "recursion!fib") {
      sawRecursive = true;
      EXPECT_EQ(got.inclusiveTicks, 1024164009u);
      EXPECT_EQ(got.exclusiveTicks, 824039009u);
      EXPECT_EQ(got.instances, 8237u);
    }
  }
  EXPECT_TRUE(sawRecursive);
}

TEST(AggregationNames, ResolvedFunctionsAreNamedFromSymbols) {
  Loaded l("recursion");
  QueryContext ctx(l.m, l.svc);
  for (const auto& fn : l.expected["functions"]) {
    if (fn["kind"] != "function") continue;
    EXPECT_EQ(functionKeyName(ctx, keyOf(fn)), fn["name"].get<std::string>());
  }
}

TEST(AggregationTiny, UnmappedFramesKeepTheirAbsoluteAddress) {
  Loaded l("tiny");
  QueryContext ctx(l.m, l.svc);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  bool unmapped = false;
  for (const FunctionRow& r : *rows) {
    if (!r.key.isUnmapped()) continue;
    unmapped = true;
    EXPECT_EQ(functionKeyName(ctx, r.key).rfind("<unmapped>!0x", 0), 0u);
  }
  EXPECT_TRUE(unmapped);
}

TEST(AggregationInline, ExpansionAddsInlineFunctionsAndKeepsTotals) {
  Loaded l("medium");
  AggregationOptions o;
  o.expandInline = true;
  QueryContext plain(l.m, l.svc), expanded(l.m, l.svc, o);
  auto a = bottomUpFunctions(plain, {});
  auto b = bottomUpFunctions(expanded, {});
  ASSERT_TRUE(a.ok() && b.ok());
  const auto ma = byKey(*a), mb = byKey(*b);
  uint64_t exclA = 0, exclB = 0, inlineRows = 0, inlineExcl = 0;
  for (const FunctionRow& r : *a) exclA += r.exclusiveTicks;
  for (const FunctionRow& r : *b) {
    exclB += r.exclusiveTicks;
    if (r.key.isInline()) {
      ++inlineRows;
      inlineExcl += r.exclusiveTicks;
      EXPECT_FALSE(functionKeyName(expanded, r.key).empty());
      EXPECT_EQ(functionKeyName(expanded, r.key).find("0x"), std::string::npos) << "inline name resolved";
    } else {
      // Physical functions keep their inclusive time (they stay in the stack); only exclusive time may move
      // to inline frames.
      const auto it = ma.find(r.key);
      ASSERT_NE(it, ma.end());
      EXPECT_EQ(it->second.inclusiveTicks, r.inclusiveTicks);
      EXPECT_LE(r.exclusiveTicks, it->second.exclusiveTicks);
    }
  }
  EXPECT_EQ(exclA, exclB);
  EXPECT_GT(inlineRows, 0u);
  EXPECT_GT(inlineExcl, 0u);
  EXPECT_EQ(mb.size(), ma.size() + inlineRows);
}

}  // namespace
}  // namespace tachyon::analysis
