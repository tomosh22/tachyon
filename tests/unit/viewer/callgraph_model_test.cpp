// WP-43: the pure logic of the Call Graph panel: iterative flattening with expand state, hot path, expand
// all, percent modes, pie slices, filter mapping, node lookup and the stale-result guard.
#include "viewer/callgraph/callgraph_model.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::callgraph;
using analysis::NodeAgg;
using symbols::FunctionKey;

namespace {

FunctionKey fk(uint64_t v) {
  return FunctionKey{ModuleIdx(0), v};
}

NodeAgg node(uint64_t key, uint64_t incl, uint64_t excl, uint32_t samples,
             std::vector<NodeAgg> children = {}) {
  NodeAgg n;
  n.key = fk(key);
  n.inclusiveTicks = incl;
  n.exclusiveTicks = excl;
  n.sampleCount = samples;
  n.children = std::move(children);
  return n;
}

// root(100) -> a(60) -> {a1(40), a2(20)} ; b(40) -> b1(40) -> b2(40)
NodeAgg sampleTree() {
  NodeAgg root;
  root.key = analysis::kRootKey;
  root.inclusiveTicks = 100;
  root.sampleCount = 10;
  std::vector<NodeAgg> a;
  a.push_back(node(11, 40, 40, 4));
  a.push_back(node(12, 20, 20, 2));
  root.children.push_back(node(1, 60, 0, 6, std::move(a)));
  std::vector<NodeAgg> b2;
  b2.push_back(node(21, 40, 40, 4));
  std::vector<NodeAgg> b1;
  b1.push_back(node(20, 40, 0, 4, std::move(b2)));
  root.children.push_back(node(2, 40, 0, 4, std::move(b1)));
  return root;
}

// A chain of `depth` nodes below the root, built bottom-up without recursion.
NodeAgg deepChain(size_t depth, uint64_t ticks = 100) {
  NodeAgg cur = node(depth, ticks, ticks, 1);
  for (size_t i = depth; i-- > 1;) {
    NodeAgg parent = node(i, ticks, 0, 1);
    parent.children.push_back(std::move(cur));
    cur = std::move(parent);
  }
  NodeAgg root;
  root.key = analysis::kRootKey;
  root.inclusiveTicks = ticks;
  root.sampleCount = 1;
  root.children.push_back(std::move(cur));
  return root;
}

}  // namespace

TEST(CallGraphModel, IdsAreStableAndDistinguishPaths) {
  EXPECT_EQ(childId(kRootId, fk(1)), childId(kRootId, fk(1)));
  EXPECT_NE(childId(kRootId, fk(1)), childId(kRootId, fk(2)));
  EXPECT_NE(childId(childId(kRootId, fk(1)), fk(2)), childId(childId(kRootId, fk(2)), fk(1)));
  const std::vector<FunctionKey> path{fk(1), fk(2), fk(3)};
  EXPECT_EQ(pathId(path), childId(childId(childId(kRootId, fk(1)), fk(2)), fk(3)));
  EXPECT_EQ(pathId({}), kRootId);
}

TEST(CallGraphModel, FlattenShowsOnlyChildrenOfExpandedNodes) {
  const NodeAgg root = sampleTree();
  ExpandSet expanded;
  std::vector<Row> rows;
  EXPECT_FALSE(flattenVisible(root, expanded, kMaxVisibleRows, rows));
  ASSERT_EQ(rows.size(), 1u);  // collapsed root
  EXPECT_TRUE(rows[0].hasChildren);
  EXPECT_FALSE(rows[0].expanded);
  EXPECT_EQ(rows[0].parentRow, UINT32_MAX);

  expanded.insert(kRootId);
  flattenVisible(root, expanded, kMaxVisibleRows, rows);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[1].node->key, fk(1));
  EXPECT_EQ(rows[2].node->key, fk(2));
  EXPECT_EQ(rows[1].depth, 1u);
  EXPECT_EQ(rows[1].parentRow, 0u);

  expanded.insert(childId(kRootId, fk(1)));
  flattenVisible(root, expanded, kMaxVisibleRows, rows);
  ASSERT_EQ(rows.size(), 5u);
  // Pre-order: root, a, a1, a2, b.
  EXPECT_EQ(rows[2].node->key, fk(11));
  EXPECT_EQ(rows[3].node->key, fk(12));
  EXPECT_EQ(rows[4].node->key, fk(2));
  EXPECT_EQ(rows[2].parentRow, 1u);
  EXPECT_EQ(rows[4].parentRow, 0u);
  EXPECT_FALSE(rows[2].hasChildren);
  EXPECT_TRUE(rows[1].expanded);
}

