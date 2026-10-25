#include "analysis/attribution.h"
#include "analysis/detail/query_support.h"
#include "analysis/query.h"
#include "analysis/query_cache.h"
#include "analysis/query_executor.h"

#include <algorithm>
#include <cstdio>
#include <list>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace tachyon::analysis {

using namespace detail;

using symbols::kInlineKeyBit;
using symbols::kScopeModule;

namespace {

std::string_view baseName(std::string_view path) {
  const size_t p = path.find_last_of("/\\");
  return p == std::string_view::npos ? path : path.substr(p + 1);
}

std::string hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

// Key of the per-leaf-node weight table: depends on the time range and the threads only.
QueryKey weightsKey(const ResolvedFilter& f) {
  QueryKey k;
  k.kind = QueryKind::Weights;
  k.filter.t0 = f.canonical.t0;
  k.filter.t1 = f.canonical.t1;
  k.filter.threads = f.canonical.threads;
  return k;
}

size_t treeBytes(const NodeAgg& root) {
  size_t bytes =
      sizeof(NodeAgg) + 2 * kAllocOverhead + 32;  // the root block and its shared_ptr control block
  std::vector<const NodeAgg*> work{&root};
  while (!work.empty()) {
    const NodeAgg* n = work.back();
    work.pop_back();
    bytes += vectorBytes(n->children);  // the children blocks hold every other node
    for (const NodeAgg& c : n->children) work.push_back(&c);
  }
  return bytes;
}

// Copies a tree without recursion (the defaulted NodeAgg copy is recursive).
NodeAgg cloneTree(const NodeAgg& src) {
  NodeAgg out;
  out.key = src.key;
  out.inclusiveTicks = src.inclusiveTicks;
  out.exclusiveTicks = src.exclusiveTicks;
  out.sampleCount = src.sampleCount;
  std::vector<std::pair<const NodeAgg*, NodeAgg*>> work;
  work.emplace_back(&src, &out);
  while (!work.empty()) {
    const auto [from, to] = work.back();
    work.pop_back();
    to->children.resize(from->children.size());  // no further resize: the pointers below stay valid
    for (size_t i = 0; i < from->children.size(); ++i) {
      const NodeAgg& s = from->children[i];
      NodeAgg& d = to->children[i];
      d.key = s.key;
      d.inclusiveTicks = s.inclusiveTicks;
      d.exclusiveTicks = s.exclusiveTicks;
      d.sampleCount = s.sampleCount;
      if (!s.children.empty()) work.emplace_back(&s, &d);
    }
  }
  return out;
}

using WeightsPtr = std::shared_ptr<const std::vector<NodeWeight>>;

Result<WeightsPtr> weightsFor(const QueryContext& ctx, const ResolvedFilter& f, Cancel cancel) {
  QueryCache& cache = ctx.cache();
  const QueryKey key = weightsKey(f);
  if (auto hit = cache.find<std::vector<NodeWeight>>(key)) return hit;
  if (f.t0 >= f.t1) return std::make_shared<const std::vector<NodeWeight>>();
  auto acc = accumulateNodeWeights(ctx.model(), f.t0, f.t1, f.threads, ctx.options().threads, cancel,
                                   ctx.executor());
  if (!acc) return acc.error();
  const size_t bytes = sizeof(std::vector<NodeWeight>) + 2 * kAllocOverhead + 32 + vectorBytes(*acc);
  auto value = std::make_shared<const std::vector<NodeWeight>>(std::move(*acc));
  cache.insert(key, value, bytes);
  return value;
}

bool sortRowsBy(const FunctionRow& a, const FunctionRow& b) {
  if (a.exclusiveTicks != b.exclusiveTicks) return a.exclusiveTicks > b.exclusiveTicks;
  if (a.inclusiveTicks != b.inclusiveTicks) return a.inclusiveTicks > b.inclusiveTicks;
  return a.key < b.key;
}

bool sortRowsByInclusive(const FunctionRow& a, const FunctionRow& b) {
  if (a.inclusiveTicks != b.inclusiveTicks) return a.inclusiveTicks > b.inclusiveTicks;
  return a.key < b.key;
}

}  // namespace

NodeAgg::~NodeAgg() {
  // Iterative: tear the tree down one level at a time so a very deep tree cannot overflow the stack.
  if (children.empty()) return;
  std::vector<std::vector<NodeAgg>> work;
  work.push_back(std::move(children));
  while (!work.empty()) {
    std::vector<NodeAgg> level = std::move(work.back());
    work.pop_back();
    for (NodeAgg& n : level)
      if (!n.children.empty()) work.push_back(std::move(n.children));
  }  // `level` dies here; its elements have no children left, so their destructors return immediately
}

