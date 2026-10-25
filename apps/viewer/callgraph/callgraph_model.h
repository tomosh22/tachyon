// Pure logic of the Call Graph panel (WP-43): flattening the top-down tree into visible rows, expand state,
// hot path, percent modes, pie slices, filter mapping, the stale-result guard and the lookup of tree nodes.
//
// Every traversal of an analysis::NodeAgg here is iterative: trees can be 100 000+ frames deep and the
// recursive NodeAgg copy / compare must never be used (docs/specs/analysis-query.md).
#pragma once

#include "analysis/query.h"
#include "analysis/query_executor.h"
#include "core/result.h"
#include "symbols/function_key.h"
#include "viewer/selection_bus.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace tachyon::viewer::callgraph {

// Identity of a tree node that survives re-querying: a hash of the path of FunctionKeys from the root.
using NodeId = uint64_t;
inline constexpr NodeId kRootId = 0x9E3779B97F4A7C15ull;

// Thread-safe: pure function. Id of the child of `parent` with function `key`.
NodeId childId(NodeId parent, const symbols::FunctionKey& key);
// Thread-safe: pure function. Id of the node at the end of `path` (keys below the root; empty = the root).
NodeId pathId(std::span<const symbols::FunctionKey> path);

struct NodeIdHash {
  size_t operator()(NodeId id) const noexcept { return static_cast<size_t>(id); }  // already well mixed
};
using ExpandSet = std::unordered_set<NodeId, NodeIdHash>;

inline constexpr size_t kMaxVisibleRows = 100'000;  // expand-all / flatten cap

// One visible row of the flattened tree. `node` and everything it points to belong to the displayed tree,
// which the panel keeps alive.
struct Row {
  const analysis::NodeAgg* node = nullptr;
  NodeId id = 0;
  uint32_t parentRow = UINT32_MAX;  // index into the row array; UINT32_MAX for the root
  uint32_t depth = 0;
  bool hasChildren = false;
  bool expanded = false;
};

// Thread-safe (no shared state). Builds the visible rows of `root` in display order: a node's children follow
// it when its id is in `expanded`. At most `maxRows` rows; returns true when the output was cut. O(rows).
bool flattenVisible(const analysis::NodeAgg& root, const ExpandSet& expanded, size_t maxRows,
                    std::vector<Row>& out);

// Thread-safe (no shared state). Marks nodes expanded in pre-order until `maxRows` nodes were visited (the
// rows an expand-all can show). Returns true when the cap cut the expansion.
bool expandAll(const analysis::NodeAgg& root, ExpandSet& expanded, size_t maxRows = kMaxVisibleRows);

// Thread-safe (no shared state). Expands the heaviest-child chain starting at `from` (whose id is `fromId`),
// inclusive of `from`, down to a leaf. Returns the id and node of the last node of the chain.
struct HotPathEnd {
  NodeId id = 0;
  const analysis::NodeAgg* node = nullptr;
  size_t length = 0;  // nodes on the chain, `from` included
};
HotPathEnd expandHotPath(const analysis::NodeAgg& from, NodeId fromId, ExpandSet& expanded);

// Thread-safe (no shared state). Collapse everything but the root.
void collapseAll(ExpandSet& expanded);

// Result of finding a node: its id and the ids of all its ancestors (root first, node excluded), plus the
// keys below the root leading to it.
struct Located {
  const analysis::NodeAgg* node = nullptr;
  NodeId id = 0;
  std::vector<NodeId> ancestors;
  std::vector<symbols::FunctionKey> path;
};

// Thread-safe (no shared state). Breadth-first search for the first node with function `key` (the root is
// not considered). Iterative; O(nodes) worst case.
std::optional<Located> findFirstByKey(const analysis::NodeAgg& root, const symbols::FunctionKey& key);
// Thread-safe (no shared state). Descends along `path`; O(depth * fan-out).
std::optional<Located> findByPath(const analysis::NodeAgg& root, std::span<const symbols::FunctionKey> path);

// Thread-safe (no shared state). Keys from the root's child down to row `row` (the root row gives an empty
// path). O(depth).
std::vector<symbols::FunctionKey> pathOfRow(std::span<const Row> rows, size_t row);

// ---- numbers ----------------------------------------------------------------------------------------------

enum class PercentMode : uint8_t { OfRoot, OfParent };