TEST(CallGraphModel, FlattenHonoursTheRowCap) {
  const NodeAgg root = sampleTree();
  ExpandSet expanded;
  expandAll(root, expanded);
  std::vector<Row> rows;
  EXPECT_FALSE(flattenVisible(root, expanded, 100, rows));
  EXPECT_EQ(rows.size(), 7u);
  EXPECT_TRUE(flattenVisible(root, expanded, 4, rows));
  EXPECT_EQ(rows.size(), 4u);
  EXPECT_TRUE(flattenVisible(root, expanded, 0, rows));
  EXPECT_TRUE(rows.empty());
}

TEST(CallGraphModel, ExpandAllIsCappedAtTheVisibleRowLimit) {
  const NodeAgg root = sampleTree();
  ExpandSet expanded;
  EXPECT_FALSE(expandAll(root, expanded, 100));
  EXPECT_EQ(expanded.size(), 4u);  // the inner nodes: root, a, b, 20
  expanded.clear();
  EXPECT_TRUE(expandAll(root, expanded, 3));  // root, a, a1 visited, then the cap
  std::vector<Row> rows;
  flattenVisible(root, expanded, 3, rows);
  EXPECT_EQ(rows.size(), 3u);
}

TEST(CallGraphModel, HotPathExpandsTheHeaviestChildChain) {
  const NodeAgg root = sampleTree();
  ExpandSet expanded;
  const HotPathEnd end = expandHotPath(root, kRootId, expanded);
  // root -> a(60) -> a1(40): three nodes.
  EXPECT_EQ(end.length, 3u);
  EXPECT_EQ(end.node->key, fk(11));
  EXPECT_EQ(end.id, pathId(std::vector<FunctionKey>{fk(1), fk(11)}));
  std::vector<Row> rows;
  flattenVisible(root, expanded, kMaxVisibleRows, rows);
  ASSERT_EQ(rows.size(), 5u);  // root, a, a1, a2, b
  EXPECT_FALSE(expanded.count(childId(kRootId, fk(2))));

  // From the middle of a tree: only below that node.
  ExpandSet other;
  const NodeAgg& b = root.children[1];
  const HotPathEnd fromB = expandHotPath(b, childId(kRootId, fk(2)), other);
  EXPECT_EQ(fromB.length, 3u);
  EXPECT_EQ(fromB.node->key, fk(21));
  EXPECT_FALSE(other.count(kRootId));

  collapseAll(expanded);
  EXPECT_EQ(expanded.size(), 1u);
  EXPECT_TRUE(expanded.count(kRootId));
}

TEST(CallGraphModel, HotPathPicksTheHeaviestEvenIfNotFirst) {
  NodeAgg root = node(0, 10, 0, 1);
  root.children.push_back(node(1, 3, 3, 1));
  root.children.push_back(node(2, 7, 7, 1));
  ExpandSet expanded;
  EXPECT_EQ(expandHotPath(root, kRootId, expanded).node->key, fk(2));
}

TEST(CallGraphModel, DeepChainsAreTraversedWithoutRecursion) {
  constexpr size_t kDepth = 100'000;
  const NodeAgg root = deepChain(kDepth);
  ExpandSet expanded;
  const HotPathEnd end = expandHotPath(root, kRootId, expanded);
  EXPECT_EQ(end.length, kDepth + 1);
  EXPECT_EQ(end.node->key, fk(kDepth));
  std::vector<Row> rows;
  EXPECT_FALSE(flattenVisible(root, expanded, kMaxVisibleRows + 10, rows));
  ASSERT_EQ(rows.size(), kDepth + 1);
  EXPECT_EQ(rows.back().depth, kDepth);

  ExpandSet all;
  EXPECT_TRUE(expandAll(root, all, kMaxVisibleRows));  // 100 001 nodes: one over the cap
  EXPECT_TRUE(flattenVisible(root, all, kMaxVisibleRows, rows));
  EXPECT_EQ(rows.size(), kMaxVisibleRows);

  const std::vector<FunctionKey> path = pathOfRow(rows, rows.size() - 1);
  EXPECT_EQ(path.size(), kMaxVisibleRows - 1);
  const auto found = findFirstByKey(root, fk(kDepth));
  ASSERT_TRUE(found);
  EXPECT_EQ(found->ancestors.size(), kDepth);
  EXPECT_EQ(found->path.size(), kDepth);
  EXPECT_EQ(found->id, pathId(found->path));
  const auto byPath = findByPath(root, found->path);
  ASSERT_TRUE(byPath);
  EXPECT_EQ(byPath->node, found->node);
  EXPECT_FALSE(findFirstByKey(root, fk(kDepth + 5)));
  EXPECT_EQ(pieSlices(root).size(), 1u);
}

