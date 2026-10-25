// Internal helpers shared by the query implementations (aggregation.cpp, stats.cpp, frames.cpp): key table,
// stack flattening (3.8 rules 2/3 with the return-address rule), filter canonicalisation, cancellation
// metering and the cached-or-compute pattern. Not part of the public API.
#pragma once

#include "analysis/attribution.h"
#include "analysis/query.h"
#include "analysis/query_cache.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tachyon::analysis::detail {

using symbols::kInlineKeyBit;
using symbols::kScopeModule;

inline constexpr uint32_t kUnset = 0xFFFFFFFFu;

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
      : m_ctx(ctx),
        m_model(ctx.model()),
        m_table(table),
        m_entries{std::vector<Entry>(ctx.model().frames.size()),
                  std::vector<Entry>(ctx.model().frames.size())} {
    m_kernelId = m_table.intern(kKernelKey);
  }

  // Fills `out` (cleared first) with the keys of the stack of `node`, root first. With a valid `subtree`
  // (not node 0) the stack is cut above that node and false is returned when `node` is not below it.
  // With a `required` key id the stack must also contain it (Filter::function); false otherwise.
  // `kernelLeaf`: the samples carry kSampleFlagKernelLeaf (return-address rule exception). With
  // KernelFrames::Hide a stack that ends up empty (empty, or only kernel frames) becomes the single frame
  // kKernelKey, so every view counts it.
  // Makes flatten() observe `cancel` while it walks a stack; it then returns false with aborted() set.
  void setCancel(Cancel cancel) { m_cancel = cancel; }
  bool aborted() const { return m_aborted; }
  // Frames visited by the last flatten() (the cost the callers meter cancellation by).
  size_t lastCost() const { return m_chain.size(); }

  bool flatten(CallstackIdx node, CallstackIdx subtree, bool kernelLeaf, std::vector<uint32_t>& out,
               uint32_t required = kUnset) {
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
    // Return-address rule: chain index 0 is the leaf and uses its exact address; every other frame is a
    // return address (rva - 1), except the first non-kernel frame of a kernel-leaf sample (interrupted IP).
    size_t exactAt = 0;
    if (kernelLeaf) {
      while (exactAt < m_chain.size() && isKernelNode(m_chain[exactAt])) ++exactAt;
    }
    for (size_t i = first; i-- > 0;) {
      // A single very deep stack costs O(depth) with a symbol lookup per new frame: stay cancellable.
      if ((first - i) % 2048 == 0 && m_cancel.requested()) {
        m_aborted = true;
        return false;
      }
      const Entry& e = entryFor(m_model.nodes[m_chain[i]].frame, i != 0 && i != exactAt);
      for (uint32_t k = 0; k < e.count; ++k) {
        const uint32_t id = m_flat[e.offset + k];
        if (group && id == m_kernelId && !out.empty() && out.back() == m_kernelId) continue;
        out.push_back(id);
      }
    }
    if (out.empty() && m_ctx.options().kernelFrames == KernelFrames::Hide) out.push_back(m_kernelId);
    // Function filter (Filter::function): only stacks that contain the function, as seen after the cut.
    if (required != kUnset && std::find(out.begin(), out.end(), required) == out.end()) return false;
    return true;
  }

 private:
  struct Entry {
    uint32_t offset = kUnset;
    uint32_t count = 0;
  };

  bool isKernel(const model::Frame& f) const { return isKernelFrame(m_model, f); }

  bool isKernelNode(uint32_t node) const {
    const FrameIdx f = m_model.nodes[node].frame;
    return f.valid() && f.get() < m_model.frames.size() && isKernel(m_model.frames[f.get()]);
  }

  const Entry& entryFor(FrameIdx frame, bool caller) {
    static const Entry kEmpty{0, 0};
    if (!frame.valid() || frame.get() >= m_entries[0].size()) return kEmpty;
    Entry& e = m_entries[caller ? 1 : 0][frame.get()];
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
    const symbols::ResolvedAddress& r =
        caller ? m_ctx.symbols().lookupCaller(frame) : m_ctx.symbols().lookup(frame);
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
  Cancel m_cancel;
  bool m_aborted = false;
  std::vector<Entry> m_entries[2];  // [0] exact address, [1] return address (caller variant)
  std::vector<uint32_t> m_flat;
  std::vector<uint32_t> m_chain;
  uint32_t m_kernelId = 0;
};

struct ResolvedFilter {
  uint64_t t0 = 0, t1 = 0;
  std::vector<ThreadIdx> threads;  // sorted, unique
  CallstackIdx subtree;            // invalid = none
  std::optional<FunctionKey> function;
  Filter canonical;  // normalised form of the request: equal requests give equal canonical filters
};

