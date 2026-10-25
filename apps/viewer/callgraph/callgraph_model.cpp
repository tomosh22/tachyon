#include "viewer/callgraph/callgraph_model.h"

#include "core/hash.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace tachyon::viewer::callgraph {

using analysis::NodeAgg;
using symbols::FunctionKey;

NodeId childId(NodeId parent, const FunctionKey& key) {
  const uint64_t k = core::hashU64(key.value ^ core::hashU64(static_cast<uint64_t>(key.module.get()) + 1));
  return core::hashU64(parent ^ (k + 0x9E3779B97F4A7C15ull + (parent << 6) + (parent >> 2)));
}

NodeId pathId(std::span<const FunctionKey> path) {
  NodeId id = kRootId;
  for (const FunctionKey& k : path) id = childId(id, k);
  return id;
}

bool flattenVisible(const NodeAgg& root, const ExpandSet& expanded, size_t maxRows, std::vector<Row>& out) {
  out.clear();
  if (maxRows == 0) return true;
  struct Item {
    const NodeAgg* node;
    NodeId id;
    uint32_t parentRow;
    uint32_t depth;
  };
  std::vector<Item> stack;
  stack.push_back({&root, kRootId, UINT32_MAX, 0});
  while (!stack.empty()) {
    const Item it = stack.back();
    stack.pop_back();
    if (out.size() >= maxRows) return true;
    Row row;
    row.node = it.node;
    row.id = it.id;
    row.parentRow = it.parentRow;
    row.depth = it.depth;
    row.hasChildren = !it.node->children.empty();
    row.expanded = row.hasChildren && expanded.count(it.id) != 0;
    const uint32_t index = static_cast<uint32_t>(out.size());
    out.push_back(row);
    if (row.expanded) {
      for (size_t i = it.node->children.size(); i-- > 0;) {
        const NodeAgg& c = it.node->children[i];
        stack.push_back({&c, childId(it.id, c.key), index, it.depth + 1});
      }
    }
  }
  return false;
}

bool expandAll(const NodeAgg& root, ExpandSet& expanded, size_t maxRows) {
  struct Item {
    const NodeAgg* node;
    NodeId id;
  };
  std::vector<Item> stack;
  stack.push_back({&root, kRootId});
  size_t visited = 0;
  while (!stack.empty()) {
    const Item it = stack.back();
    stack.pop_back();
    if (visited >= maxRows) return true;
    ++visited;
    if (it.node->children.empty()) continue;
    expanded.insert(it.id);
    for (size_t i = it.node->children.size(); i-- > 0;) {
      const NodeAgg& c = it.node->children[i];
      stack.push_back({&c, childId(it.id, c.key)});
    }
  }
  return false;
}

HotPathEnd expandHotPath(const NodeAgg& from, NodeId fromId, ExpandSet& expanded) {
  HotPathEnd end{fromId, &from, 1};
  while (!end.node->children.empty()) {
    expanded.insert(end.id);
    // The query sorts children by inclusive ticks descending; do not rely on it for correctness.
    const NodeAgg* best = &end.node->children.front();
    for (const NodeAgg& c : end.node->children)
      if (c.inclusiveTicks > best->inclusiveTicks) best = &c;
    end.id = childId(end.id, best->key);
    end.node = best;
    ++end.length;
  }
  return end;
}

void collapseAll(ExpandSet& expanded) {
  expanded.clear();
  expanded.insert(kRootId);
}

