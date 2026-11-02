#include "analysis/scope_blend.h"

#include "analysis/detail/cancellable_sort.h"
#include "analysis/detail/parallel.h"
#include "analysis/fibers.h"
#include "analysis/query_cache.h"
#include "analysis/query_executor.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <thread>
#include <unordered_map>

namespace tachyon::analysis {

size_t BlendIndex::heapBytes() const {
  size_t bytes = sizeof(BlendIndex) + vectorBytes(nodes) + vectorBytes(byThread);
  for (const auto& v : byThread) bytes += vectorBytes(v);
  return bytes;
}

std::vector<ScopeStackNode> BlendIndex::stackOf(uint32_t stack) const {
  std::vector<ScopeStackNode> out;
  for (uint32_t n = stack; n != 0 && n < nodes.size(); n = nodes[n].parent) out.push_back(nodes[n]);
  std::reverse(out.begin(), out.end());
  return out;
}

uint32_t ScopeCursor::locate(uint64_t ts) {
  const std::vector<ScopeSegment>& s = *m_seg;
  const size_t n = s.size();
  // Invariant after this block: s[m_pos - 1].end <= ts < s[m_pos].end (m_pos == n: ts is past every segment).
  if (m_pos > n || (m_pos > 0 && ts < s[m_pos - 1].end)) {
    m_pos = static_cast<size_t>(
        std::partition_point(s.begin(), s.end(), [&](const ScopeSegment& g) { return g.end <= ts; }) -
        s.begin());
  } else {
    int steps = 0;
    while (m_pos < n && s[m_pos].end <= ts) {
      if (++steps > 8) {  // a long jump: search the rest
        m_pos =
            static_cast<size_t>(std::partition_point(s.begin() + static_cast<std::ptrdiff_t>(m_pos), s.end(),
                                                     [&](const ScopeSegment& g) { return g.end <= ts; }) -
                                s.begin());
        break;
      }
      ++m_pos;
    }
  }
  if (m_pos < n && s[m_pos].begin <= ts) {  // inside a segment
    m_lo = s[m_pos].begin;
    m_hi = s[m_pos].end;
    m_stack = s[m_pos].stack;
  } else {  // in the gap before segment m_pos (or after the last one)
    m_lo = m_pos > 0 ? s[m_pos - 1].end : 0;
    m_hi = m_pos < n ? s[m_pos].begin : UINT64_MAX;
    m_stack = 0;
  }
  return m_stack;
}

namespace {

class TrieBuilder {
 public:
  explicit TrieBuilder(std::vector<ScopeStackNode>& nodes) : m_nodes(nodes) {
    if (m_nodes.empty()) m_nodes.push_back(ScopeStackNode{});
  }
  uint32_t child(uint32_t parent, StringIdx id, uint64_t caller) {
    const Key k{parent, id.get(), caller};
    auto [it, inserted] = m_map.try_emplace(k, static_cast<uint32_t>(m_nodes.size()));
    if (inserted) m_nodes.push_back(ScopeStackNode{parent, id, caller});
    return it->second;
  }