TEST(CallGraphModel, FindFirstByKeyIsBreadthFirstAndReportsAncestors) {
  // The key 7 occurs deep under a and shallow under b: the shallow one wins.
  NodeAgg root;
  root.key = analysis::kRootKey;
  std::vector<NodeAgg> deepKids;
  deepKids.push_back(node(7, 5, 5, 1));
  std::vector<NodeAgg> aKids;
  aKids.push_back(node(3, 5, 0, 1, std::move(deepKids)));
  root.children.push_back(node(1, 5, 0, 1, std::move(aKids)));
  std::vector<NodeAgg> bKids;
  bKids.push_back(node(7, 4, 4, 1));
  root.children.push_back(node(2, 4, 0, 1, std::move(bKids)));

  const auto found = findFirstByKey(root, fk(7));
  ASSERT_TRUE(found);
  EXPECT_EQ(found->node, &root.children[1].children[0]);
  ASSERT_EQ(found->ancestors.size(), 2u);
  EXPECT_EQ(found->ancestors[0], kRootId);
  EXPECT_EQ(found->ancestors[1], childId(kRootId, fk(2)));
  EXPECT_EQ(found->path, (std::vector<FunctionKey>{fk(2), fk(7)}));
  EXPECT_EQ(found->id, pathId(found->path));
  EXPECT_FALSE(findFirstByKey(root, fk(99)));
  EXPECT_FALSE(findFirstByKey(root, analysis::kRootKey));  // the root itself is not a match

  // Row lookup of an external selection: the id is in the flattened rows once the ancestors are expanded.
  ExpandSet expanded;
  for (const NodeId a : found->ancestors) expanded.insert(a);
  std::vector<Row> rows;
  flattenVisible(root, expanded, kMaxVisibleRows, rows);
  size_t at = SIZE_MAX;
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].id == found->id) at = i;
  ASSERT_NE(at, SIZE_MAX);
  EXPECT_EQ(rows[at].node, found->node);
  EXPECT_EQ(pathOfRow(rows, at), found->path);
  EXPECT_TRUE(pathOfRow(rows, 0).empty());
  EXPECT_TRUE(pathOfRow(rows, rows.size() + 3).empty());
}

TEST(CallGraphModel, FindByPathDescendsByKey) {
  const NodeAgg root = sampleTree();
  const auto at = findByPath(root, std::vector<FunctionKey>{fk(1), fk(12)});
  ASSERT_TRUE(at);
  EXPECT_EQ(at->node->inclusiveTicks, 20u);
  EXPECT_EQ(at->ancestors.size(), 2u);
  EXPECT_FALSE(findByPath(root, std::vector<FunctionKey>{fk(1), fk(21)}));
  EXPECT_EQ(findByPath(root, {})->node, &root);
}

