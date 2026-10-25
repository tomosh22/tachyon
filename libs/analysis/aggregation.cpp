#include "analysis/attribution.h"
#include "analysis/query.h"

#include <algorithm>
#include <cstdio>
#include <list>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace tachyon::analysis {

using symbols::kInlineKeyBit;
using symbols::kScopeModule;

namespace {

constexpr uint64_t kKernelBase = 0xFFFF800000000000ull;
constexpr size_t kCacheEntries = 4;
constexpr uint32_t kUnset = 0xFFFFFFFFu;

std::string_view baseName(std::string_view path) {
  const size_t p = path.find_last_of("/\\");
  return p == std::string_view::npos ? path : path.substr(p + 1);
}

bool isKernelModulePath(std::string_view path) {
  const std::string_view b = baseName(path);
  return b == "ntoskrnl.exe" || b == "ntkrnlmp.exe" || b == "ntkrnlpa.exe" || b == "vmlinux" ||
         b.substr(0, 7) == "[kernel";
}

std::string hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

// Interns FunctionKeys to dense ids for the duration of one query.
class KeyTable {
 public:
  uint32_t intern(const FunctionKey& k) {
    auto [it, inserted] = m_ids.try_emplace(k, static_cast<uint32_t>(m_keys.size()));
    if (inserted) m_keys.push_back(k);
    return it->second;
  }
  const FunctionKey& key(uint32_t id) const { return m_keys[id]; }
  size_t size() const { return m_keys.size(); }

 private:
  std::unordered_map<FunctionKey, uint32_t> m_ids;
  std::vector<FunctionKey> m_keys;
};

// 3.8 rule 3 / 3.7 FunctionKey: turns callstacks into root-first sequences of interned FunctionKeys,
// honouring inline expansion and kernel frame handling. Not thread-safe (memoizes per frame).
class StackFlattener {
 public:
  StackFlattener(const QueryContext& ctx, KeyTable& table)
      : m_ctx(ctx), m_model(ctx.model()), m_table(table), m_entries(ctx.model().frames.size()) {
    m_kernelId = m_table.intern(kKernelKey);
  }

  // Fills `out` (cleared first) with the keys of the stack of `node`, root first. With a valid `subtree`
  // (not node 0) the stack is cut above that node and false is returned when `node` is not below it.
  bool flatten(CallstackIdx node, CallstackIdx subtree, std::vector<uint32_t>& out) {
    out.clear();
    m_chain.clear();
    const size_t limit = m_model.nodes.size();
    size_t n = node.valid() ? node.get() : 0;
    while (n != 0 && n < limit && m_chain.size() <= limit) {
      m_chain.push_back(static_cast<uint32_t>(n));
      n = m_model.nodes[n].parent.get();
    }
    size_t first = m_chain.size();  // chain index of the topmost node to include, counting from the leaf
    if (subtree.valid() && subtree.get() != 0) {
      const auto it = std::find(m_chain.begin(), m_chain.end(), subtree.get());
      if (it == m_chain.end()) return false;
      first = static_cast<size_t>(it - m_chain.begin()) + 1;
    }
    const bool group = m_ctx.options().kernelFrames == KernelFrames::Group;
    for (size_t i = first; i-- > 0;) {
      const Entry& e = entryFor(m_model.nodes[m_chain[i]].frame);
      for (uint32_t k = 0; k < e.count; ++k) {
        const uint32_t id = m_flat[e.offset + k];
        if (group && id == m_kernelId && !out.empty() && out.back() == m_kernelId) continue;
        out.push_back(id);
      }
    }
    return true;
  }

 private:
  struct Entry {
    uint32_t offset = kUnset;
    uint32_t count = 0;
  };

  bool isKernel(const model::Frame& f) const {
    if (!f.module.valid()) return f.addr_or_rva >= kKernelBase;
    return f.module.get() < m_model.modules.size() &&
           isKernelModulePath(m_model.modules[f.module.get()].path);
  }

  const Entry& entryFor(FrameIdx frame) {
    static const Entry kEmpty{0, 0};
    if (!frame.valid() || frame.get() >= m_entries.size()) return kEmpty;
    Entry& e = m_entries[frame.get()];
    if (e.offset != kUnset) return e;
    e.offset = static_cast<uint32_t>(m_flat.size());
    const model::Frame& f = m_model.frames[frame.get()];
    const KernelFrames kf = m_ctx.options().kernelFrames;
    if (kf != KernelFrames::Show && isKernel(f)) {
      if (kf == KernelFrames::Group) {
        m_flat.push_back(m_kernelId);
        e.count = 1;
      }
      return e;
    }
    if (!f.module.valid()) {
      m_flat.push_back(m_table.intern(FunctionKey{ModuleIdx::invalid(), f.addr_or_rva}));
      e.count = 1;
      return e;
    }
    const symbols::ResolvedAddress& r = m_ctx.symbols().lookup(frame);
    if (!r.resolved) {
      m_flat.push_back(m_table.intern(FunctionKey{f.module, f.addr_or_rva}));
      e.count = 1;
      return e;
    }
    m_flat.push_back(m_table.intern(FunctionKey{f.module, r.functionStartRva}));
    e.count = 1;
    if (m_ctx.options().expandInline) {
      // Inline chain is innermost first; the stack is root first, so emit outermost inline first.
      for (size_t i = r.inlines.size(); i-- > 0;) {
        m_flat.push_back(m_table.intern(FunctionKey{f.module, kInlineKeyBit | r.inlines[i].function.get()}));
        ++e.count;
      }
    }
    return e;
  }