struct QueryContext::Impl {
  Impl(const model::TraceModel& m, const symbols::SymbolService& s, AggregationOptions o, QueryCacheConfig c)
      : model(m), symbols(s), options(o), cache(c.budgetBytes) {}
  const model::TraceModel& model;
  const symbols::SymbolService& symbols;
  AggregationOptions options;
  QueryCache cache;
  FunctionRegistry registry;
  std::atomic<QueryExecutor*> executor{nullptr};
};

QueryContext::QueryContext(const model::TraceModel& model, const symbols::SymbolService& symbols,
                           AggregationOptions options, QueryCacheConfig cache)
    : m_impl(new Impl(model, symbols, options, cache)) {}
QueryContext::~QueryContext() = default;

const model::TraceModel& QueryContext::model() const {
  return m_impl->model;
}
const symbols::SymbolService& QueryContext::symbols() const {
  return m_impl->symbols;
}
const AggregationOptions& QueryContext::options() const {
  return m_impl->options;
}
QueryCache& QueryContext::cache() const {
  return m_impl->cache;
}
FunctionRegistry& QueryContext::functions() const {
  return m_impl->registry;
}
void QueryContext::setExecutor(QueryExecutor* executor) {
  m_impl->executor.store(executor, std::memory_order_release);
}
QueryExecutor* QueryContext::executor() const {
  return m_impl->executor.load(std::memory_order_acquire);
}
void QueryContext::setCacheBudget(size_t bytes) {
  m_impl->cache.setBudget(bytes);
}

void QueryContext::clearCache() {
  m_impl->cache.clear();
}

CacheStats QueryContext::cacheStats() const {
  const CacheCounters total = m_impl->cache.totalCounters();
  const CacheCounters results = m_impl->cache.resultCounters();
  CacheStats s;
  s.hits = total.hits;
  s.misses = total.misses;
  s.resultHits = results.hits;
  s.resultMisses = results.misses;
  s.evictions = total.evictions;
  s.entries = m_impl->cache.entryCount();
  s.bytes = m_impl->cache.bytesUsed();
  return s;
}

namespace {

size_t rowsBytes(const std::vector<FunctionRow>& rows) {
  return sizeof(rows) + 2 * kAllocOverhead + 32 + vectorBytes(rows);
}

size_t butterflyBytes(const Butterfly& b) {
  return sizeof(Butterfly) + 2 * kAllocOverhead + 32 + vectorBytes(b.callers) + vectorBytes(b.callees);
}

}  // namespace

Result<std::shared_ptr<const std::vector<FunctionRow>>> bottomUpFunctionsShared(const QueryContext& ctx,
                                                                                const Filter& filter,
                                                                                Cancel cancel) {
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  return cachedOrCompute<std::vector<FunctionRow>>(
      ctx, resultKey(*rf, QueryKind::BottomUp),
      [&]() -> Result<std::vector<FunctionRow>> {
        auto w = weightsFor(ctx, *rf, cancel);
        if (!w) return w.error();

        KeyTable table;
        StackFlattener flat(ctx, table);
        const uint32_t required = rf->function ? table.intern(*rf->function) : kUnset;
        std::vector<uint64_t> incl, excl;
        std::vector<uint64_t> inst;
        std::vector<uint32_t> stamp;
        std::vector<uint32_t> ids;
        uint32_t serial = 0;
        CancelMeter meter{cancel};
        flat.setCancel(cancel);
        for (const NodeWeight& nw : **w) {
          if (meter.due()) return cancelledError();
          const bool kept = flat.flatten(nw.node, rf->subtree, nw.kernelLeaf, ids, required);
          if (flat.aborted()) return cancelledError();
          meter.add(flat.lastCost() + ids.size());
          if (!kept || ids.empty()) continue;
          if (table.size() > incl.size()) {
            incl.resize(table.size(), 0);
            excl.resize(table.size(), 0);
            inst.resize(table.size(), 0);
            stamp.resize(table.size(), 0);
          }
          ++serial;
          for (uint32_t id : ids) {
            if (stamp[id] == serial) continue;  // recursion: count once per sample
            stamp[id] = serial;
            incl[id] += nw.ticks;
            inst[id] += nw.samples;
          }
          excl[ids.back()] += nw.ticks;
        }
        std::vector<FunctionRow> rows;
        for (uint32_t id = 0; id < incl.size(); ++id) {
          if (inst[id] == 0) continue;
          rows.push_back(FunctionRow{table.key(id), incl[id], excl[id], saturate32(inst[id])});
        }
        std::sort(rows.begin(), rows.end(), sortRowsBy);
        return rows;
      },
      rowsBytes);
}