TEST(CallGraphModel, PercentModes) {
  EXPECT_DOUBLE_EQ(percentOf(25, 100, 50, PercentMode::OfRoot), 25.0);
  EXPECT_DOUBLE_EQ(percentOf(25, 100, 50, PercentMode::OfParent), 50.0);
  EXPECT_DOUBLE_EQ(percentOf(25, 0, 50, PercentMode::OfRoot), 0.0);
  EXPECT_DOUBLE_EQ(percentOf(25, 100, 0, PercentMode::OfParent), 0.0);
  EXPECT_DOUBLE_EQ(percentOf(100, 100, 100, PercentMode::OfRoot), 100.0);
  EXPECT_DOUBLE_EQ(ticksToMs(10'000, 10'000'000.0), 1.0);
  EXPECT_DOUBLE_EQ(ticksToMs(5, 0.0), 0.0);
}

TEST(CallGraphModel, PieSlicesTakeTheTopTenChildrenAndFoldTheRestIntoOther) {
  NodeAgg n = node(0, 1000, 0, 1);
  for (uint64_t i = 1; i <= 14; ++i) n.children.push_back(node(i, i * 10, i * 10, 1));  // 10..140 ticks
  const std::vector<PieSlice> slices = pieSlices(n);
  ASSERT_EQ(slices.size(), 11u);
  EXPECT_EQ(slices[0].node->key, fk(14));
  EXPECT_EQ(slices[0].ticks, 140u);
  EXPECT_EQ(slices[9].node->key, fk(5));
  EXPECT_EQ(slices[10].node, nullptr);
  EXPECT_EQ(slices[10].otherCount, 4u);
  EXPECT_EQ(slices[10].ticks, 10u + 20u + 30u + 40u);
  double sum = 0;
  uint64_t ticks = 0;
  for (const PieSlice& s : slices) {
    sum += s.fraction;
    ticks += s.ticks;
  }
  EXPECT_NEAR(sum, 1.0, 1e-9);
  EXPECT_EQ(ticks, 14u * 15u / 2u * 10u);
  EXPECT_FLOAT_EQ(slices.front().angle0, 0.0f);
  EXPECT_NEAR(slices.back().angle1, 6.2831853f, 1e-4f);
  for (size_t i = 1; i < slices.size(); ++i) EXPECT_FLOAT_EQ(slices[i].angle0, slices[i - 1].angle1);
}

TEST(CallGraphModel, PieSlicesWithFewChildrenHaveNoOtherAndZeroTimeGivesNone) {
  const NodeAgg root = sampleTree();
  const std::vector<PieSlice> slices = pieSlices(root);
  ASSERT_EQ(slices.size(), 2u);
  EXPECT_EQ(slices[0].node->key, fk(1));
  EXPECT_NEAR(slices[0].fraction, 0.6, 1e-12);
  EXPECT_NE(slices[1].node, nullptr);

  NodeAgg zero = node(0, 0, 0, 3);
  zero.children.push_back(node(1, 0, 0, 3));
  EXPECT_TRUE(pieSlices(zero).empty());
  EXPECT_TRUE(pieSlices(node(0, 5, 5, 1)).empty());  // a leaf
  // Exactly ten children: no "other".
  NodeAgg ten = node(0, 55, 0, 1);
  for (uint64_t i = 1; i <= 10; ++i) ten.children.push_back(node(i, i, i, 1));
  EXPECT_EQ(pieSlices(ten).size(), 10u);
}

TEST(CallGraphModel, FilterMapping) {
  const analysis::Filter whole = toQueryFilter(std::nullopt);
  EXPECT_EQ(whole.t0, 0u);
  EXPECT_EQ(whole.t1, UINT64_MAX);
  EXPECT_FALSE(whole.threads);
  EXPECT_FALSE(whole.subtreeRoot);
  EXPECT_FALSE(whole.function);

  ViewFilter v;
  v.t0 = 100;
  v.t1 = 900;
  v.threads = std::vector<ThreadIdx>{ThreadIdx(2), ThreadIdx(5)};
  v.subtreeRoot = CallstackIdx(42);
  v.function = FunctionIdx(3);  // a bus-level handle is not a handle of the query context: not mapped
  const analysis::Filter f = toQueryFilter(v);
  EXPECT_EQ(f.t0, 100u);
  EXPECT_EQ(f.t1, 900u);
  ASSERT_TRUE(f.threads);
  EXPECT_EQ(f.threads->size(), 2u);
  EXPECT_EQ(f.subtreeRoot, CallstackIdx(42));
  EXPECT_FALSE(f.function);
}

namespace {

using Value = std::shared_ptr<const int>;

bool waitFor(const std::function<bool()>& pred, int ms = 10000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

TEST(CallGraphModel, LatestQueryDropsTheResultOfASupersededRequest) {
  analysis::QueryExecutor ex(analysis::QueryExecutor::Options{2, 16});
  LatestQuery<int> q;
  std::promise<void> gate;
  const std::shared_future<void> open = gate.get_future().share();
  std::atomic<int> started{0};

  q.issue(ex, [&](const analysis::SubmitOptions& so) {
    return ex.submitQuery<Value>(so, [&](analysis::Cancel) -> Result<Value> {
      ++started;
      open.wait();  // ignores the cancel flag on purpose: a task that finishes late
      return Value(std::make_shared<const int>(1));
    });
  });
  ASSERT_TRUE(waitFor([&] { return started.load() == 1; }));
  EXPECT_TRUE(q.pending());
  EXPECT_FALSE(q.poll());

  q.issue(ex, [&](const analysis::SubmitOptions& so) {
    return ex.submitQuery<Value>(
        so, [&](analysis::Cancel) -> Result<Value> { return Value(std::make_shared<const int>(2)); });
  });
  ASSERT_TRUE(waitFor([&] { return !q.pending(); }));
  gate.set_value();  // the stale task completes after the newer result is available
  ex.waitIdle();
  const auto r = q.poll();
  ASSERT_TRUE(r);
  ASSERT_TRUE(r->ok());
  EXPECT_EQ(*r->value(), 2);
  EXPECT_FALSE(q.poll());  // delivered once
  EXPECT_EQ(q.issued(), 2u);
}

TEST(CallGraphModel, LatestQueryNeverRunsATaskThatWasSupersededWhileQueued) {
  analysis::QueryExecutor ex(analysis::QueryExecutor::Options{1, 16});  // one worker: later tasks queue
  LatestQuery<int> q;
  std::promise<void> gate;
  const std::shared_future<void> open = gate.get_future().share();
  std::atomic<int> started{0}, ranSecond{0}, ranThird{0};

  q.issue(ex, [&](const analysis::SubmitOptions& so) {
    return ex.submitQuery<Value>(so, [&](analysis::Cancel) -> Result<Value> {
      ++started;
      open.wait();
      return Value(std::make_shared<const int>(1));
    });
  });
  ASSERT_TRUE(waitFor([&] { return started.load() == 1; }));
  q.issue(ex, [&](const analysis::SubmitOptions& so) {
    return ex.submitQuery<Value>(so, [&](analysis::Cancel) -> Result<Value> {
      ++ranSecond;
      return Value(std::make_shared<const int>(2));
    });
  });
  q.issue(ex, [&](const analysis::SubmitOptions& so) {
    return ex.submitQuery<Value>(so, [&](analysis::Cancel) -> Result<Value> {
      ++ranThird;
      return Value(std::make_shared<const int>(3));
    });
  });
  gate.set_value();
  ASSERT_TRUE(waitFor([&] { return !q.pending(); }));
  ex.waitIdle();
  EXPECT_EQ(ranSecond.load(), 0);
  EXPECT_EQ(ranThird.load(), 1);
  const auto r = q.poll();
  ASSERT_TRUE(r && r->ok());
  EXPECT_EQ(*r->value(), 3);
}

TEST(CallGraphModel, LatestQueryCancelForgetsTheRequestAndSurfacesErrors) {
  analysis::QueryExecutor ex(analysis::QueryExecutor::Options{1, 16});
  LatestQuery<int> q;
  std::promise<void> gate;
  const std::shared_future<void> open = gate.get_future().share();
  std::atomic<int> started{0};
  q.issue(ex, [&](const analysis::SubmitOptions& so) {
    return ex.submitQuery<Value>(so, [&](analysis::Cancel) -> Result<Value> {
      ++started;
      open.wait();
      return Value(std::make_shared<const int>(1));
    });
  });
  ASSERT_TRUE(waitFor([&] { return started.load() == 1; }));
  q.cancel();
  EXPECT_FALSE(q.pending());
  gate.set_value();
  ex.waitIdle();
  EXPECT_FALSE(q.poll());

  q.issue(ex, [&](const analysis::SubmitOptions& so) {
    return ex.submitQuery<Value>(so, [](analysis::Cancel) -> Result<Value> {
      return makeError(ErrorCode::InvalidArgument, "bad filter");
    });
  });
  ASSERT_TRUE(waitFor([&] { return !q.pending(); }));
  const auto err = q.poll();
  ASSERT_TRUE(err);
  EXPECT_FALSE(err->ok());
  EXPECT_EQ(err->error().code, ErrorCode::InvalidArgument);
}