std::optional<Located> findFirstByKey(const NodeAgg& root, const FunctionKey& key) {
  struct Entry {
    const NodeAgg* node;
    NodeId id;
    uint32_t parent;
  };
  std::vector<Entry> queue;
  queue.push_back({&root, kRootId, UINT32_MAX});
  for (size_t head = 0; head < queue.size(); ++head) {
    const Entry e = queue[head];
    for (const NodeAgg& c : e.node->children) {
      const NodeId cid = childId(e.id, c.key);
      if (c.key == key) {
        Located found;
        found.node = &c;
        found.id = cid;
        // Walk the parent chain back to the root.
        std::vector<uint32_t> chain;
        for (uint32_t p = static_cast<uint32_t>(head); p != UINT32_MAX; p = queue[p].parent)
          chain.push_back(p);
        for (size_t i = chain.size(); i-- > 0;) found.ancestors.push_back(queue[chain[i]].id);
        for (size_t i = chain.size(); i-- > 0;) {
          if (queue[chain[i]].node != &root) found.path.push_back(queue[chain[i]].node->key);
        }
        found.path.push_back(key);
        return found;
      }
      queue.push_back({&c, cid, static_cast<uint32_t>(head)});
    }
  }
  return std::nullopt;
}

std::optional<Located> findByPath(const NodeAgg& root, std::span<const FunctionKey> path) {
  Located found;
  found.node = &root;
  found.id = kRootId;
  for (const FunctionKey& k : path) {
    const NodeAgg* next = nullptr;
    for (const NodeAgg& c : found.node->children) {
      if (c.key == k) {
        next = &c;
        break;
      }
    }
    if (!next) return std::nullopt;
    found.ancestors.push_back(found.id);
    found.id = childId(found.id, k);
    found.node = next;
    found.path.push_back(k);
  }
  return found;
}

std::vector<FunctionKey> pathOfRow(std::span<const Row> rows, size_t row) {
  std::vector<FunctionKey> path;
  if (row >= rows.size()) return path;
  for (size_t r = row; r < rows.size() && rows[r].parentRow != UINT32_MAX; r = rows[r].parentRow)
    path.push_back(rows[r].node->key);
  std::reverse(path.begin(), path.end());
  return path;
}

double percentOf(uint64_t ticks, uint64_t rootTotal, uint64_t parentTotal, PercentMode mode) {
  const uint64_t base = mode == PercentMode::OfRoot ? rootTotal : parentTotal;
  if (base == 0) return 0.0;
  return 100.0 * static_cast<double>(ticks) / static_cast<double>(base);
}

std::vector<PieSlice> pieSlices(const NodeAgg& node, size_t maxChildren) {
  std::vector<PieSlice> slices;
  const size_t n = node.children.size();
  if (n == 0) return slices;
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  const auto heavier = [&](uint32_t a, uint32_t b) {
    const uint64_t ta = node.children[a].inclusiveTicks, tb = node.children[b].inclusiveTicks;
    return ta != tb ? ta > tb : a < b;
  };
  const size_t shown = std::min(n, maxChildren);
  if (shown < n)
    std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(shown), order.end(), heavier);
  std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(shown), heavier);
  uint64_t total = 0;
  for (const NodeAgg& c : node.children) total += c.inclusiveTicks;
  if (total == 0) return slices;
  uint64_t shownTicks = 0;
  for (size_t i = 0; i < shown; ++i) {
    PieSlice s;
    s.node = &node.children[order[i]];
    s.ticks = s.node->inclusiveTicks;
    shownTicks += s.ticks;
    if (s.ticks > 0) slices.push_back(s);
  }
  if (shown < n && total > shownTicks) {
    PieSlice other;
    other.ticks = total - shownTicks;
    other.otherCount = static_cast<uint32_t>(n - shown);
    slices.push_back(other);
  }
  constexpr double kTwoPi = 6.283185307179586;
  uint64_t acc = 0;
  for (PieSlice& s : slices) {
    s.fraction = static_cast<double>(s.ticks) / static_cast<double>(total);
    s.angle0 = static_cast<float>(kTwoPi * static_cast<double>(acc) / static_cast<double>(total));
    acc += s.ticks;
    s.angle1 = static_cast<float>(kTwoPi * static_cast<double>(acc) / static_cast<double>(total));
  }
  return slices;
}

}  // namespace tachyon::viewer::callgraph