Result<std::vector<FunctionRow>> bottomUpFunctions(const QueryContext& ctx, const Filter& filter,
                                                   Cancel cancel) {
  auto r = bottomUpFunctionsShared(ctx, filter, cancel);
  if (!r) return r.error();
  return **r;
}

Result<std::shared_ptr<const NodeAgg>> topDownTreeShared(const QueryContext& ctx, const Filter& filter,
                                                         Cancel cancel) {
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  return cachedOrCompute<NodeAgg>(
      ctx, resultKey(*rf, QueryKind::TopDown),
      [&]() -> Result<NodeAgg> {
        auto w = weightsFor(ctx, *rf, cancel);
        if (!w) return w.error();

        struct TNode {
          uint32_t parent = 0;
          uint32_t keyId = 0;
          uint64_t incl = 0, excl = 0;
          uint64_t count = 0, ownCount = 0;
        };
        KeyTable table;
        StackFlattener flat(ctx, table);
        const uint32_t required = rf->function ? table.intern(*rf->function) : kUnset;
        std::vector<TNode> tn(1);
        std::unordered_map<uint64_t, uint32_t> children;
        std::vector<uint32_t> ids;
        CancelMeter meter{cancel};
        flat.setCancel(cancel);
        for (const NodeWeight& nw : **w) {
          if (meter.due()) return cancelledError();
          const bool kept = flat.flatten(nw.node, rf->subtree, nw.kernelLeaf, ids, required);
          if (flat.aborted()) return cancelledError();
          meter.add(flat.lastCost() + ids.size());
          if (!kept) continue;
          uint32_t cur = 0;
          for (uint32_t id : ids) {
            const uint64_t ck = (uint64_t{cur} << 32) | id;
            auto [it, inserted] = children.try_emplace(ck, static_cast<uint32_t>(tn.size()));
            if (inserted) {
              TNode n;
              n.parent = cur;
              n.keyId = id;
              tn.push_back(n);
            }
            cur = it->second;
          }
          tn[cur].excl += nw.ticks;
          tn[cur].ownCount += nw.samples;
        }
        // Children always have larger indices than their parents: propagate inclusive values bottom-up.
        for (size_t i = tn.size(); i-- > 0;) {
          if ((i & (kCancelStride - 1)) == 0 && cancel.requested()) return cancelledError();
          tn[i].incl += tn[i].excl;
          tn[i].count += tn[i].ownCount;
          if (i != 0) {
            tn[tn[i].parent].incl += tn[i].incl;
            tn[tn[i].parent].count += tn[i].count;
          }
        }
        // Materialize iteratively (deep recursion must not overflow the stack): every node's children are
        // complete by the time it is moved into its parent.
        std::vector<NodeAgg> out(tn.size());
        for (size_t i = tn.size(); i-- > 0;) {
          if ((i & (kCancelStride - 1)) == 0 && cancel.requested()) return cancelledError();
          NodeAgg& a = out[i];
          a.key = i == 0 ? kRootKey : table.key(tn[i].keyId);
          a.inclusiveTicks = tn[i].incl;
          a.exclusiveTicks = tn[i].excl;
          a.sampleCount = saturate32(tn[i].count);
          std::sort(a.children.begin(), a.children.end(), [](const NodeAgg& x, const NodeAgg& y) {
            if (x.inclusiveTicks != y.inclusiveTicks) return x.inclusiveTicks > y.inclusiveTicks;
            return x.key < y.key;
          });
          if (i != 0) out[tn[i].parent].children.push_back(std::move(a));
        }
        if (cancel.requested()) return cancelledError();
        NodeAgg root = std::move(out[0]);
        if (rf->subtree.valid() && root.children.size() == 1 && root.exclusiveTicks == 0 &&
            root.children[0].key != kRootKey)
          return std::move(root.children[0]);
        return root;
      },
      treeBytes);
}