  const QueryContext& m_ctx;
  const model::TraceModel& m_model;
  KeyTable& m_table;
  std::vector<Entry> m_entries;
  std::vector<uint32_t> m_flat;
  std::vector<uint32_t> m_chain;
  uint32_t m_kernelId = 0;
};

struct ResolvedFilter {
  uint64_t t0 = 0, t1 = 0;
  std::vector<ThreadIdx> threads;  // sorted, unique
  CallstackIdx subtree;            // invalid = none
};

Result<ResolvedFilter> resolveFilter(const model::TraceModel& m, const Filter& f) {
  if (f.function) return makeError(ErrorCode::Unsupported, "Filter::function is not supported yet");
  if (auto ok = checkAttributable(m); !ok) return ok.error();
  ResolvedFilter r;
  r.t0 = f.t0;
  r.t1 = f.t1;
  if (f.threads) {
    r.threads = *f.threads;
    for (ThreadIdx t : r.threads)
      if (!t.valid() || t.get() >= m.threads.size())
        return makeError(ErrorCode::InvalidArgument, "Filter::threads holds an invalid ThreadIdx");
    std::sort(r.threads.begin(), r.threads.end());
    r.threads.erase(std::unique(r.threads.begin(), r.threads.end()), r.threads.end());
  } else {
    r.threads.reserve(m.threads.size());
    for (uint32_t t = 0; t < m.threads.size(); ++t) r.threads.push_back(ThreadIdx(t));
  }
  if (f.subtreeRoot) {
    if (f.subtreeRoot->valid() && f.subtreeRoot->get() >= m.nodes.size() && !m.nodes.empty())
      return makeError(ErrorCode::InvalidArgument, "Filter::subtreeRoot is out of range");
    if (f.subtreeRoot->valid()) r.subtree = *f.subtreeRoot;
  }
  return r;
}

void appendBytes(std::string& s, const void* p, size_t n) {
  s.append(static_cast<const char*>(p), n);
}

std::string weightsKey(const ResolvedFilter& f) {
  std::string k;
  appendBytes(k, &f.t0, sizeof f.t0);
  appendBytes(k, &f.t1, sizeof f.t1);
  for (ThreadIdx t : f.threads) {
    const uint32_t v = t.get();
    appendBytes(k, &v, sizeof v);
  }
  return k;
}

std::string rowsKey(const ResolvedFilter& f, const char* tag, const FunctionKey* fk = nullptr) {
  std::string k = weightsKey(f);
  const uint32_t s = f.subtree.get();
  appendBytes(k, &s, sizeof s);
  k += tag;
  if (fk) {
    const uint32_t m = fk->module.get();
    appendBytes(k, &m, sizeof m);
    appendBytes(k, &fk->value, sizeof fk->value);
  }
  return k;
}

template <typename V>
struct Lru {
  struct Entry {
    std::string key;
    std::shared_ptr<const V> value;
  };
  std::list<Entry> entries;

  std::shared_ptr<const V> find(const std::string& key) {
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (it->key == key) {
        entries.splice(entries.begin(), entries, it);
        return it->value;
      }
    }
    return nullptr;
  }
  void put(std::string key, std::shared_ptr<const V> value) {
    entries.push_front(Entry{std::move(key), std::move(value)});
    while (entries.size() > kCacheEntries) entries.pop_back();
  }
};

}  // namespace

struct QueryContext::Impl {
  const model::TraceModel& model;
  const symbols::SymbolService& symbols;
  AggregationOptions options;
  mutable std::mutex mutex;
  Lru<std::vector<NodeWeight>> weights;
  Lru<std::vector<FunctionRow>> rows;
  CacheStats stats;
};

QueryContext::QueryContext(const model::TraceModel& model, const symbols::SymbolService& symbols,
                           AggregationOptions options)
    : m_impl(new Impl{model, symbols, options, {}, {}, {}, {}}) {}
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

void QueryContext::clearCache() {
  std::lock_guard lock(m_impl->mutex);
  m_impl->weights.entries.clear();
  m_impl->rows.entries.clear();
}

CacheStats QueryContext::cacheStats() const {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->stats;
}