 private:
  struct Key {
    uint32_t parent, id;
    uint64_t caller;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
      uint64_t h = (uint64_t{k.parent} << 32 | k.id) * 0x9E3779B97F4A7C15ull;
      h ^= (k.caller + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
      return static_cast<size_t>(h ^ (h >> 29));
    }
  };
  std::vector<ScopeStackNode>& m_nodes;
  std::unordered_map<Key, uint32_t, KeyHash> m_map;
};

// Sweeps the instances of one lane (sorted by begin, properly nested) and calls emit(begin, end, node) for
// every maximal stretch with a constant non-empty open stack, in time order. Returns false when cancelled.
template <typename Emit>
bool sweepLane(const std::vector<ScopeInstance>& scopes, TrieBuilder& trie, Cancel cancel, size_t& polled,
               Emit&& emit) {
  struct Open {
    uint64_t end;
    uint32_t node;
  };
  std::vector<Open> st;
  uint32_t cur = 0;
  uint64_t segStart = 0;
  auto closeSeg = [&](uint64_t t) {
    if (cur != 0 && t > segStart) emit(segStart, t, cur);
  };
  auto popTo = [&](uint64_t t) {
    while (!st.empty() && st.back().end <= t) {
      const uint64_t e = st.back().end;
      closeSeg(e);
      st.pop_back();
      cur = st.empty() ? 0u : st.back().node;
      segStart = e;
    }
  };
  for (const ScopeInstance& s : scopes) {
    if ((++polled & 0xFFFFu) == 0 && cancel.requested()) return false;
    if (s.end <= s.begin) continue;  // never open at any timestamp
    popTo(s.begin);
    closeSeg(s.begin);
    cur = trie.child(cur, s.id, s.callerAddr);
    st.push_back(Open{s.end, cur});
    segStart = s.begin;
  }
  popTo(UINT64_MAX);
  return true;
}

}  // namespace

Result<BlendIndex> buildBlendIndex(const model::TraceModel& m, const PairedScopes& paired,
                                   const PairedFibers* fibers, Cancel cancel) {
  BlendIndex idx;
  idx.byThread.resize(m.threads.size());
  TrieBuilder trie(idx.nodes);
  size_t polled = 0;
  if (fibers != nullptr && fibers->fiberAware) {
    idx.byFiber = true;
    for (const FiberLane& lane : fibers->lanes) {
      if (cancel.requested()) return makeError(ErrorCode::Cancelled, "query cancelled");
      std::vector<ScopeSegment> logical;
      if (!sweepLane(lane.scopes, trie, cancel, polled,
                     [&](uint64_t b, uint64_t e, uint32_t n) { logical.push_back(ScopeSegment{b, e, n}); }))
        return makeError(ErrorCode::Cancelled, "query cancelled");
      // Intersect with the runs (both sorted and disjoint): the lane's scopes only show where it really ran.
      size_t r = 0;
      for (const ScopeSegment& g : logical) {
        if ((++polled & 0xFFFFu) == 0 && cancel.requested())
          return makeError(ErrorCode::Cancelled, "query cancelled");
        while (r < lane.runs.size() && lane.runs[r].end <= g.begin) ++r;
        for (size_t k = r; k < lane.runs.size() && lane.runs[k].begin < g.end; ++k) {
          const uint64_t b = std::max(g.begin, lane.runs[k].begin), e = std::min(g.end, lane.runs[k].end);
          const uint32_t t = lane.runs[k].thread.get();
          if (e > b && t < idx.byThread.size()) idx.byThread[t].push_back(ScopeSegment{b, e, g.stack});
        }
      }
    }
    for (std::vector<ScopeSegment>& v : idx.byThread) {
      if (!detail::cancellableSort(
              v, [](const ScopeSegment& a, const ScopeSegment& b) { return a.begin < b.begin; }, cancel))
        return makeError(ErrorCode::Cancelled, "query cancelled");
      uint64_t prevEnd = 0;
      size_t w = 0;
      for (size_t i = 0; i < v.size(); ++i) {  // lanes never overlap on a thread; guard against bad events
        if ((i & 0xFFFFu) == 0xFFFFu && cancel.requested())
          return makeError(ErrorCode::Cancelled, "query cancelled");
        ScopeSegment g = v[i];
        g.begin = std::max(g.begin, prevEnd);
        if (g.end <= g.begin) continue;
        prevEnd = g.end;
        v[w++] = g;
      }
      v.resize(w);
    }
  } else {
    for (size_t t = 0; t < paired.byThread.size() && t < idx.byThread.size(); ++t) {
      std::vector<ScopeSegment>& out = idx.byThread[t];
      if (!sweepLane(paired.byThread[t], trie, cancel, polled,
                     [&](uint64_t b, uint64_t e, uint32_t n) { out.push_back(ScopeSegment{b, e, n}); }))
        return makeError(ErrorCode::Cancelled, "query cancelled");
    }
  }
  for (const auto& v : idx.byThread) idx.segmentCount += v.size();
  for (auto& v : idx.byThread) v.shrink_to_fit();
  idx.nodes.shrink_to_fit();
  return idx;
}

Result<std::shared_ptr<const BlendIndex>> blendIndexFor(const QueryContext& ctx, Cancel cancel) {
  const bool fib = ctx.options().groupByFiber && hasFiberEvents(ctx.model());
  QueryKey k;
  k.kind = QueryKind::ScopePairs;
  k.arg = fib ? 3 : 2;
  QueryCache& cache = ctx.cache();
  if (auto hit = cache.find<BlendIndex>(k)) return hit;
  Result<BlendIndex> built = makeError(ErrorCode::Internal, "unset");
  if (fib) {
    auto pf = pairedFibersFor(ctx, cancel);
    if (!pf) return pf.error();
    built = buildBlendIndex(ctx.model(), (*pf)->scopes, pf->get(), cancel);
  } else {
    auto ps = pairedScopesFor(ctx, cancel);
    if (!ps) return ps.error();
    built = buildBlendIndex(ctx.model(), **ps, nullptr, cancel);
  }
  if (!built) return built.error();
  const size_t bytes = built->heapBytes() + 2 * kAllocOverhead + 32;
  auto value = std::make_shared<const BlendIndex>(std::move(*built));
  cache.insert(k, value, bytes);
  return value;
}

namespace {

// Open-addressing table (linear probing) from (stack, node, kernel flag) to ticks and sample counts. A per
// worker accumulator: no deletion, grows at 1/2 load.
class WeightTable {
 public:
  WeightTable() { reset(1u << 10); }

  void add(uint64_t key, uint64_t ticks) {
    size_t i = slotOf(key);
    while (m_slots[i].key != kEmpty && m_slots[i].key != key) i = (i + 1) & m_mask;
    if (m_slots[i].key == kEmpty) {
      m_slots[i].key = key;
      if (++m_count * 2 > m_slots.size()) {
        grow();
        i = slotOf(key);
        while (m_slots[i].key != key) i = (i + 1) & m_mask;
      }
    }
    m_slots[i].ticks += ticks;
    ++m_slots[i].samples;
  }

  template <typename Fn>
  void forEach(Fn&& fn) const {
    for (const Slot& s : m_slots)
      if (s.key != kEmpty) fn(s.key, s.ticks, s.samples);
  }