Result<NodeAgg> topDownTree(const QueryContext& ctx, const Filter& filter, Cancel cancel) {
  auto r = topDownTreeShared(ctx, filter, cancel);
  if (!r) return r.error();
  return cloneTree(**r);
}

Result<std::shared_ptr<const Butterfly>> butterflyShared(const QueryContext& ctx, const Filter& filter,
                                                         FunctionKey key, Cancel cancel) {
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  return cachedOrCompute<Butterfly>(
      ctx, resultKey(*rf, QueryKind::Butterfly, key),
      [&]() -> Result<Butterfly> {
        auto w = weightsFor(ctx, *rf, cancel);
        if (!w) return w.error();

        KeyTable table;
        StackFlattener flat(ctx, table);
        const uint32_t target = table.intern(key);
        const uint32_t rootId = table.intern(kRootKey);
        const uint32_t required = rf->function ? table.intern(*rf->function) : kUnset;
        Butterfly bf;
        bf.self.key = key;
        std::unordered_map<uint32_t, FunctionRow> callers, callees;
        std::vector<uint32_t> ids;
        CancelMeter meter{cancel};
        flat.setCancel(cancel);
        for (const NodeWeight& nw : **w) {
          if (meter.due()) return cancelledError();
          const bool kept = flat.flatten(nw.node, rf->subtree, nw.kernelLeaf, ids, required);
          if (flat.aborted()) return cancelledError();
          meter.add(flat.lastCost() + ids.size());
          if (!kept) continue;
          const auto it = std::find(ids.begin(), ids.end(), target);  // outermost occurrence
          if (it == ids.end()) continue;
          const size_t pos = static_cast<size_t>(it - ids.begin());
          const bool leaf = ids.back() == target;
          bf.self.inclusiveTicks += nw.ticks;
          bf.self.instances = saturate32(uint64_t{bf.self.instances} + nw.samples);
          if (leaf) bf.self.exclusiveTicks += nw.ticks;

          FunctionRow& caller = callers[pos > 0 ? ids[pos - 1] : rootId];
          caller.inclusiveTicks += nw.ticks;
          caller.instances = saturate32(uint64_t{caller.instances} + nw.samples);
          if (leaf) caller.exclusiveTicks += nw.ticks;

          if (pos + 1 < ids.size()) {
            FunctionRow& callee = callees[ids[pos + 1]];
            callee.inclusiveTicks += nw.ticks;
            callee.instances = saturate32(uint64_t{callee.instances} + nw.samples);
            if (pos + 2 == ids.size()) callee.exclusiveTicks += nw.ticks;
          }
        }
        auto collect = [&](std::unordered_map<uint32_t, FunctionRow>& m, std::vector<FunctionRow>& out) {
          out.reserve(m.size());
          for (auto& [id, row] : m) {
            row.key = table.key(id);
            out.push_back(row);
          }
          std::sort(out.begin(), out.end(), sortRowsByInclusive);
        };
        collect(callers, bf.callers);
        collect(callees, bf.callees);
        return bf;
      },
      butterflyBytes);
}

Result<Butterfly> butterfly(const QueryContext& ctx, const Filter& filter, FunctionKey key, Cancel cancel) {
  auto r = butterflyShared(ctx, filter, key, cancel);
  if (!r) return r.error();
  return **r;
}

std::string functionKeyName(const QueryContext& ctx, FunctionKey key) {
  if (key == kRootKey) return "<root>";
  if (key == kKernelKey) return "<kernel>";
  const model::TraceModel& m = ctx.model();
  if (key.isScope())
    return "scope:" + std::string(m.strings.get(StringIdx(static_cast<uint32_t>(key.value))));
  if (key.isUnmapped()) return "<unmapped>!" + hex(key.value);
  std::string mod = key.module.get() < m.modules.size()
                        ? std::string(baseName(m.modules[key.module.get()].path))
                        : "<module " + std::to_string(key.module.get()) + ">";
  const symbols::SymbolService& sym = ctx.symbols();
  if (key.isInline()) {
    return mod + "!" +
           std::string(sym.symbolStrings().get(StringIdx(static_cast<uint32_t>(key.value & ~kInlineKeyBit))));
  }
  if (symbols::ISymbolProvider* p = sym.provider(key.module)) {
    auto fb = p->functionBounds(key.value);
    if (fb && fb->startRva == key.value) return mod + "!" + std::string(sym.symbolStrings().get(fb->name));
  }
  return mod + "!" + hex(key.value);
}

}  // namespace tachyon::analysis