namespace {

Result<std::shared_ptr<const std::vector<NodeWeight>>> weightsFor(const QueryContext& ctx,
                                                                  const ResolvedFilter& f, Cancel cancel) {
  QueryContext::Impl& impl = ctx.impl();
  const std::string key = weightsKey(f);
  {
    std::lock_guard lock(impl.mutex);
    if (auto hit = impl.weights.find(key)) {
      ++impl.stats.hits;
      return hit;
    }
    ++impl.stats.misses;
  }
  if (f.t0 >= f.t1) return std::make_shared<const std::vector<NodeWeight>>();
  auto acc = accumulateNodeWeights(ctx.model(), f.t0, f.t1, f.threads, impl.options.threads, cancel);
  if (!acc) return acc.error();
  auto value = std::make_shared<const std::vector<NodeWeight>>(std::move(*acc));
  std::lock_guard lock(impl.mutex);
  impl.weights.put(key, value);
  return value;
}

uint32_t saturate32(uint64_t v) {
  return static_cast<uint32_t>(std::min<uint64_t>(v, UINT32_MAX));
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

constexpr size_t kCancelStride = 1024;

}  // namespace

Result<std::vector<FunctionRow>> bottomUpFunctions(const QueryContext& ctx, const Filter& filter,
                                                   Cancel cancel) {
  auto rf = resolveFilter(ctx.model(), filter);
  if (!rf) return rf.error();
  QueryContext::Impl& impl = ctx.impl();
  const std::string key = rowsKey(*rf, "bu");
  {
    std::lock_guard lock(impl.mutex);
    if (auto hit = impl.rows.find(key)) {
      ++impl.stats.hits;
      return *hit;
    }
  }
  auto w = weightsFor(ctx, *rf, cancel);
  if (!w) return w.error();

  KeyTable table;
  StackFlattener flat(ctx, table);
  std::vector<uint64_t> incl, excl;
  std::vector<uint64_t> inst;
  std::vector<uint32_t> stamp;
  std::vector<uint32_t> ids;
  uint32_t serial = 0;
  size_t counter = 0;
  for (const NodeWeight& nw : **w) {
    if (++counter % kCancelStride == 0 && cancel.requested())
      return makeError(ErrorCode::Cancelled, "query cancelled");
    if (!flat.flatten(nw.node, rf->subtree, ids) || ids.empty()) continue;
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
  {
    std::lock_guard lock(impl.mutex);
    impl.rows.put(key, std::make_shared<const std::vector<FunctionRow>>(rows));
  }
  return rows;
}

Result<NodeAgg> topDownTree(const QueryContext& ctx, const Filter& filter, Cancel cancel) {
  auto rf = resolveFilter(ctx.model(), filter);
  if (!rf) return rf.error();
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
  std::vector<TNode> tn(1);
  std::unordered_map<uint64_t, uint32_t> children;
  std::vector<uint32_t> ids;
  size_t counter = 0;
  for (const NodeWeight& nw : **w) {
    if (++counter % kCancelStride == 0 && cancel.requested())
      return makeError(ErrorCode::Cancelled, "query cancelled");
    if (!flat.flatten(nw.node, rf->subtree, ids)) continue;
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
    tn[i].incl += tn[i].excl;
    tn[i].count += tn[i].ownCount;
    if (i != 0) {
      tn[tn[i].parent].incl += tn[i].incl;
      tn[tn[i].parent].count += tn[i].count;
    }
  }
  // Materialize iteratively (deep recursion must not overflow the stack): every node's children are complete
  // by the time it is moved into its parent.
  std::vector<NodeAgg> out(tn.size());
  for (size_t i = tn.size(); i-- > 0;) {
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
  if (cancel.requested()) return makeError(ErrorCode::Cancelled, "query cancelled");
  NodeAgg root = std::move(out[0]);
  if (rf->subtree.valid() && root.children.size() == 1 && root.exclusiveTicks == 0 &&
      root.children[0].key != kRootKey)
    return std::move(root.children[0]);
  return root;
}

Result<Butterfly> butterfly(const QueryContext& ctx, const Filter& filter, FunctionKey key, Cancel cancel) {
  auto rf = resolveFilter(ctx.model(), filter);
  if (!rf) return rf.error();
  auto w = weightsFor(ctx, *rf, cancel);
  if (!w) return w.error();

  KeyTable table;
  StackFlattener flat(ctx, table);
  const uint32_t target = table.intern(key);
  const uint32_t rootId = table.intern(kRootKey);
  Butterfly bf;
  bf.self.key = key;
  std::unordered_map<uint32_t, FunctionRow> callers, callees;
  std::vector<uint32_t> ids;
  size_t counter = 0;
  for (const NodeWeight& nw : **w) {
    if (++counter % kCancelStride == 0 && cancel.requested())
      return makeError(ErrorCode::Cancelled, "query cancelled");
    if (!flat.flatten(nw.node, rf->subtree, ids)) continue;
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