 private:
  static constexpr uint64_t kEmpty = ~uint64_t{0};
  struct Slot {  // one cache line access per sample
    uint64_t key = kEmpty;
    uint64_t ticks = 0;
    uint32_t samples = 0;
  };

  size_t slotOf(uint64_t key) const {
    return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> m_shift);  // the high bits mix every key bit
  }
  void reset(size_t n) {
    m_slots.assign(n, Slot{});
    m_mask = n - 1;
    m_shift = 64u - static_cast<unsigned>(std::countr_zero(n));
  }
  void grow() {
    std::vector<Slot> old = std::move(m_slots);
    reset(old.size() * 2);
    for (const Slot& o : old) {
      if (o.key == kEmpty) continue;
      size_t i = slotOf(o.key);
      while (m_slots[i].key != kEmpty) i = (i + 1) & m_mask;
      m_slots[i] = o;
    }
  }

  // Padding keeps the mutable fields of neighbouring workers' tables (they sit in one vector) on different
  // cache lines: false sharing made the accumulation twice as slow.
  [[maybe_unused]] char m_padHead[64] = {};
  std::vector<Slot> m_slots;
  size_t m_mask = 0, m_count = 0;
  unsigned m_shift = 0;
  [[maybe_unused]] char m_padTail[64] = {};
};

}  // namespace

Result<std::vector<NodeWeight>> accumulateBlendedWeights(const model::TraceModel& m, const BlendIndex& index,
                                                         uint64_t t0, uint64_t t1,
                                                         std::span<const ThreadIdx> threads, unsigned workers,
                                                         Cancel cancel, QueryExecutor* executor) {
  if (auto ok = checkAttributable(m); !ok) return ok.error();
  for (ThreadIdx t : threads)
    if (!t.valid() || t.get() >= m.threads.size())
      return makeError(ErrorCode::InvalidArgument, "thread index out of range");
  if (index.nodes.size() >= (size_t{1} << 31))
    return makeError(ErrorCode::InvalidArgument, "too many distinct scope stacks");
  const size_t nodeCount = std::max<size_t>(m.nodes.size(), 1);

  unsigned w = workers != 0 ? workers : std::max(1u, std::thread::hardware_concurrency());
  w = static_cast<unsigned>(std::min<size_t>(w, std::max<size_t>(threads.size(), 1)));
  if (executor != nullptr) w = std::min(w, executor->threadCount() + 1);
  w = std::max(w, 1u);

  std::vector<WeightTable> tables(w);
  std::atomic<size_t> next{0};
  std::atomic<bool> cancelled{false};
  static const std::vector<ScopeSegment> kNoSegments;

  auto work = [&](unsigned id) {
    WeightTable& tab = tables[id];
    for (;;) {
      const size_t k = next.fetch_add(1, std::memory_order_relaxed);
      if (k >= threads.size() || cancelled.load(std::memory_order_relaxed)) return;
      const ThreadIdx thread = threads[k];
      ScopeCursor cursor(thread.get() < index.byThread.size() ? index.byThread[thread.get()] : kNoSegments);
      const bool done = forEachWeightedSample(
          m, thread, t0, t1, cancel, [&](uint64_t ts, CallstackIdx cs, uint64_t weight, uint16_t flags) {
            uint64_t n = cs.get();
            if (n >= nodeCount) n = 0;
            const uint64_t kernel = (flags & trace::kSampleFlagKernelLeaf) != 0 ? 1u : 0u;
            const uint64_t stack = cursor.stackAt(ts);
            tab.add((stack << 33) | (n << 1) | kernel, weight);
          });
      if (!done) {
        cancelled.store(true, std::memory_order_relaxed);
        return;
      }
    }
  };
  detail::runSlots(executor, w, work);
  if (cancelled.load() || cancel.requested()) return makeError(ErrorCode::Cancelled, "query cancelled");

  struct Row {
    uint64_t key, ticks;
    uint64_t samples;
  };
  std::vector<Row> rows;
  for (const WeightTable& t : tables)
    t.forEach(
        [&](uint64_t key, uint64_t ticks, uint32_t samples) { rows.push_back(Row{key, ticks, samples}); });
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.key < b.key; });
  std::vector<NodeWeight> out;
  for (size_t i = 0; i < rows.size();) {
    Row acc = rows[i];
    size_t j = i + 1;
    for (; j < rows.size() && rows[j].key == acc.key; ++j) {
      acc.ticks += rows[j].ticks;
      acc.samples += rows[j].samples;
    }
    NodeWeight nw;
    nw.node = CallstackIdx(static_cast<uint32_t>((acc.key >> 1) & 0xFFFFFFFFu));
    nw.scopeStack = static_cast<uint32_t>(acc.key >> 33);
    nw.kernelLeaf = (acc.key & 1u) != 0;
    nw.ticks = acc.ticks;
    nw.samples = static_cast<uint32_t>(std::min<uint64_t>(acc.samples, UINT32_MAX));
    out.push_back(nw);
    i = j;
  }
  return out;
}

}  // namespace tachyon::analysis