// Validates `f` and brings it into canonical form: an empty range is [0, 0), the thread list is sorted and
// unique (and absent when it names every thread), the root node as subtree root means no subtree.
inline Result<ResolvedFilter> resolveFilterBasic(const QueryContext& ctx, const Filter& f) {
  const model::TraceModel& m = ctx.model();
  ResolvedFilter r;
  r.t0 = f.t0;
  r.t1 = f.t1;
  if (r.t0 >= r.t1) r.t0 = r.t1 = 0;
  bool allThreads = true;
  if (f.threads) {
    r.threads = *f.threads;
    for (ThreadIdx t : r.threads)
      if (!t.valid() || t.get() >= m.threads.size())
        return makeError(ErrorCode::InvalidArgument, "Filter::threads holds an invalid ThreadIdx");
    std::sort(r.threads.begin(), r.threads.end());
    r.threads.erase(std::unique(r.threads.begin(), r.threads.end()), r.threads.end());
    allThreads = r.threads.size() == m.threads.size();
  } else {
    r.threads.reserve(m.threads.size());
    for (uint32_t t = 0; t < m.threads.size(); ++t) r.threads.push_back(ThreadIdx(t));
  }
  if (f.subtreeRoot) {
    if (f.subtreeRoot->valid() && f.subtreeRoot->get() >= m.nodes.size() && !m.nodes.empty())
      return makeError(ErrorCode::InvalidArgument, "Filter::subtreeRoot is out of range");
    if (f.subtreeRoot->valid() && f.subtreeRoot->get() != 0) r.subtree = *f.subtreeRoot;
  }
  if (f.function) {
    r.function = ctx.functions().key(*f.function);
    if (!r.function)
      return makeError(ErrorCode::InvalidArgument, "Filter::function is not a handle of this QueryContext");
  }
  r.canonical.t0 = r.t0;
  r.canonical.t1 = r.t1;
  if (!allThreads) r.canonical.threads = r.threads;
  if (r.subtree.valid()) r.canonical.subtreeRoot = r.subtree;
  r.canonical.function = f.function;
  return r;
}

// As resolveFilterBasic, and additionally requires a model that can be attributed (checkAttributable).
inline Result<ResolvedFilter> resolveFilter(const QueryContext& ctx, const Filter& f) {
  if (auto ok = checkAttributable(ctx.model()); !ok) return ok.error();
  return resolveFilterBasic(ctx, f);
}

inline QueryKey resultKey(const ResolvedFilter& f, QueryKind kind, FunctionKey fk = {}) {
  QueryKey k;
  k.kind = kind;
  k.function = fk;
  k.filter = f.canonical;
  return k;
}

inline uint32_t saturate32(uint64_t v) {
  return static_cast<uint32_t>(std::min<uint64_t>(v, UINT32_MAX));
}

// Node loops meter cancellation by work, not by node count: a stack costs O(depth) to flatten, so a fixed
// number of nodes can take arbitrarily long. Every node counts kNodeCost units plus its stack depth, and the
// flag is read each time kCancelWork units have accumulated (a node-sized loop still checks every ~1024
// nodes; sample scans check every 65536 samples).
inline constexpr size_t kCancelStride = 1024;  // nodes, for the O(1)-per-node loops
inline constexpr size_t kCancelWork = 4096;
inline constexpr size_t kNodeCost = 4;

struct CancelMeter {
  Cancel cancel;
  size_t work = 0;
  // True when the flag is up; reads it only once enough work has accumulated since the last read.
  bool due() {
    if (work < kCancelWork) return false;
    work = 0;
    return cancel.requested();
  }
  void add(size_t frames) { work += kNodeCost + frames; }
};

inline Error cancelledError() {
  return makeError(ErrorCode::Cancelled, "query cancelled");
}

// Finds `key` in the cache or computes it: compute() returns a Result<T>, bytes(const T&) its approximate
// size. Errors (including cancellation) are never cached.
template <typename T, typename Compute, typename Bytes>
Result<std::shared_ptr<const T>> cachedOrCompute(const QueryContext& ctx, const QueryKey& key,
                                                 Compute&& compute, Bytes&& bytes) {
  QueryCache& cache = ctx.cache();
  if (auto hit = cache.find<T>(key)) return hit;
  Result<T> r = compute();
  if (!r) return r.error();
  const size_t size = bytes(*r);
  auto value = std::make_shared<const T>(std::move(*r));
  cache.insert(key, value, size);
  return value;
}

}  // namespace tachyon::analysis::detail