// Thread-safe: pure function. Percentage (0..100) of `ticks` relative to the root total or to the parent's
// inclusive time; 0 when the base is 0. The root row is 100 in both modes.
double percentOf(uint64_t ticks, uint64_t rootTotal, uint64_t parentTotal, PercentMode mode);
// Thread-safe: pure function. Ticks to milliseconds.
double ticksToMs(uint64_t ticks, double clockHz);

struct PieSlice {
  const analysis::NodeAgg* node = nullptr;  // null for the "other" slice
  uint64_t ticks = 0;
  double fraction = 0;           // of the sum of the children's inclusive ticks
  float angle0 = 0, angle1 = 0;  // radians, clockwise from 12 o'clock, [0, 2 pi]
  uint32_t otherCount = 0;       // children folded into "other"
};

// Thread-safe (no shared state). Slices for the children of `node`: the `maxChildren` heaviest children in
// descending order, then one "other" slice for the rest. Empty when the children carry no time.
std::vector<PieSlice> pieSlices(const analysis::NodeAgg& node, size_t maxChildren = 10);

// ---- filter mapping ---------------------------------------------------------------------------------------

// Thread-safe: pure function. SelectionBus filter -> analysis::Filter. No bus filter means the whole trace.
// ViewFilter::function is deliberately not mapped: a bus-level FunctionIdx is not a handle of a particular
// QueryContext (contexts are rebuilt when symbols resolve).
analysis::Filter toQueryFilter(const std::optional<ViewFilter>& filter);

// ---- stale-result guard -----------------------------------------------------------------------------------

// The newest query of one view. issue() cancels and supersedes the previous one; poll() delivers the result
// of the newest request exactly once and never the result of a superseded one.
template <typename T>
class LatestQuery {
 public:
  using Value = std::shared_ptr<const T>;
  using Submitter = std::function<analysis::QueryFuture<Value>(const analysis::SubmitOptions&)>;

  LatestQuery() : m_token(std::make_shared<analysis::GenerationToken>()) {}
  LatestQuery(const LatestQuery&) = delete;
  LatestQuery& operator=(const LatestQuery&) = delete;
  // Cancels the request in flight (superseded ones were cancelled when they were replaced). The token is kept
  // alive by the queued tasks themselves.
  ~LatestQuery() { cancel(); }

  // Main-thread only. Starts a new generation, cancels the previous request and submits the new one through
  // `submit` (which receives the options carrying the guard and the generation). Foreground priority.
  void issue(analysis::QueryExecutor& executor, const Submitter& submit) {
    cancel();
    m_executor = &executor;
    analysis::SubmitOptions options;
    options.priority = analysis::Priority::Foreground;
    options.guard = m_token.get();
    options.generation = m_token->advance();
    options.onFinished = [token = m_token](analysis::TaskState) {};  // keeps the guard alive while queued
    m_future = submit(options);
    m_delivered = false;
    ++m_issued;
  }

  // Main-thread only. Cancels the request in flight and forgets it (a later poll() returns nothing).
  void cancel() {
    if (m_executor && m_future.handle().valid() && !m_future.handle().finished())
      m_executor->cancel(m_future.handle());
    m_token->advance();  // whatever is still queued or running is stale now
    m_future = {};
    m_delivered = true;
  }

  // Main-thread only. True while the newest request has not finished.
  bool pending() const { return m_future.handle().valid() && !m_delivered && !m_future.handle().finished(); }
  // Main-thread only. The newest request's result once it finished (then pending() is false). Nothing while
  // it runs, after it was delivered, or when it was cancelled / superseded.
  std::optional<Result<Value>> poll() {
    if (m_delivered || !m_future.handle().valid() || !m_future.handle().finished()) return std::nullopt;
    m_delivered = true;
    if (!m_future.handle().isCurrent()) return std::nullopt;  // stale
    const Result<Value>& r = m_future.get();
    if (!r.ok() && r.error().code == ErrorCode::Cancelled) return std::nullopt;
    return r;
  }

  // Number of requests issued so far (tests).
  uint64_t issued() const { return m_issued; }

 private:
  std::shared_ptr<analysis::GenerationToken> m_token;
  analysis::QueryExecutor* m_executor = nullptr;
  analysis::QueryFuture<Value> m_future;
  bool m_delivered = true;
  uint64_t m_issued = 0;
};

}  // namespace tachyon::viewer::callgraph
