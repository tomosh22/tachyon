#include "analysis/lod.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using model::ThreadStateInterval;
using model::TraceModel;

constexpr uint32_t kInvalid = kInvalidIdx;
constexpr uint32_t kCatCount = 5;  // WaitCategory values
constexpr uint32_t kMaxLevelNumber = 56;
constexpr uint64_t kMaxTraceTicks = 1ull << 50;
constexpr uint32_t kMaxBaseBucketsHard = 1u << 26;

// Thread-safe: pure function. round(255 * num / den), saturating.
inline uint8_t q8(uint64_t num, uint64_t den) {
  if (den == 0) return 0;
  const uint64_t v = (num * 255 + den / 2) / den;
  return static_cast<uint8_t>(v > 255 ? 255 : v);
}

// Thread-safe: pure function. round(15 * num / den), saturating.
inline uint64_t q15(uint64_t num, uint64_t den) {
  if (den == 0) return 0;
  const uint64_t v = (num * 15 + den / 2) / den;
  return v > 15 ? 15 : v;
}

// ---- callstack node table ----
// depth, Euler-tour
// interval (O(1) ancestor test) and skew-binary jump pointers (O(log) level ancestor).
struct NodeTable {
  std::vector<uint32_t> parent, depth, jump, tin, tout;
  uint32_t maxDepth = 0;

  uint32_t leaf(CallstackIdx c) const { return c.get() < parent.size() ? c.get() : 0; }
  bool isAncestor(uint32_t a, uint32_t b) const { return tin[a] <= tin[b] && tin[b] <= tout[a]; }
  // Ancestor of v at depth d (d <= depth[v]).
  uint32_t ancestorAt(uint32_t v, uint32_t d) const {
    while (depth[v] > d) {
      const uint32_t j = jump[v];
      v = depth[j] >= d ? j : parent[v];
    }
    return v;
  }
  uint32_t lca(uint32_t a, uint32_t b) const {
    while (!isAncestor(a, b)) {
      const uint32_t j = jump[a];
      a = isAncestor(j, b) ? parent[a] : j;
    }
    return a;
  }
};

Result<NodeTable> buildNodeTable(const std::vector<model::CallstackNode>& nodes) {
  NodeTable t;
  const size_t n = std::max<size_t>(nodes.size(), 1);
  if (n >= kInvalid) return makeError(ErrorCode::Unsupported, "too many callstack nodes");
  t.parent.assign(n, 0);
  for (size_t i = 1; i < nodes.size(); ++i) {
    const CallstackIdx p = nodes[i].parent;
    t.parent[i] = (p.valid() && p.get() < n && p.get() != i) ? p.get() : 0;
  }
  // children in CSR form
  std::vector<uint32_t> off(n + 1, 0);
  for (size_t i = 1; i < n; ++i) ++off[t.parent[i] + 1];
  for (size_t i = 0; i < n; ++i) off[i + 1] += off[i];
  std::vector<uint32_t> child(off[n]);
  {
    std::vector<uint32_t> fill(off.begin(), off.end() - 1);
    for (size_t i = 1; i < n; ++i) child[fill[t.parent[i]]++] = static_cast<uint32_t>(i);
  }
  t.depth.assign(n, 0);
  t.jump.assign(n, 0);
  t.tin.assign(n, 0);
  t.tout.assign(n, 0);
  std::vector<uint32_t> cursor(off.begin(), off.end() - 1);
  std::vector<uint32_t> stack;
  stack.push_back(0);
  uint32_t timer = 1;  // tin[0] = 0
  while (!stack.empty()) {
    const uint32_t v = stack.back();
    if (cursor[v] < off[v + 1]) {
      const uint32_t c = child[cursor[v]++];
      t.depth[c] = t.depth[v] + 1;
      t.maxDepth = std::max(t.maxDepth, t.depth[c]);
      const uint32_t jp = t.jump[v];
      t.jump[c] = (t.depth[v] - t.depth[jp] == t.depth[jp] - t.depth[t.jump[jp]]) ? t.jump[jp] : v;
      t.tin[c] = timer++;
      stack.push_back(c);
    } else {
      t.tout[v] = timer - 1;
      stack.pop_back();
    }
  }
  if (timer != n) return makeError(ErrorCode::CorruptData, "callstack nodes contain a cycle");
  return t;
}

// ---- geometry ----
struct Geometry {
  uint64_t start = 0, end = 0, total = 1;
  std::vector<LodLevelInfo> levels;
  uint32_t step = 2;
  uint32_t platform = 0;
  // Fine strip level (see lod.h): 1/fineSplit of the base bucket width (a quarter, or a half when the base
  // level is L1), strip data only. fineCount == 0 when absent.
  uint32_t fineLevel = 0;
  uint32_t fineSplit = 4;  // fine buckets per base bucket (power of two)
  uint64_t fineWidth = 0;
  uint32_t fineCount = 0;
};

inline uint64_t widthOf(uint32_t level) {
  return 64ull << level;
}
inline uint64_t countAt(uint64_t total, uint32_t level) {
  const uint64_t w = widthOf(level);
  return (total + w - 1) / w;
}

// ---- per-thread level data ----
struct BandCell {
  uint8_t running, ready, waiting, category;
};
struct StripCell {
  uint8_t share, coverage;
};
struct LevelArrays {
  std::vector<BandCell> band;
  std::vector<uint32_t> dominant;  // leaf node, kInvalid when no sample
  std::vector<StripCell> strip;
  std::vector<uint32_t> lca;  // kInvalid when no sample
  std::vector<uint16_t> maxDepth;
};
// One fine strip bucket in 32 bits: bits 0..23 the dominant leaf (kFineNone when the bucket holds no sample),
// bits 24..27 its share of the sampled ticks and bits 28..31 the sampled fraction of the bucket, both in
// 1/15.
constexpr uint32_t kFineNone = 0x00FFFFFFu;
constexpr uint32_t kFineMaxNodes = kFineNone;  // node ids must stay below this
inline uint32_t fineDominant(uint32_t v) {
  return v & kFineNone;
}
inline uint32_t fineShare15(uint32_t v) {
  return (v >> 24) & 15u;
}
inline uint32_t fineCoverage15(uint32_t v) {
  return v >> 28;
}
struct ThreadLod {
  std::vector<LevelArrays> levels;
  std::vector<uint32_t> fine;  // fineCount cells, empty without a fine level
};

// ---- sample view ----
struct SampleView {
  const uint64_t* ts = nullptr;
  const CallstackIdx* cs = nullptr;
  size_t n = 0;
  uint64_t interval = 1;
  uint64_t start = 0, end = 0;

  uint64_t extentStart(size_t i) const { return ts[i] > start ? ts[i] : start; }
  uint64_t extentEnd(size_t i) const {
    uint64_t e = ts[i] + interval;
    if (i + 1 < n && ts[i + 1] < e) e = ts[i + 1];
    return e < end ? e : end;
  }
  // First sample whose extent may reach past t0.
  size_t firstAfter(uint64_t t0) const {
    const uint64_t low = t0 >= interval ? t0 - interval + 1 : 0;
    return static_cast<size_t>(std::lower_bound(ts, ts + n, low) - ts);
  }
};

SampleView sampleView(const TraceModel& m, const Geometry& g, size_t thread) {
  SampleView v;
  if (thread < m.samplesByThread.size()) {
    const auto& s = m.samplesByThread[thread];
    v.n = std::min(s.ts.size(), s.callstack.size());
    v.ts = s.ts.data();
    v.cs = s.callstack.data();
  }
  v.interval = std::max<uint64_t>(m.sampleIntervalTicks, 1);
  v.start = g.start;
  v.end = g.end;
  return v;
}

const std::vector<ThreadStateInterval>& statesOf(const TraceModel& m, size_t thread) {
  static const std::vector<ThreadStateInterval> kEmpty;
  return thread < m.statesByThread.size() ? m.statesByThread[thread] : kEmpty;
}

// ---- pixel column cursor ----
// Walks the columns of a query in increasing order without per-column divisions (Bresenham on the
// floor(c * W / P) edges). Offsets are relative to t0.
class ColumnCursor {
 public:
  ColumnCursor(uint64_t t0, uint64_t t1, uint32_t pixelWidth)
      : m_p(pixelWidth), m_w(t1 - t0), m_q(m_w / pixelWidth), m_r(static_cast<uint32_t>(m_w % pixelWidth)) {}

  // u = offset of a tick in [0, W): positions on the last column whose begin is <= u.
  void seek(uint64_t u) {
    m_c = static_cast<uint32_t>(((u + 1) * m_p - 1) / m_w);
    const uint64_t cr = static_cast<uint64_t>(m_c) * m_r;
    m_begin = static_cast<uint64_t>(m_c) * m_q + cr / m_p;
    m_remBegin = static_cast<uint32_t>(cr % m_p);
    computeEnd();
  }
  void next() {
    ++m_c;
    m_begin = m_end;
    m_remBegin = m_remEnd;
    computeEnd();
  }
  uint32_t column() const { return m_c; }
  uint64_t begin() const { return m_begin; }
  uint64_t end() const { return m_end; }
  uint64_t width() const { return m_end - m_begin; }

 private:
  // Branch-free: whether the column is one tick wider is data dependent and would mispredict.
  void computeEnd() {
    const uint32_t rem = m_remBegin + m_r;
    const uint32_t carry = rem >= m_p ? 1u : 0u;
    m_remEnd = rem - (m_p & (0u - carry));
    m_end = m_begin + m_q + carry;
  }
  uint32_t m_p;
  uint64_t m_w, m_q;
  uint32_t m_r;
  uint32_t m_c = 0;
  uint64_t m_begin = 0, m_end = 0;
  uint32_t m_remBegin = 0, m_remEnd = 0;
};

// Walks the pixel columns of a query. colEnd is the absolute end of the current column; `active` is set once
// a column was entered. The caller flushes the last active column itself.
struct Sweep {
  ColumnCursor cur;
  uint64_t t0;
  uint64_t colEnd = 0;
  bool active = false;

  Sweep(uint64_t from, uint64_t to, uint32_t pixels) : cur(from, to, pixels), t0(from) {}
  void seek(uint64_t tick) {
    cur.seek(tick - t0);
    colEnd = t0 + cur.end();
    active = true;
  }
  void advance() {
    cur.next();
    colEnd = t0 + cur.end();
  }
};

// Moves to the column containing `lo` (calls must have non-decreasing `lo`), calling flush(cursor) for the
// column that is left.
template <class Flush>
inline void moveTo(Sweep& sw, uint64_t lo, Flush&& flush) {
  if (lo < sw.colEnd) return;
  if (sw.active) {
    flush(sw.cur);
    sw.advance();
    if (lo >= sw.colEnd) sw.seek(lo);
  } else {
    sw.seek(lo);
  }
}

// Splits the piece [lo, hi) (inside [t0, t1)) over pixel columns, calling piece(column, ticks) per column
// and flush(cursor) when a column is left. Pieces must arrive in increasing order.
template <class Piece, class Flush>
inline void splitInto(Sweep& sw, uint64_t lo, uint64_t hi, Piece&& piece, Flush&& flush) {
  moveTo(sw, lo, flush);
  if (hi <= sw.colEnd) {
    piece(sw.cur.column(), hi - lo);
    return;
  }
  for (;;) {
    const uint64_t segEnd = hi < sw.colEnd ? hi : sw.colEnd;
    if (segEnd > lo) piece(sw.cur.column(), segEnd - lo);
    lo = segEnd;
    if (lo >= hi) break;
    flush(sw.cur);
    sw.advance();
  }
}

// ---- build ----
struct Entry {
  uint64_t ticks;
  uint32_t cs;
};
inline bool heavier(const Entry& a, const Entry& b) {
  return a.ticks > b.ticks || (a.ticks == b.ticks && a.cs < b.cs);
}
void prune(std::vector<Entry>& h, size_t cap) {
  if (h.size() <= cap) return;
  std::nth_element(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(cap), h.end(), heavier);
  h.resize(cap);
  std::sort(h.begin(), h.end(), [](const Entry& a, const Entry& b) { return a.cs < b.cs; });
}

struct Agg {
  uint64_t running = 0, ready = 0, waiting = 0;
  uint64_t cat[kCatCount] = {};
  uint64_t cover = 0;
  uint32_t lca = kInvalid;
  uint32_t maxDepth = 0;
  std::vector<Entry> hist;  // sorted by cs once finished

  void reset() {
    running = ready = waiting = cover = 0;
    std::memset(cat, 0, sizeof cat);
    lca = kInvalid;
    maxDepth = 0;
    hist.clear();
  }
};

class ThreadBuilder {
 public:
  ThreadBuilder(const TraceModel& m, const Geometry& g, const NodeTable& nt, const LodOptions& opt,
                LodCancel cancel)
      : m_m(m), m_g(g), m_nt(nt), m_cap(std::max<uint32_t>(opt.maxStacksPerBucket, 1)), m_cancel(cancel) {}

  Result<void> run(size_t thread, ThreadLod& out) {
    const size_t levels = m_g.levels.size();
    out.levels.assign(levels, {});
    out.fine.assign(m_g.fineCount, kFineNone);
    for (size_t l = 0; l < levels; ++l) {
      const size_t n = m_g.levels[l].bucketCount;
      LevelArrays& a = out.levels[l];
      a.band.assign(n, BandCell{0, 0, 0, kNoWaitCategory});
      a.dominant.assign(n, kInvalid);
      a.strip.assign(n, StripCell{0, 0});
      a.lca.assign(n, kInvalid);
      a.maxDepth.assign(n, 0);
    }
    m_out = &out;
    m_acc.assign(levels, Agg{});
    const bool fine = m_g.fineCount > 0;
    const uint64_t fineW = fine ? m_g.fineWidth : 0;

    const SampleView sv = sampleView(m_m, m_g, thread);
    const auto& ivs = statesOf(m_m, thread);
    const uint64_t w0 = m_g.levels[0].bucketWidth;
    const uint32_t n0 = m_g.levels[0].bucketCount;
    size_t k = 0, j = 0;
    Agg cur;
    for (auto& h : fineHist) h.reserve(8);
    for (uint32_t b = 0; b < n0; ++b) {
      if ((b & 4095u) == 0 && m_cancel && m_cancel->load(std::memory_order_relaxed))
        return makeError(ErrorCode::Cancelled, "LOD build cancelled");
      const uint64_t bs = m_g.start + static_cast<uint64_t>(b) * w0;
      const uint64_t be = std::min(bs + w0, m_g.end);
      cur.reset();
      if (fine) {
        for (uint32_t q = 0; q < m_g.fineSplit; ++q) {
          fineHist[q].clear();
          fineCover[q] = 0;
        }
      }
      while (j < ivs.size() && ivs[j].end <= bs) ++j;
      for (size_t q = j; q < ivs.size() && ivs[q].start < be; ++q) {
        const ThreadStateInterval& iv = ivs[q];
        const uint64_t lo = std::max(iv.start, bs), hi = std::min(iv.end, be);
        if (hi <= lo) continue;
        const uint64_t ticks = hi - lo;
        switch (iv.state) {
          case ThreadState::Running: cur.running += ticks; break;
          case ThreadState::Ready: cur.ready += ticks; break;
          case ThreadState::Waiting:
            cur.waiting += ticks;
            cur.cat[static_cast<uint32_t>(waitReasonCategory(m_g.platform, iv.wait_reason))] += ticks;
            break;
          case ThreadState::Unknown: break;
        }
      }
      while (k < sv.n && sv.extentEnd(k) <= bs) ++k;
      for (size_t i = k; i < sv.n && sv.ts[i] < be; ++i) {
        const uint64_t lo = std::max(sv.extentStart(i), bs), hi = std::min(sv.extentEnd(i), be);
        if (hi <= lo) continue;
        const uint64_t ticks = hi - lo;
        const uint32_t leaf = m_nt.leaf(sv.cs[i]);
        addHist(cur, leaf, ticks);
        cur.cover += ticks;
        if (cur.lca == kInvalid)
          cur.lca = leaf;
        else if (!m_nt.isAncestor(cur.lca, leaf))
          cur.lca = m_nt.lca(cur.lca, leaf);
        cur.maxDepth = std::max(cur.maxDepth, m_nt.depth[leaf]);
        if (fine) {  // the same piece split at the fine bucket edges of the base bucket
          const uint32_t q0 = static_cast<uint32_t>((lo - bs) / fineW);
          const uint32_t q1 = static_cast<uint32_t>((hi - 1 - bs) / fineW);
          for (uint32_t q = q0; q <= q1; ++q) {
            const uint64_t qs = bs + q * fineW;
            addFine(q, leaf, std::min(hi, qs + fineW) - std::max(lo, qs));
          }
        }
      }
      std::sort(cur.hist.begin(), cur.hist.end(), [](const Entry& a, const Entry& c) { return a.cs < c.cs; });
      if (fine) {
        for (uint32_t q = 0; q < m_g.fineSplit; ++q) emitFine(static_cast<uint64_t>(b) * m_g.fineSplit + q, q);
      }
      propagate(0, b, cur);
    }
    return {};
  }

 private:
  void addHist(Agg& a, uint32_t leaf, uint64_t ticks) {
    for (Entry& e : a.hist) {
      if (e.cs == leaf) {
        e.ticks += ticks;
        return;
      }
    }
    a.hist.push_back({ticks, leaf});
    if (a.hist.size() > 4u * m_cap) {
      std::sort(a.hist.begin(), a.hist.end(), [](const Entry& x, const Entry& y) { return x.cs < y.cs; });
      prune(a.hist, m_cap);
    }
  }

  void addFine(uint32_t half, uint32_t leaf, uint64_t ticks) {
    fineCover[half] += ticks;
    for (Entry& e : fineHist[half]) {
      if (e.cs == leaf) {
        e.ticks += ticks;
        return;
      }
    }
    fineHist[half].push_back({ticks, leaf});
  }

  // Writes fine bucket `idx` (no-op beyond the trace) from the tallies of part `half` (0..fineSplit-1) of the
  // current base bucket.
  void emitFine(uint64_t idx, uint32_t half) {
    if (idx >= m_g.fineCount || fineCover[half] == 0) return;
    const Entry* best = &fineHist[half][0];
    for (const Entry& e : fineHist[half])
      if (e.ticks > best->ticks || (e.ticks == best->ticks && e.cs < best->cs)) best = &e;
    const uint64_t ew = std::min(m_g.fineWidth, m_g.total - idx * m_g.fineWidth);
    const uint32_t share = static_cast<uint32_t>(q15(best->ticks, fineCover[half]));
    const uint32_t cover = std::max<uint32_t>(static_cast<uint32_t>(q15(fineCover[half], ew)), 1);
    m_out->fine[idx] = best->cs | (share << 24) | (cover << 28);
  }

  void emit(size_t level, uint32_t idx, const Agg& a) {
    const LodLevelInfo& li = m_g.levels[level];
    const uint64_t ew = std::min(li.bucketWidth, m_g.total - static_cast<uint64_t>(idx) * li.bucketWidth);
    LevelArrays& arr = m_out->levels[level];
    uint8_t cat = kNoWaitCategory;
    if (a.waiting > 0) {
      uint64_t best = 0;
      for (uint32_t c = 0; c < kCatCount; ++c) {
        if (a.cat[c] > best) {
          best = a.cat[c];
          cat = static_cast<uint8_t>(c);
        }
      }
    }
    arr.band[idx] = BandCell{q8(a.running, ew), q8(a.ready, ew), q8(a.waiting, ew), cat};
    if (a.cover > 0 && !a.hist.empty()) {
      const Entry* best = &a.hist[0];
      for (const Entry& e : a.hist)
        if (e.ticks > best->ticks) best = &e;
      arr.dominant[idx] = best->cs;
      // A bucket with any sample never reads as zero coverage.
      arr.strip[idx] = StripCell{q8(best->ticks, a.cover), std::max<uint8_t>(q8(a.cover, ew), 1)};
      arr.lca[idx] = a.lca;
      arr.maxDepth[idx] = static_cast<uint16_t>(std::min<uint32_t>(a.maxDepth, 65535));
    }
  }

  void merge(Agg& dst, const Agg& src) {
    dst.running += src.running;
    dst.ready += src.ready;
    dst.waiting += src.waiting;
    for (uint32_t c = 0; c < kCatCount; ++c) dst.cat[c] += src.cat[c];
    dst.cover += src.cover;
    if (src.lca != kInvalid) {
      if (dst.lca == kInvalid)
        dst.lca = src.lca;
      else if (!m_nt.isAncestor(dst.lca, src.lca))
        dst.lca = m_nt.lca(dst.lca, src.lca);
    }
    dst.maxDepth = std::max(dst.maxDepth, src.maxDepth);
    if (src.hist.empty()) return;
    if (dst.hist.empty()) {
      dst.hist = src.hist;
      return;
    }
    m_scratch.clear();
    size_t a = 0, b = 0;
    while (a < dst.hist.size() && b < src.hist.size()) {
      if (dst.hist[a].cs < src.hist[b].cs) {
        m_scratch.push_back(dst.hist[a++]);
      } else if (src.hist[b].cs < dst.hist[a].cs) {
        m_scratch.push_back(src.hist[b++]);
      } else {
        m_scratch.push_back({dst.hist[a].ticks + src.hist[b].ticks, dst.hist[a].cs});
        ++a;
        ++b;
      }
    }
    while (a < dst.hist.size()) m_scratch.push_back(dst.hist[a++]);
    while (b < src.hist.size()) m_scratch.push_back(src.hist[b++]);
    prune(m_scratch, m_cap);
    dst.hist.swap(m_scratch);
  }

  void propagate(size_t level, uint32_t idx, const Agg& a) {
    emit(level, idx, a);
    if (level + 1 >= m_g.levels.size()) return;
    Agg& parent = m_acc[level + 1];
    merge(parent, a);
    const uint32_t fanMask = (1u << m_g.step) - 1;
    if (((idx + 1) & fanMask) == 0 || idx + 1 == m_g.levels[level].bucketCount) {
      Agg done = std::move(parent);
      parent = Agg{};
      propagate(level + 1, idx >> m_g.step, done);
    }
  }

  const TraceModel& m_m;
  const Geometry& m_g;
  const NodeTable& m_nt;
  size_t m_cap;
  LodCancel m_cancel;
  ThreadLod* m_out = nullptr;
  std::vector<Agg> m_acc;
  std::vector<Entry> m_scratch;
  std::vector<Entry> fineHist[4];
  uint64_t fineCover[4] = {0, 0, 0, 0};
};

// Stable counting sort of the closed spans by depth (closing order is time order within a depth).
void sortByDepth(const std::vector<TimelineSpan>& raw, uint32_t d0, uint32_t depthCount,
                 std::vector<TimelineSpan>& out) {
  out.clear();
  if (raw.empty()) return;
  std::vector<uint32_t> offset(static_cast<size_t>(depthCount) + 1, 0);
  for (const TimelineSpan& s : raw) ++offset[s.depth - d0 + 1];
  for (size_t i = 0; i < depthCount; ++i) offset[i + 1] += offset[i];
  out.resize(raw.size());
  for (const TimelineSpan& s : raw) out[offset[s.depth - d0]++] = s;
}

// ---- flame emitter ----
// Consumes per-column (lca, maxDepth) cells in increasing column order and produces spans per depth.
class FlameEmitter {
 public:
  // `raw` receives the closed runs in closing order (columns in t0/t1, kind in mergedCount); it is cleared.
  FlameEmitter(const NodeTable& nt, uint32_t d0, uint32_t depthCount, std::vector<TimelineSpan>& raw)
      : m_nt(nt), m_d0(d0), m_count(depthCount), m_run(depthCount), m_nodeAt(depthCount, 0), m_raw(raw) {
    raw.clear();
  }

  void column(uint32_t c, bool any, uint32_t lca, uint32_t md) {
    if (static_cast<int64_t>(c) != m_prevC + 1) {
      if (m_prevC >= 0) closeAll(static_cast<uint32_t>(m_prevC + 1));
      m_hasPrev = false;
    } else if (m_hasPrev && any == m_pAny && lca == m_pLca && md == m_pMd) {
      m_prevC = c;
      return;
    }
    m_prevC = c;
    m_hasPrev = true;
    m_pAny = any;
    m_pLca = lca;
    m_pMd = md;

    int32_t mdK = -1;
    uint32_t dl = 0;
    if (any) {
      dl = m_nt.depth[lca];
      if (md >= m_d0) mdK = static_cast<int32_t>(std::min<uint32_t>(md - m_d0, m_count - 1));
    }
    const int32_t hiK = std::max(mdK, m_openMax);
    if (hiK < 0) return;
    if (any && dl >= m_d0) {
      const uint32_t top = std::min(dl, m_d0 + static_cast<uint32_t>(hiK));
      uint32_t v = m_nt.ancestorAt(lca, top);
      for (uint32_t d = top;; --d) {
        m_nodeAt[d - m_d0] = v;
        v = m_nt.parent[v];
        if (d == m_d0) break;
      }
    }
    for (int32_t k = 0; k <= hiK; ++k) {
      const uint32_t d = m_d0 + static_cast<uint32_t>(k);
      Run& run = m_run[static_cast<size_t>(k)];
      if (!any || d > md) {
        if (run.kind) {
          close(static_cast<uint32_t>(k), run, c);
          run.kind = 0;
        }
        continue;
      }
      if (d <= dl) {
        const uint32_t node = m_nodeAt[static_cast<size_t>(k)];
        if (run.kind == 1 && run.node == node) continue;
        if (run.kind) close(static_cast<uint32_t>(k), run, c);
        run = Run{node, c, 1};
      } else {
        if (run.kind == 2) continue;
        if (run.kind) close(static_cast<uint32_t>(k), run, c);
        run = Run{kInvalid, c, 2};
      }
    }
    m_openMax = -1;
    for (int32_t k = hiK; k >= 0; --k) {
      if (m_run[static_cast<size_t>(k)].kind) {
        m_openMax = k;
        break;
      }
    }
  }

  // Closes all runs and writes the spans sorted by (depth, t0) to `out`. countStarts(c0, c1, t0, t1) counts
  // the samples starting in columns [c0, c1) = ticks [t0, t1); `edges` is reused scratch.
  template <class CountStarts>
  void finish(uint64_t t0, uint64_t t1, uint32_t pixelWidth, std::vector<uint64_t>& edges,
              CountStarts&& countStarts, std::vector<TimelineSpan>& out) {
    if (m_prevC >= 0) closeAll(static_cast<uint32_t>(m_prevC + 1));
    sortByDepth(m_raw, m_d0, m_count, out);
    if (out.empty()) return;

    const bool table = out.size() > 48;
    if (table) {
      edges.resize(static_cast<size_t>(pixelWidth) + 1);
      const uint64_t w = t1 - t0, q = w / pixelWidth, r = w % pixelWidth;
      uint64_t e = t0, rem = 0;
      for (uint32_t c = 0; c <= pixelWidth; ++c) {
        edges[c] = e;
        e += q;
        rem += r;
        if (rem >= pixelWidth) {
          rem -= pixelWidth;
          ++e;
        }
      }
    }
    for (TimelineSpan& s : out) {
      const auto c0 = static_cast<uint32_t>(s.t0), c1 = static_cast<uint32_t>(s.t1);
      const uint32_t kind = s.mergedCount;
      s.t0 = table ? edges[c0] : pixelEdge(t0, t1, pixelWidth, c0);
      s.t1 = table ? edges[c1] : pixelEdge(t0, t1, pixelWidth, c1);
      s.mergedCount = kind == 1 ? 1 : countStarts(c0, c1, s.t0, s.t1);
    }
  }

 private:
  struct Run {
    uint32_t node = kInvalid;
    uint32_t start = 0;
    uint8_t kind = 0;  // 0 none, 1 uniform, 2 mixed
  };
  void close(uint32_t k, const Run& run, uint32_t endCol) {
    TimelineSpan s;
    s.depth = m_d0 + k;
    s.t0 = run.start;
    s.t1 = endCol;
    s.node = CallstackIdx(run.kind == 1 ? run.node : kInvalid);
    s.mergedCount = run.kind;
    m_raw.push_back(s);
  }
  void closeAll(uint32_t endCol) {
    for (int32_t k = 0; k <= m_openMax; ++k) {
      Run& run = m_run[static_cast<size_t>(k)];
      if (run.kind) {
        close(static_cast<uint32_t>(k), run, endCol);
        run.kind = 0;
      }
    }
    m_openMax = -1;
  }

  const NodeTable& m_nt;
  uint32_t m_d0, m_count;
  std::vector<Run> m_run;
  std::vector<uint32_t> m_nodeAt;
  std::vector<TimelineSpan>& m_raw;
  int32_t m_openMax = -1;
  int64_t m_prevC = -1;
  bool m_hasPrev = false, m_pAny = false;
  uint32_t m_pLca = kInvalid, m_pMd = 0;
};

// Exact regime (a pixel is narrower than a sample interval): per depth, runs of consecutive contiguous
// samples with the same prefix node, spans at exact tick edges; runs narrower than a pixel are merged with
// their narrow neighbours (gap < one pixel).
class RunEmitter {
 public:
  // `bins` receives the spans per depth (index = depth - d0); the vectors keep their capacity between
  // queries.
  RunEmitter(const NodeTable& nt, uint32_t d0, uint32_t depthCount, uint64_t t0, uint64_t t1, uint32_t pixels,
             std::vector<std::vector<TimelineSpan>>& bins)
      : m_nt(nt),
        m_d0(d0),
        m_count(depthCount),
        m_t0(t0),
        m_t1(t1),
        m_narrow((t1 - t0 + pixels - 1) / pixels),
        m_d(depthCount),
        m_bins(bins) {
    if (bins.size() < depthCount) bins.resize(depthCount);
    for (uint32_t k = 0; k < depthCount; ++k) bins[k].clear();
  }

  // Samples must arrive in time order; [s, e) is the (non-empty) extent.
  void sample(uint64_t s, uint64_t e, uint32_t leaf) {
    const uint32_t dl = m_nt.depth[leaf];
    int32_t topK = -1;
    const uint32_t* path = nullptr;
    if (dl >= m_d0) {
      topK = static_cast<int32_t>(std::min<uint32_t>(dl - m_d0, m_count - 1));
      path = pathOf(leaf, topK);
    }
    const int32_t hiK = std::max(topK, m_openMax);
    int32_t k = 0;
    for (; k <= topK; ++k) {
      Depth& d = m_d[static_cast<size_t>(k)];
      const uint32_t node = path[k];
      if (d.open) {
        if (d.node == node && d.end == s) {
          d.end = e;
          ++d.samples;
          continue;
        }
        if (d.gRuns == 0 && d.start >= m_t0 && d.end <= m_t1 && d.end - d.start >= m_narrow) {
          // common case: a wide run inside the window with no narrow runs pending
          TimelineSpan& sp = m_bins[static_cast<size_t>(k)].emplace_back();
          sp.depth = m_d0 + static_cast<uint32_t>(k);
          sp.t0 = d.start;
          sp.t1 = d.end;
          sp.node = CallstackIdx(d.node);
          sp.mergedCount = 1;
        } else {
          closeRun(static_cast<uint32_t>(k));
        }
      }
      d.open = true;
      d.node = node;
      d.start = s;
      d.end = e;
      d.samples = 1;
    }
    for (; k <= hiK; ++k) {
      if (m_d[static_cast<size_t>(k)].open) closeRun(static_cast<uint32_t>(k));
    }
    m_openMax = topK;  // depths above topK were closed, so the deepest open run is topK
  }

  void finish() {
    for (int32_t k = 0; k <= m_openMax; ++k)
      if (m_d[static_cast<size_t>(k)].open) closeRun(static_cast<uint32_t>(k));
    m_openMax = -1;
    for (uint32_t k = 0; k < m_count; ++k) flushGroup(k);
  }

 private:
  struct Depth {
    uint32_t node = 0, samples = 0;
    uint64_t start = 0, end = 0;
    bool open = false;
    // pending group of narrow runs
    uint64_t gStart = 0, gEnd = 0;
    uint32_t gNode = 0, gRuns = 0, gSamples = 0;
  };
  // A run is narrow when it is shorter than one pixel: ticks * pixels < window.
  bool narrow(uint64_t ticks) const { return ticks < m_narrow; }

  // Nodes at depths d0 .. d0 + topK of the path to `leaf`, cached per leaf (a thread has few distinct
  // leaves).
  const uint32_t* pathOf(uint32_t leaf, int32_t topK) {
    const uint32_t slot = (leaf * 2654435761u) >> (32 - kCacheBits);
    uint32_t* nodes = m_paths.data() + static_cast<size_t>(slot) * m_count;
    if (m_cacheLeaf[slot] != leaf) {
      m_cacheLeaf[slot] = leaf;
      uint32_t v = m_nt.ancestorAt(leaf, m_d0 + static_cast<uint32_t>(topK));
      for (int32_t k = topK; k >= 0; --k) {
        nodes[k] = v;
        v = m_nt.parent[v];
      }
    }
    return nodes;
  }

  void emit(uint32_t k, uint64_t start, uint64_t end, uint32_t node, uint32_t merged) {
    const uint64_t a = start > m_t0 ? start : m_t0, b = end < m_t1 ? end : m_t1;
    if (b <= a) return;
    TimelineSpan sp;
    sp.depth = m_d0 + k;
    sp.t0 = a;
    sp.t1 = b;
    sp.node = CallstackIdx(node);
    sp.mergedCount = merged;
    m_bins[k].push_back(sp);
  }
  void flushGroup(uint32_t k) {
    Depth& d = m_d[k];
    if (d.gRuns == 0) return;
    if (d.gRuns == 1)
      emit(k, d.gStart, d.gEnd, d.gNode, 1);
    else
      emit(k, d.gStart, d.gEnd, kInvalid, d.gSamples);
    d.gRuns = 0;
  }
  void closeRun(uint32_t k) {
    Depth& d = m_d[k];
    d.open = false;
    if (!narrow(d.end - d.start)) {
      if (d.gRuns) flushGroup(k);
      emit(k, d.start, d.end, d.node, 1);
      return;
    }
    if (d.gRuns > 0 && d.start - d.gEnd < m_narrow) {
      d.gEnd = d.end;
      ++d.gRuns;
      d.gSamples += d.samples;
      return;
    }
    flushGroup(k);
    d.gStart = d.start;
    d.gEnd = d.end;
    d.gNode = d.node;
    d.gRuns = 1;
    d.gSamples = d.samples;
  }

  const NodeTable& m_nt;
  uint32_t m_d0, m_count;
  uint64_t m_t0, m_t1, m_narrow;
  static constexpr uint32_t kCacheBits = 6;
  std::vector<Depth> m_d;
  std::vector<uint32_t> m_paths = std::vector<uint32_t>(static_cast<size_t>(m_count) << kCacheBits);
  std::vector<uint32_t> m_cacheLeaf = std::vector<uint32_t>(1u << kCacheBits, kInvalid);
  std::vector<std::vector<TimelineSpan>>& m_bins;
  int32_t m_openMax = -1;
};

struct FlameAcc {
  bool any = false;
  uint32_t lca = kInvalid;
  uint32_t maxDepth = 0;
};

}  // namespace

// ---- Impl ----
struct TimelineLod::Impl {
  const TraceModel* model = nullptr;
  LodOptions options;
  Geometry geo;
  NodeTable nodes;
  std::vector<ThreadLod> threads;
  std::vector<std::atomic<uint8_t>> ready;
  std::vector<uint64_t> work;  // build weight per thread
  uint64_t totalWork = 0;
  std::atomic<uint64_t> doneWork{0};
  // trimMemory(): finest levels freed and whether the fine strip level is gone (read by queries).
  std::atomic<uint32_t> firstLevel{0};
  std::atomic<bool> fineDropped{false};
  uint32_t degradedBase = 0;

  explicit Impl(size_t n) : ready(n), work(n, 0) {
    for (auto& r : ready) r.store(0, std::memory_order_relaxed);
  }

  // Coarsest level whose bucket width is <= the tick span of one pixel (width * pixels <= span), or -1.
  int selectLevel(uint64_t span, uint32_t pixels) const {
    const uint64_t pixelSpan = span / pixels;
    const int first = static_cast<int>(firstLevel.load(std::memory_order_relaxed));
    for (int l = static_cast<int>(geo.levels.size()) - 1; l >= first; --l)
      if (geo.levels[static_cast<size_t>(l)].bucketWidth <= pixelSpan) return l;
    return -1;
  }
  bool isReady(size_t t) const { return ready[t].load(std::memory_order_acquire) != 0; }
};

uint64_t pixelEdge(uint64_t t0, uint64_t t1, uint32_t pixelWidth, uint32_t column) {
  const uint64_t w = t1 - t0;
  const uint64_t q = w / pixelWidth, r = w % pixelWidth;
  return t0 + static_cast<uint64_t>(column) * q + (static_cast<uint64_t>(column) * r) / pixelWidth;
}

TimelineLod::TimelineLod() = default;
TimelineLod::~TimelineLod() = default;
TimelineLod::TimelineLod(TimelineLod&&) noexcept = default;
TimelineLod& TimelineLod::operator=(TimelineLod&&) noexcept = default;

Result<TimelineLod> TimelineLod::prepare(const TraceModel& model, const LodOptions& options) {
  if (options.levelStep < 1 || options.levelStep > 4)
    return makeError(ErrorCode::InvalidArgument, "levelStep must be 1..4");
  if (options.maxBaseBuckets < 1) return makeError(ErrorCode::InvalidArgument, "maxBaseBuckets must be >= 1");
  if (options.forceBaseLevel > static_cast<int>(kMaxLevelNumber))
    return makeError(ErrorCode::InvalidArgument, "forceBaseLevel too large");

  auto table = buildNodeTable(model.nodes);
  if (!table.ok()) return table.error();

  const uint64_t start = model.header.start_ts, end = model.header.end_ts;
  if (end > start && end - start > kMaxTraceTicks) return makeError(ErrorCode::Unsupported, "trace too long");

  auto impl = std::make_unique<Impl>(model.threads.size());
  impl->model = &model;
  impl->options = options;
  impl->nodes = std::move(*table);
  Geometry& g = impl->geo;
  g.start = start;
  g.end = end > start ? end : start;
  g.total = std::max<uint64_t>(g.end - g.start, 1);
  g.step = options.levelStep;
  g.platform = model.header.platform;

  uint32_t base = 0;
  if (options.forceBaseLevel >= 0) {
    base = static_cast<uint32_t>(options.forceBaseLevel);
    if (countAt(g.total, base) > kMaxBaseBucketsHard)
      return makeError(ErrorCode::InvalidArgument, "forceBaseLevel gives too many buckets");
  } else {
    uint32_t byCount = 0;
    while (byCount < kMaxLevelNumber && countAt(g.total, byCount) > options.maxBaseBuckets) ++byCount;
    uint32_t byDensity = 0;
    const uint64_t need = static_cast<uint64_t>(options.minSamplesPerBaseBucket) *
                          std::max<uint64_t>(model.sampleIntervalTicks, 1);
    while (byDensity < kMaxLevelNumber && widthOf(byDensity) < need) ++byDensity;
    base = std::max(byCount, byDensity);
  }
  auto makeLevels = [&](uint32_t from) {
    g.levels.clear();
    for (uint32_t level = from;; level += g.step) {
      LodLevelInfo li;
      li.level = level;
      li.bucketWidth = widthOf(level);
      li.bucketCount = static_cast<uint32_t>(countAt(g.total, level));
      g.levels.push_back(li);
      if (li.bucketCount <= 1 || level + g.step > kMaxLevelNumber) break;
    }
  };
  makeLevels(base);
  bool degraded = false;
  if (options.maxBytes != 0) {
    // 16 bytes per bucket and level, per thread; the fine strip level (4 bytes per half base bucket) is
    // sacrificed first, then the base level moves up.
    const auto levelBytes = [&]() {
      uint64_t buckets = 0;
      for (const LodLevelInfo& li : g.levels) buckets += li.bucketCount;
      return buckets * 16ull * model.threads.size();
    };
    const uint32_t wanted = base;
    while (levelBytes() > options.maxBytes && g.levels.size() > 1) {
      base += 1;
      makeLevels(base);
    }
    degraded = base != wanted;
    impl->degradedBase = base - wanted;
  }

  // Fine strip level: half the base bucket, for pixel spans between half the base width and the base width.
  if (!degraded && options.fineStrip && options.forceBaseLevel < 0 && base >= 1 &&
      model.nodes.size() < kFineMaxNodes) {
    g.fineLevel = base >= 2 ? base - 2 : base - 1;  // a quarter of the base bucket (WP-68), a half at L1
    g.fineSplit = 1u << (base - g.fineLevel);
    g.fineWidth = widthOf(g.fineLevel);
    g.fineCount = static_cast<uint32_t>(countAt(g.total, g.fineLevel));
  }

  for (size_t t = 0; t < model.threads.size(); ++t) {
    const size_t samples = t < model.samplesByThread.size() ? model.samplesByThread[t].ts.size() : 0;
    impl->work[t] = samples + g.levels[0].bucketCount + 1;
    impl->totalWork += impl->work[t];
  }
  impl->threads.resize(model.threads.size());

  TimelineLod lod;
  lod.m_impl = std::move(impl);
  return lod;
}

Result<void> TimelineLod::build(LodCancel cancel, const std::function<void(float)>& progress) {
  if (!m_impl) return makeError(ErrorCode::InvalidArgument, "TimelineLod is not prepared");
  Impl& im = *m_impl;
  std::vector<size_t> todo;
  for (size_t t = 0; t < im.threads.size(); ++t)
    if (!im.isReady(t)) todo.push_back(t);

  unsigned workers = im.options.threads ? im.options.threads : std::thread::hardware_concurrency();
  workers =
      std::max(1u, std::min<unsigned>(workers, static_cast<unsigned>(std::max<size_t>(todo.size(), 1))));

  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::atomic<int> errorCode{0};

  auto runWorker = [&](bool reportProgress) {
    ThreadBuilder builder(*im.model, im.geo, im.nodes, im.options, cancel);
    float lastReported = 0;
    for (;;) {
      if (stop.load(std::memory_order_relaxed)) break;
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= todo.size()) break;
      const size_t t = todo[i];
      auto r = builder.run(t, im.threads[t]);
      if (!r.ok()) {
        int expected = 0;
        errorCode.compare_exchange_strong(expected, static_cast<int>(r.error().code));
        stop.store(true, std::memory_order_relaxed);
        break;
      }
      im.ready[t].store(1, std::memory_order_release);
      im.doneWork.fetch_add(im.work[t], std::memory_order_relaxed);
      if (reportProgress && progress) {
        const float p = im.totalWork ? static_cast<float>(static_cast<double>(im.doneWork.load()) /
                                                          static_cast<double>(im.totalWork))
                                     : 1.0f;
        if (p > lastReported && p < 1.0f) {
          lastReported = p;
          progress(p);
        }
      }
    }
  };

  std::vector<std::thread> pool;
  for (unsigned i = 1; i < workers; ++i) pool.emplace_back(runWorker, false);
  runWorker(true);
  for (auto& th : pool) th.join();

  if (errorCode.load() != 0) {
    const auto code = static_cast<ErrorCode>(errorCode.load());
    return makeError(code, code == ErrorCode::Cancelled ? "LOD build cancelled" : "LOD build failed");
  }
  if (progress) progress(1.0f);
  return {};
}

size_t TimelineLod::threadCount() const {
  return m_impl ? m_impl->threads.size() : 0;
}
bool TimelineLod::threadReady(ThreadIdx thread) const {
  return m_impl && thread.get() < m_impl->threads.size() && m_impl->isReady(thread.get());
}
size_t TimelineLod::readyThreadCount() const {
  size_t n = 0;
  for (size_t t = 0; m_impl && t < m_impl->threads.size(); ++t) n += m_impl->isReady(t) ? 1 : 0;
  return n;
}
float TimelineLod::progress() const {
  if (!m_impl) return 0;
  if (m_impl->totalWork == 0) return 1;
  return static_cast<float>(static_cast<double>(m_impl->doneWork.load()) /
                            static_cast<double>(m_impl->totalWork));
}
const std::vector<LodLevelInfo>& TimelineLod::levels() const {
  static const std::vector<LodLevelInfo> kEmpty;
  return m_impl ? m_impl->geo.levels : kEmpty;
}

size_t TimelineLod::trimMemory(size_t bytes) {
  if (!m_impl || readyThreadCount() != m_impl->threads.size()) return 0;
  Impl& im = *m_impl;
  size_t freed = 0;
  auto release = [&](auto& vec) {
    freed += vec.capacity() * sizeof(typename std::decay_t<decltype(vec)>::value_type);
    std::decay_t<decltype(vec)>().swap(vec);
  };
  if (!im.fineDropped.load(std::memory_order_relaxed) && im.geo.fineCount > 0) {
    im.fineDropped.store(true, std::memory_order_relaxed);
    for (ThreadLod& t : im.threads) release(t.fine);
  }
  while (freed < bytes) {
    const uint32_t first = im.firstLevel.load(std::memory_order_relaxed);
    if (first + 1 >= im.geo.levels.size()) break;  // the coarsest level stays
    for (ThreadLod& t : im.threads) {
      if (first >= t.levels.size()) continue;
      LevelArrays& a = t.levels[first];
      release(a.band);
      release(a.dominant);
      release(a.strip);
      release(a.lca);
      release(a.maxDepth);
    }
    im.firstLevel.store(first + 1, std::memory_order_relaxed);
  }
  return freed;
}

LodTrim TimelineLod::trimState() const {
  LodTrim t;
  if (m_impl) {
    t.droppedLevels = m_impl->firstLevel.load(std::memory_order_relaxed);
    t.fineDropped = m_impl->fineDropped.load(std::memory_order_relaxed);
  }
  return t;
}

uint32_t TimelineLod::degradedBaseLevels() const {
  return m_impl ? m_impl->degradedBase : 0;
}

bool TimelineLod::bucket(ThreadIdx thread, size_t levelIndex, uint32_t bucketIndex, LodBucket& out) const {
  if (!threadReady(thread) || levelIndex >= m_impl->geo.levels.size() ||
      levelIndex < m_impl->firstLevel.load(std::memory_order_relaxed) ||
      bucketIndex >= m_impl->geo.levels[levelIndex].bucketCount)
    return false;
  const LevelArrays& a = m_impl->threads[thread.get()].levels[levelIndex];
  const BandCell& b = a.band[bucketIndex];
  out.running = b.running;
  out.ready = b.ready;
  out.waiting = b.waiting;
  out.waitCategory = b.category;
  out.dominant = CallstackIdx(a.dominant[bucketIndex]);
  out.share = a.strip[bucketIndex].share;
  out.coverage = a.strip[bucketIndex].coverage;
  out.commonPrefix = CallstackIdx(a.lca[bucketIndex]);
  out.maxDepth = a.maxDepth[bucketIndex];
  return true;
}

size_t TimelineLod::memoryBytes() const {
  size_t bytes = 0;
  for (size_t t = 0; m_impl && t < m_impl->threads.size(); ++t) {
    if (!m_impl->isReady(t)) continue;
    bytes += m_impl->threads[t].fine.capacity() * sizeof(uint32_t);
    for (const LevelArrays& a : m_impl->threads[t].levels) {
      bytes += a.band.capacity() * sizeof(BandCell) + a.dominant.capacity() * sizeof(uint32_t) +
               a.strip.capacity() * sizeof(StripCell) + a.lca.capacity() * sizeof(uint32_t) +
               a.maxDepth.capacity() * sizeof(uint16_t);
    }
  }
  return bytes;
}

uint64_t TimelineLod::contentHash() const {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  for (size_t t = 0; m_impl && t < m_impl->threads.size(); ++t) {
    if (!m_impl->isReady(t)) continue;
    mix(&t, sizeof t);
    mix(m_impl->threads[t].fine.data(), m_impl->threads[t].fine.size() * sizeof(uint32_t));
    for (const LevelArrays& a : m_impl->threads[t].levels) {
      mix(a.band.data(), a.band.size() * sizeof(BandCell));
      mix(a.dominant.data(), a.dominant.size() * sizeof(uint32_t));
      mix(a.strip.data(), a.strip.size() * sizeof(StripCell));
      mix(a.lca.data(), a.lca.size() * sizeof(uint32_t));
      mix(a.maxDepth.data(), a.maxDepth.size() * sizeof(uint16_t));
    }
  }
  return h;
}

// ---- queries ----
namespace {

Result<void> validateQuery(const TimelineLod& lod, ThreadIdx thread, uint64_t t0, uint64_t t1,
                           uint32_t pixels) {
  if (thread.get() >= lod.threadCount()) return makeError(ErrorCode::InvalidArgument, "bad thread");
  if (pixels < 1 || pixels > kMaxPixelWidth) return makeError(ErrorCode::InvalidArgument, "bad pixelWidth");
  if (t1 <= t0) return makeError(ErrorCode::InvalidArgument, "empty time range");
  if (t1 - t0 > kMaxQuerySpanTicks) return makeError(ErrorCode::InvalidArgument, "time range too large");
  return {};
}

// Category with the most ticks (lowest id on ties), kNoWaitCategory when all are zero.
inline uint8_t argmaxCategory(const uint64_t* cat) {
  static_assert(kCatCount == 5);
  uint8_t best = 0;
  uint64_t bestV = cat[0];
  for (uint8_t c = 1; c < 5; ++c) {
    const bool better = cat[c] > bestV;
    best = better ? c : best;
    bestV = better ? cat[c] : bestV;
  }
  return bestV ? best : kNoWaitCategory;
}

// Signed conversion: a single instruction, ticks stay far below 2^63.
inline float toFloat(uint64_t v) {
  return static_cast<float>(static_cast<int64_t>(v));
}

// Fractions of one column from tick weights; the three are renormalised if rounding made them sum above 1.
inline BandColumn makeBand(float r, float y, float w, uint8_t cat) {
  // Branch-free (the "rounding pushed the sum above 1" case is data dependent and would mispredict): inv is
  // exactly 1 unless the sum exceeds 1.
  float sum = r + y + w;
  const float inv = 1.0f / (sum > 1.0f ? sum : 1.0f);
  r *= inv;
  y *= inv;
  w *= inv;
  sum = sum > 1.0f ? 1.0f : sum;
  BandColumn col;
  col.running = r;
  col.ready = y;
  col.waiting = w;
  col.unknown = 1.0f - sum;
  col.waitCategory = cat;
  return col;
}

// Writes the columns of a result in increasing order; columns that are never written get the default value.
template <class T>
class ColumnWriter {
 public:
  ColumnWriter(std::vector<T>& v, uint32_t n) : m_n(n) {
    v.resize(n);
    m_data = v.data();
  }
  void put(uint32_t c, const T& value) {
    fillTo(c);
    m_data[c] = value;
    m_next = c + 1;
  }
  void finish() { fillTo(m_n); }

 private:
  void fillTo(uint32_t c) {
    for (; m_next < c; ++m_next) m_data[m_next] = T{};
  }
  T* m_data = nullptr;
  uint32_t m_n, m_next = 0;
};

struct DirectBandAcc {
  uint64_t running = 0, ready = 0, waiting = 0;
  uint64_t cat[kCatCount] = {};
  bool any = false;
  void reset() {
    running = ready = waiting = 0;
    any = false;
    for (uint64_t& c : cat) c = 0;
  }
};

// Per-column leaf tally of the direct strip path: a direct-mapped table stamped per column (so it never needs
// clearing), a list for leaves that collide in it, and the running maximum (ties go to the smaller leaf).
struct StripTally {
  struct Slot {
    uint32_t leaf = 0, stamp = 0;
    uint64_t ticks = 0;
  };
  static constexpr uint32_t kSlotBits = 6;
  Slot table[1u << kSlotBits];
  std::vector<Entry> overflow;
  uint32_t stamp = 1;
  uint64_t cover = 0, bestTicks = 0;
  uint32_t bestLeaf = kInvalid;

  void add(uint32_t leaf, uint64_t ticks) {
    cover += ticks;
    Slot& slot = table[(leaf * 2654435761u) >> (32 - kSlotBits)];
    uint64_t total;
    if (slot.stamp != stamp) {
      slot.leaf = leaf;
      slot.stamp = stamp;
      slot.ticks = total = ticks;
    } else if (slot.leaf == leaf) {
      slot.ticks += ticks;
      total = slot.ticks;
    } else {
      total = addOverflow(leaf, ticks);
    }
    if (total > bestTicks || (total == bestTicks && leaf < bestLeaf)) {
      bestTicks = total;
      bestLeaf = leaf;
    }
  }
  void reset() {
    ++stamp;
    cover = 0;
    bestTicks = 0;
    bestLeaf = kInvalid;
    overflow.clear();
  }

 private:
  uint64_t addOverflow(uint32_t leaf, uint64_t ticks) {
    for (Entry& e : overflow) {
      if (e.cs == leaf) {
        e.ticks += ticks;
        return e.ticks;
      }
    }
    overflow.push_back({ticks, leaf});
    return ticks;
  }
};

// LOD path of the strip: tick weights of the bucket dominants times 255 * 255 (coverage and share are 1/255).
struct StripAcc {
  static constexpr uint32_t kMax = 8;
  uint32_t cs[kMax] = {};
  uint64_t est[kMax] = {};
  uint32_t n = 0;
  uint64_t cover = 0;  // ticks x coverage (x255)
  void add(uint32_t stack, uint64_t v) {
    for (uint32_t i = 0; i < n; ++i) {
      if (cs[i] == stack) {
        est[i] += v;
        return;
      }
    }
    if (n < kMax) {
      cs[n] = stack;
      est[n++] = v;
    }
  }
};

// Strip columns from the fine strip level (packed 32-bit cells, bucket width 1 << shift), the strip half of
// fusedLevelPass: a fixed-trip masked loop of K slots per column (a column holds at most K bucket starts),
// no data-dependent branch. Buckets weigh their effective width (w, except the last bucket of the level when
// the trace end clips it: the column holding that one takes `slow`).
template <uint32_t K, class Slow>
void fineStripPass(const uint32_t* fine, uint64_t start, uint64_t end, uint64_t bucketCount, uint32_t shift,
                   uint64_t t0, uint64_t t1, uint32_t pixels, StripColumn* cols, Slow&& slow) {
  const uint64_t w = 1ull << shift;
  const uint64_t lo = std::max(t0, start), hi = std::max(lo, std::min(t1, end));
  const uint64_t first = (lo - start + w - 1) >> shift;
  const uint64_t last = std::min<uint64_t>(bucketCount, (hi - start + w - 1) >> shift);
  const bool partialTail = bucketCount > 0 && start + bucketCount * w > end;

  Sweep sw(t0, t1, pixels);
  uint32_t c = 0;
  uint64_t i = first;
  if (first < last) {
    sw.seek(start + first * w);
    c = sw.cur.column();
  }
  for (uint32_t k = 0; k < c; ++k) cols[k] = StripColumn{};
  while (i < last) {
    const uint64_t colEnd = sw.colEnd;
    const uint64_t bj = std::min<uint64_t>(last, (colEnd - start + w - 1) >> shift);
    const uint32_t n = static_cast<uint32_t>(bj > i ? bj - i : 0);
    if (n == 0) {
      cols[c] = StripColumn{};
    } else if (partialTail && bj == bucketCount) {
      slow(i, n, sw.cur, cols[c]);
    } else {
      uint32_t doms[K], ests[K], covs[K];
      uint32_t coverSum = 0;
      for (uint32_t j = 0; j < K; ++j) {
        const uint64_t idx = i + std::min<uint64_t>(j, n - 1);
        const uint32_t v = fine[idx];
        const uint32_t dom = v & kFineNone;
        const uint32_t vmask = (j < n && dom != kFineNone) ? 0xFFFFFFFFu : 0u;
        const uint32_t cov = (v >> 28) & vmask;
        doms[j] = dom;
        covs[j] = vmask;
        ests[j] = cov * ((v >> 24) & 15u);
        coverSum += cov;
      }
      StripColumn column;
      if (coverSum > 0) {
        uint64_t bestKey = 0;
        for (uint32_t x = 0; x < K; ++x) {
          uint32_t e = 0;
          for (uint32_t y = 0; y < K; ++y) e += doms[y] == doms[x] ? (ests[y] & covs[y]) : 0u;
          const uint64_t key = (static_cast<uint64_t>(e) << 32) | (0xFFFFFFFFu - doms[x]);
          bestKey = (covs[x] != 0 && key > bestKey) ? key : bestKey;
        }
        column.stack = CallstackIdx(0xFFFFFFFFu - static_cast<uint32_t>(bestKey));
        column.share =
            std::min(1.0f, static_cast<float>(bestKey >> 32) / (15.0f * static_cast<float>(coverSum)));
        column.coverage =
            std::min(1.0f, static_cast<float>(coverSum) * toFloat(w) / (15.0f * toFloat(sw.cur.width())));
      }
      cols[c] = column;
    }
    i = std::max<uint64_t>(i, bj);
    ++c;
    if (i < last) sw.advance();
  }
  for (; c < pixels; ++c) cols[c] = StripColumn{};
}

// Fine-level strip of one query through fineStripPass. False (nothing written) when a column could hold more
// than 4 bucket starts, which the zoom rule excludes; the caller then uses its generic pass.
bool fineStripColumns(const Geometry& g, const uint32_t* fine, uint64_t t0, uint64_t t1, uint32_t pixels,
                      StripColumn* cols) {
  const uint32_t shift = 6 + g.fineLevel;
  const uint64_t k = ((t1 - t0) / pixels + 1 + g.fineWidth - 1) >> shift;
  if (k > 4) return false;
  auto slow = [&](uint64_t i, uint32_t n, const ColumnCursor& cur, StripColumn& out) {
    const uint64_t w = g.fineWidth;
    StripAcc acc;
    for (uint32_t j = 0; j < n; ++j) {
      const uint64_t idx = i + j;
      const uint32_t v = fine[idx];
      if ((v & kFineNone) == kFineNone) continue;
      const uint64_t ew = std::min(w, g.end - (g.start + idx * w));
      const uint64_t cover = ew * fineCoverage15(v);
      acc.add(v & kFineNone, cover * fineShare15(v));
      acc.cover += cover;
    }
    out = StripColumn{};
    if (acc.n > 0 && acc.cover > 0) {
      uint32_t best = 0;
      for (uint32_t x = 1; x < acc.n; ++x)
        if (acc.est[x] > acc.est[best] || (acc.est[x] == acc.est[best] && acc.cs[x] < acc.cs[best])) best = x;
      out.stack = CallstackIdx(acc.cs[best]);
      out.share = std::min(1.0f, toFloat(acc.est[best]) / (15.0f * toFloat(acc.cover)));
      out.coverage = std::min(1.0f, toFloat(acc.cover) / (15.0f * toFloat(cur.width())));
    }
  };
  auto run = [&](auto kTag) {
    fineStripPass<decltype(kTag)::value>(fine, g.start, g.end, g.fineCount, shift, t0, t1, pixels, cols, slow);
  };
  switch (k) {
    case 0:
    case 1: run(std::integral_constant<uint32_t, 1>{}); break;
    case 2: run(std::integral_constant<uint32_t, 2>{}); break;
    case 3: run(std::integral_constant<uint32_t, 3>{}); break;
    default: run(std::integral_constant<uint32_t, 4>{}); break;
  }
  return true;
}

}  // namespace

Result<void> TimelineLod::stateBands(const StateBandQuery& q, StateBandResult& out) const {
  if (!m_impl) return makeError(ErrorCode::InvalidArgument, "TimelineLod is not prepared");
  if (auto v = validateQuery(*this, q.thread, q.t0, q.t1, q.pixelWidth); !v.ok()) return v;
  const Impl& im = *m_impl;
  const size_t t = q.thread.get();
  out.pending = false;
  out.level = kDirectLevel;
  ColumnWriter<BandColumn> writer(out.columns, q.pixelWidth);
  const int lv = q.mode == QueryMode::Direct ? -1 : im.selectLevel(q.t1 - q.t0, q.pixelWidth);
  Sweep sw(q.t0, q.t1, q.pixelWidth);

  if (lv >= 0) {
    const LodLevelInfo& li = im.geo.levels[static_cast<size_t>(lv)];
    out.level = static_cast<uint8_t>(li.level);
    if (!im.isReady(t)) {
      out.pending = true;
      writer.finish();
      return {};
    }
    const BandCell* band = im.threads[t].levels[static_cast<size_t>(lv)].band.data();
    const uint64_t start = im.geo.start, end = im.geo.end, w = li.bucketWidth;
    const uint64_t lo = std::max(q.t0, start), hi = std::min(q.t1, end);
    if (lo >= hi) {
      writer.finish();
      return {};
    }
    // Buckets whose start lies in [lo, hi) are assigned to the column containing their start.
    const uint64_t first = (lo - start + w - 1) / w;
    const uint64_t last = std::min<uint64_t>(li.bucketCount, (hi - start + w - 1) / w);
    // Column by column: all buckets that start in the column are summed (weights are the bucket ticks), then
    // the column is written once. Columns without a bucket start keep the default (unknown) value.
    uint64_t i = first;
    while (i < last) {
      uint64_t bs = start + i * w;
      if (!sw.active) {
        sw.seek(bs);
      } else if (bs >= sw.colEnd) {
        sw.advance();
        if (bs >= sw.colEnd) sw.seek(bs);
      }
      const uint64_t colEnd = sw.colEnd;
      uint64_t running = 0, ready = 0, waiting = 0, weight = 0;
      uint64_t cat[kCatCount + 1] = {};  // the last entry collects "no category"
      do {
        const uint64_t ew = std::min(w, end - bs);
        const BandCell cell = band[i];
        running += cell.running * ew;
        ready += cell.ready * ew;
        const uint64_t wt = cell.waiting * ew;
        waiting += wt;
        cat[cell.category < kCatCount ? cell.category : kCatCount] += wt;
        weight += ew;
        ++i;
        bs += w;
      } while (i < last && bs < colEnd);
      const uint64_t a = q.t0 + sw.cur.begin(), b = colEnd;
      float scale = 1.0f / (255.0f * toFloat(weight));
      if (a < start || b > end) {  // column sticks out of the trace: the outside part is unknown
        scale *= toFloat(std::min(b, end) - std::max(a, start)) / toFloat(b - a);
      }
      writer.put(sw.cur.column(),
                 makeBand(toFloat(running) * scale, toFloat(ready) * scale, toFloat(waiting) * scale,
                          waiting ? argmaxCategory(cat) : kNoWaitCategory));
    }
    writer.finish();
    return {};
  }

  // Direct path: exact tally over the state intervals.
  const auto& ivs = statesOf(*im.model, t);
  auto it = std::partition_point(ivs.begin(), ivs.end(),
                                 [&](const ThreadStateInterval& iv) { return iv.end <= q.t0; });
  DirectBandAcc acc;
  auto flush = [&](const ColumnCursor& c) {
    if (acc.any) {
      const float inv = 1.0f / toFloat(c.width());
      writer.put(c.column(),
                 makeBand(toFloat(acc.running) * inv, toFloat(acc.ready) * inv, toFloat(acc.waiting) * inv,
                          acc.waiting ? argmaxCategory(acc.cat) : kNoWaitCategory));
    }
    acc.reset();
  };
  for (; it != ivs.end() && it->start < q.t1; ++it) {
    const uint64_t lo = std::max(it->start, q.t0), hi = std::min(it->end, q.t1);
    if (hi <= lo || it->state == ThreadState::Unknown) continue;
    const ThreadState st = it->state;
    const uint32_t cat = st == ThreadState::Waiting
                             ? static_cast<uint32_t>(waitReasonCategory(im.geo.platform, it->wait_reason))
                             : 0;
    auto piece = [&](uint64_t ticks) {
      acc.any = true;
      if (st == ThreadState::Running) {
        acc.running += ticks;
      } else if (st == ThreadState::Ready) {
        acc.ready += ticks;
      } else {
        acc.waiting += ticks;
        acc.cat[cat] += ticks;
      }
    };
    moveTo(sw, lo, flush);
    if (hi <= sw.colEnd) {
      piece(hi - lo);
      continue;
    }
    piece(sw.colEnd - lo);
    flush(sw.cur);
    sw.advance();
    if (sw.colEnd <= hi) {
      // Columns lying completely inside the interval take the state as is.
      const BandColumn full = st == ThreadState::Running ? makeBand(1, 0, 0, kNoWaitCategory)
                              : st == ThreadState::Ready ? makeBand(0, 1, 0, kNoWaitCategory)
                                                         : makeBand(0, 0, 1, static_cast<uint8_t>(cat));
      do {
        if (sw.cur.width() > 0) writer.put(sw.cur.column(), full);
        sw.advance();
      } while (sw.colEnd <= hi);
    }
    const uint64_t begin = q.t0 + sw.cur.begin();
    if (hi > begin) piece(hi - begin);
  }
  if (sw.active) flush(sw.cur);
  writer.finish();
  return {};
}

Result<void> TimelineLod::sampleStrip(const SampleStripQuery& q, SampleStripResult& out) const {
  if (!m_impl) return makeError(ErrorCode::InvalidArgument, "TimelineLod is not prepared");
  if (auto v = validateQuery(*this, q.thread, q.t0, q.t1, q.pixelWidth); !v.ok()) return v;
  const Impl& im = *m_impl;
  const size_t t = q.thread.get();
  out.pending = false;
  out.level = kDirectLevel;
  const int lv = q.mode == QueryMode::Direct ? -1 : im.selectLevel(q.t1 - q.t0, q.pixelWidth);
  const bool useFine = lv < 0 && q.mode == QueryMode::Auto && im.geo.fineCount > 0 &&
                       !im.fineDropped.load(std::memory_order_relaxed) &&
                       (q.t1 - q.t0) / q.pixelWidth >= im.geo.fineWidth && im.isReady(t);
  if (useFine)  // the fast fine pass writes every column
    out.columns.resize(q.pixelWidth);
  else
    out.columns.assign(q.pixelWidth, StripColumn{});
  Sweep sw(q.t0, q.t1, q.pixelWidth);
  StripColumn* const cols = out.columns.data();

  // LOD pass over the buckets of one level: bucket i (width w) belongs to the column containing its start.
  // cellAt(i, dominant, coverage255, share255) returns false for a bucket without samples.
  auto lodPass = [&](uint64_t w, uint64_t bucketCount, auto&& cellAt) {
    const uint64_t start = im.geo.start, end = im.geo.end;
    const uint64_t lo = std::max(q.t0, start), hi = std::min(q.t1, end);
    if (lo >= hi) return;
    const uint64_t first = (lo - start + w - 1) / w;
    const uint64_t last = std::min<uint64_t>(bucketCount, (hi - start + w - 1) / w);
    StripAcc acc;
    auto flush = [&](const ColumnCursor& c) {
      if (acc.n > 0 && acc.cover > 0) {
        uint32_t best = 0;
        for (uint32_t k = 1; k < acc.n; ++k)
          if (acc.est[k] > acc.est[best] || (acc.est[k] == acc.est[best] && acc.cs[k] < acc.cs[best]))
            best = k;
        StripColumn& col = cols[c.column()];
        col.stack = CallstackIdx(acc.cs[best]);
        col.share = std::min(1.0f, toFloat(acc.est[best]) / (255.0f * toFloat(acc.cover)));
        col.coverage = std::min(1.0f, toFloat(acc.cover) / (255.0f * toFloat(c.width())));
      }
      acc.n = 0;
      acc.cover = 0;
    };
    for (uint64_t i = first; i < last; ++i) {
      uint32_t dom;
      uint64_t coverage, share;
      if (!cellAt(i, dom, coverage, share)) continue;
      const uint64_t bs = start + i * w;
      moveTo(sw, bs, flush);
      const uint64_t ew = std::min(w, end - bs);
      const uint64_t cover = ew * coverage;
      acc.add(dom, cover * share);
      acc.cover += cover;
    }
    if (sw.active) flush(sw.cur);
  };

  if (lv >= 0) {
    const LodLevelInfo& li = im.geo.levels[static_cast<size_t>(lv)];
    out.level = static_cast<uint8_t>(li.level);
    if (!im.isReady(t)) {
      out.pending = true;
      return {};
    }
    const LevelArrays& arr = im.threads[t].levels[static_cast<size_t>(lv)];
    const uint32_t* dominant = arr.dominant.data();
    const StripCell* strip = arr.strip.data();
    lodPass(li.bucketWidth, li.bucketCount,
            [&](uint64_t i, uint32_t& dom, uint64_t& coverage, uint64_t& share) {
              dom = dominant[i];
              coverage = strip[i].coverage;
              share = strip[i].share;
              return dom != kInvalid;
            });
    return {};
  }

  // Fine strip level: pixel spans between the fine bucket width and the base width (a quarter of it, WP-68; an
  // approximation like every level; the exact direct path below serves narrower pixels and threads that are
  // not built yet).
  if (useFine) {
    const uint32_t* fine = im.threads[t].fine.data();
    out.level = static_cast<uint8_t>(im.geo.fineLevel);
    if (fineStripColumns(im.geo, fine, q.t0, q.t1, q.pixelWidth, cols)) return {};
    std::fill(out.columns.begin(), out.columns.end(), StripColumn{});
    lodPass(im.geo.fineWidth, im.geo.fineCount,
            [&](uint64_t i, uint32_t& dom, uint64_t& coverage, uint64_t& share) {
              const uint32_t v = fine[i];
              dom = fineDominant(v);
              coverage = fineCoverage15(v) * 17u;
              share = fineShare15(v) * 17u;
              return dom != kFineNone;
            });
    return {};
  }

  // Direct path: exact tally over the samples of the window. Per column the ticks of each leaf are summed in
  // a small direct-mapped table (stamped per column; leaves that collide go to a list) and the maximum is
  // tracked on the fly, so a column costs O(samples in it).
  const SampleView sv = sampleView(*im.model, im.geo, t);
  const NodeTable& nt = im.nodes;
  StripTally tally;
  auto flush = [&](const ColumnCursor& c) {
    if (tally.cover > 0) {
      StripColumn& col = cols[c.column()];
      col.stack = CallstackIdx(tally.bestLeaf);
      // Float divisions (the tick counts are far below 2^24 per column in practice, and the result is a float
      // anyway); double divisions cost ~4x as much per column.
      const float invCover = 1.0f / toFloat(tally.cover);
      col.share = toFloat(tally.bestTicks) * invCover;
      col.coverage = toFloat(tally.cover) / toFloat(c.width());
    }
    tally.reset();
  };
  auto slow = [&](uint64_t lo, uint64_t hi, uint32_t leaf) {
    splitInto(sw, lo, hi, [&](uint32_t, uint64_t ticks) { tally.add(leaf, ticks); }, flush);
  };
  size_t i = sv.firstAfter(q.t0);
  const uint64_t limit = std::min(q.t1, sv.end);
  const uint64_t lowest = std::max(q.t0, sv.start);
  const uint64_t interval = sv.interval;
  const size_t nodes = nt.parent.size();
  for (; i < sv.n && sv.ts[i] < lowest; ++i) {  // extents reaching into the window from before it
    const uint64_t lo = std::max(sv.extentStart(i), q.t0), hi = std::min(sv.extentEnd(i), q.t1);
    if (hi > lo) slow(lo, hi, nt.leaf(sv.cs[i]));
  }
  {
    const uint64_t* ts = sv.ts;
    const CallstackIdx* cs = sv.cs;
    uint64_t colEnd = sw.colEnd;
    for (; i + 1 < sv.n; ++i) {
      const uint64_t a = ts[i];
      if (a + interval > limit) break;
      uint64_t b = a + interval;
      const uint64_t next = ts[i + 1];
      b = next < b ? next : b;
      if (b <= a) continue;
      const uint32_t c = cs[i].get();
      const uint32_t leaf = c < nodes ? c : 0;
      if (b <= colEnd) {
        tally.add(leaf, b - a);
      } else if (sw.active && a < colEnd && b <= colEnd + sw.cur.width() + 1) {
        // The common crossing: the sample starts in the current column and ends in the next one (columns are
        // at least as wide as a sample here). Split it by hand instead of through the generic splitter.
        tally.add(leaf, colEnd - a);
        flush(sw.cur);
        sw.advance();
        if (b <= sw.colEnd) {
          tally.add(leaf, b - colEnd);
        } else {  // the next column is narrower than the rest of the sample: the generic splitter
          splitInto(sw, colEnd, b, [&](uint32_t, uint64_t ticks) { tally.add(leaf, ticks); }, flush);
        }
        colEnd = sw.colEnd;
      } else {
        slow(a, b, leaf);
        colEnd = sw.colEnd;
      }
    }
  }
  for (; i < sv.n && sv.ts[i] < q.t1; ++i) {
    const uint64_t lo = std::max(sv.extentStart(i), q.t0), hi = std::min(sv.extentEnd(i), q.t1);
    if (hi > lo) slow(lo, hi, nt.leaf(sv.cs[i]));
  }
  if (sw.active) flush(sw.cur);
  return {};
}

// ---- fused band + strip (WP-64) ----
namespace {

// Sums the buckets that start in each pixel column in a fixed-trip masked loop of K slots (the level is the
// coarsest one whose width is <= the pixel span, so a column holds at most 4 bucket starts). There is no
// data-dependent branch, whatever the phase of the zoom against the bucket grid, and the band and strip share
// the column walk. Equivalent to the per-bucket loops of stateBands() and sampleStrip(): buckets weigh
// their effective width, which is w for all but the last bucket of the level; a column holding that one (if
// it is clipped by the trace end) takes `slow`.
template <uint32_t K, class Slow>
void fusedLevelPass(const BandCell* band, const uint32_t* dominant, const StripCell* strip, uint64_t start,
                    uint64_t end, uint64_t bucketCount, uint32_t shift, uint64_t t0, uint64_t t1,
                    uint32_t pixels, BandColumn* bandCols, StripColumn* stripCols, Slow&& slow) {
  const uint64_t w = 1ull << shift;
  // hi never below lo: a window entirely before the trace would otherwise underflow `last` and walk past the
  // output columns.
  const uint64_t lo = std::max(t0, start), hi = std::max(lo, std::min(t1, end));
  const uint64_t first = (lo - start + w - 1) >> shift;
  const uint64_t last = std::min<uint64_t>(bucketCount, (hi - start + w - 1) >> shift);
  const bool partialTail = bucketCount > 0 && start + bucketCount * w > end;

  Sweep sw(t0, t1, pixels);
  uint32_t c = 0;
  uint64_t i = first;
  if (first < last) {
    sw.seek(start + first * w);
    c = sw.cur.column();
  }
  for (uint32_t k = 0; k < c; ++k) {
    bandCols[k] = BandColumn{};
    stripCols[k] = StripColumn{};
  }
  const float invW = 1.0f / 255.0f;
  const float invN[5] = {0.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f};
  const uint64_t trace0 = start;
  while (i < last) {
    const uint64_t colEnd = sw.colEnd;
    const uint64_t bj = std::min<uint64_t>(last, (colEnd - trace0 + w - 1) >> shift);
    const uint32_t n = static_cast<uint32_t>(bj > i ? bj - i : 0);
    if (n == 0) {
      bandCols[c] = BandColumn{};
      stripCols[c] = StripColumn{};
    } else if (partialTail && bj == bucketCount) {
      slow(i, n, sw.cur, bandCols[c], stripCols[c]);
    } else {
      uint32_t even = 0, odd = 0;  // 16-bit lanes: running | waiting, ready | (unused)
      uint32_t cells[K];
      uint32_t doms[K], ests[K], covs[K];
      uint32_t coverSum = 0;
      for (uint32_t j = 0; j < K; ++j) {
        const uint64_t idx = i + std::min<uint64_t>(j, n - 1);
        const uint32_t mask = j < n ? 0xFFFFFFFFu : 0u;
        uint32_t cell;
        std::memcpy(&cell, &band[idx], sizeof cell);
        cells[j] = cell & mask;
        even += (cell & 0x00FF00FFu) & mask;
        odd += ((cell >> 8) & 0x00FF00FFu) & mask;
        const uint32_t dom = dominant[idx];
        const StripCell sc = strip[idx];
        const uint32_t vmask = (j < n && dom != kInvalid) ? 0xFFFFFFFFu : 0u;
        const uint32_t cov = sc.coverage & vmask;
        doms[j] = dom;
        covs[j] = vmask;
        ests[j] = cov * sc.share;
        coverSum += cov;
      }
      const uint32_t running = even & 0xFFFFu, waiting = even >> 16, ready = odd & 0xFFFFu;
      const uint64_t a = t0 + sw.cur.begin(), b = colEnd;
      float scale = invN[n] * invW;
      if (a < start || b > end) scale *= toFloat(std::min(b, end) - std::max(a, start)) / toFloat(b - a);
      uint8_t category = kNoWaitCategory;
      if (waiting != 0) {  // rare: the wait category with the most waiting ticks (lowest id on ties)
        uint32_t lanes[kCatCount] = {};
        for (uint32_t j = 0; j < K; ++j) {
          const uint32_t cat = cells[j] >> 24;
          if (cat < kCatCount) lanes[cat] += (cells[j] >> 16) & 0xFFu;
        }
        uint32_t bestLane = 0;
        for (uint32_t k = 0; k < kCatCount; ++k) {
          if (lanes[k] > bestLane) {
            bestLane = lanes[k];
            category = static_cast<uint8_t>(k);
          }
        }
      }
      bandCols[c] = makeBand(static_cast<float>(running) * scale, static_cast<float>(ready) * scale,
                             static_cast<float>(waiting) * scale, category);
      StripColumn column;
      if (coverSum > 0) {
        // The distinct dominant with the largest summed estimate; ties go to the smaller callstack id.
        uint64_t bestKey = 0;
        for (uint32_t x = 0; x < K; ++x) {
          uint32_t e = 0;
          for (uint32_t y = 0; y < K; ++y) e += doms[y] == doms[x] ? (ests[y] & covs[y]) : 0u;
          const uint64_t key = (static_cast<uint64_t>(e) << 32) | (0xFFFFFFFFu - doms[x]);
          bestKey = (covs[x] != 0 && key > bestKey) ? key : bestKey;
        }
        column.stack = CallstackIdx(0xFFFFFFFFu - static_cast<uint32_t>(bestKey));
        column.share =
            std::min(1.0f, static_cast<float>(bestKey >> 32) / (255.0f * static_cast<float>(coverSum)));
        column.coverage =
            std::min(1.0f, static_cast<float>(coverSum) * toFloat(w) / (255.0f * toFloat(sw.cur.width())));
      }
      stripCols[c] = column;
    }
    i = std::max<uint64_t>(i, bj);
    ++c;
    if (i < last) sw.advance();
  }
  for (; c < pixels; ++c) {
    bandCols[c] = BandColumn{};
    stripCols[c] = StripColumn{};
  }
}

}  // namespace

Result<void> TimelineLod::rowColumns(const RowQuery& q, StateBandResult& band,
                                     SampleStripResult& strip) const {
  if (!m_impl) return makeError(ErrorCode::InvalidArgument, "TimelineLod is not prepared");
  if (auto v = validateQuery(*this, q.thread, q.t0, q.t1, q.pixelWidth); !v.ok()) return v;
  const Impl& im = *m_impl;
  const size_t t = q.thread.get();
  const int lv = q.mode == QueryMode::Direct ? -1 : im.selectLevel(q.t1 - q.t0, q.pixelWidth);
  if (lv >= 0 && im.isReady(t)) {
    const LodLevelInfo& li = im.geo.levels[static_cast<size_t>(lv)];
    const uint32_t shift = 6 + li.level;
    // A column of c ticks holds at most ceil(c / w) bucket starts; c <= pixel span + 1.
    const uint64_t k = ((q.t1 - q.t0) / q.pixelWidth + 1 + li.bucketWidth - 1) >> shift;
    if (k <= 4) {
      band.pending = false;
      band.level = static_cast<uint8_t>(li.level);
      strip.pending = false;
      strip.level = static_cast<uint8_t>(li.level);
      band.columns.resize(q.pixelWidth);
      strip.columns.resize(q.pixelWidth);
      const LevelArrays& arr = im.threads[t].levels[static_cast<size_t>(lv)];
      // The column holding the last bucket when the trace end clips it: buckets weigh their effective width.
      auto slow = [&](uint64_t i, uint32_t n, const ColumnCursor& cur, BandColumn& bandOut,
                      StripColumn& stripOut) {
        const uint64_t start = im.geo.start, end = im.geo.end, w = li.bucketWidth;
        uint64_t running = 0, ready = 0, waiting = 0, weight = 0;
        uint64_t cat[kCatCount + 1] = {};
        StripAcc acc;
        for (uint32_t j = 0; j < n; ++j) {
          const uint64_t idx = i + j;
          const uint64_t ew = std::min(w, end - (start + idx * w));
          const BandCell cell = arr.band[idx];
          running += cell.running * ew;
          ready += cell.ready * ew;
          const uint64_t wt = cell.waiting * ew;
          waiting += wt;
          cat[cell.category < kCatCount ? cell.category : kCatCount] += wt;
          weight += ew;
          if (arr.dominant[idx] != kInvalid) {
            const uint64_t cover = ew * arr.strip[idx].coverage;
            acc.add(arr.dominant[idx], cover * arr.strip[idx].share);
            acc.cover += cover;
          }
        }
        const uint64_t a = q.t0 + cur.begin(), b = q.t0 + cur.end();
        float scale = 1.0f / (255.0f * toFloat(weight));
        if (a < start || b > end) scale *= toFloat(std::min(b, end) - std::max(a, start)) / toFloat(b - a);
        bandOut = makeBand(toFloat(running) * scale, toFloat(ready) * scale, toFloat(waiting) * scale,
                           waiting ? argmaxCategory(cat) : kNoWaitCategory);
        stripOut = StripColumn{};
        if (acc.n > 0 && acc.cover > 0) {
          uint32_t best = 0;
          for (uint32_t k = 1; k < acc.n; ++k)
            if (acc.est[k] > acc.est[best] || (acc.est[k] == acc.est[best] && acc.cs[k] < acc.cs[best]))
              best = k;
          stripOut.stack = CallstackIdx(acc.cs[best]);
          stripOut.share = std::min(1.0f, toFloat(acc.est[best]) / (255.0f * toFloat(acc.cover)));
          stripOut.coverage = std::min(1.0f, toFloat(acc.cover) / (255.0f * toFloat(cur.width())));
        }
      };
      auto run = [&](auto kTag) {
        fusedLevelPass<decltype(kTag)::value>(arr.band.data(), arr.dominant.data(), arr.strip.data(),
                                              im.geo.start, im.geo.end, li.bucketCount, shift, q.t0, q.t1,
                                              q.pixelWidth, band.columns.data(), strip.columns.data(), slow);
      };
      switch (k) {
        case 0:
        case 1: run(std::integral_constant<uint32_t, 1>{}); break;
        case 2: run(std::integral_constant<uint32_t, 2>{}); break;
        case 3: run(std::integral_constant<uint32_t, 3>{}); break;
        default: run(std::integral_constant<uint32_t, 4>{}); break;
      }
      return {};
    }
  }
  StateBandQuery bq;
  bq.thread = q.thread;
  bq.t0 = q.t0;
  bq.t1 = q.t1;
  bq.pixelWidth = q.pixelWidth;
  bq.mode = q.mode;
  if (auto r = stateBands(bq, band); !r.ok()) return r;
  SampleStripQuery sq;
  sq.thread = q.thread;
  sq.t0 = q.t0;
  sq.t1 = q.t1;
  sq.pixelWidth = q.pixelWidth;
  sq.mode = q.mode;
  return sampleStrip(sq, strip);
}

Result<void> TimelineLod::timeline(const TimelineQuery& q, TimelineResult& out) const {
  if (!m_impl) return makeError(ErrorCode::InvalidArgument, "TimelineLod is not prepared");
  if (auto v = validateQuery(*this, q.thread, q.t0, q.t1, q.pixelWidth); !v.ok()) return v;
  const Impl& im = *m_impl;
  const size_t t = q.thread.get();
  out.pending = false;
  out.level = kDirectLevel;
  out.spans.clear();
  const NodeTable& nt = im.nodes;
  const SampleView sv = sampleView(*im.model, im.geo, t);
  const uint32_t d0 = std::max<uint32_t>(q.depthBegin, 1);
  const uint32_t d1 =
      static_cast<uint32_t>(std::min<uint64_t>(q.depthEnd, static_cast<uint64_t>(nt.maxDepth) + 1));
  const uint64_t span = q.t1 - q.t0;

  // Exact regime: a pixel is narrower than one sample interval, so samples are drawn at their own edges.
  if (span / q.pixelWidth < sv.interval) {
    if (d0 >= d1) return {};
    RunEmitter em(nt, d0, d1 - d0, q.t0, q.t1, q.pixelWidth, out.bins);
    for (size_t i = sv.firstAfter(q.t0); i < sv.n && sv.ts[i] < q.t1; ++i) {
      const uint64_t lo = sv.extentStart(i), hi = sv.extentEnd(i);
      if (hi <= lo || hi <= q.t0) continue;
      em.sample(lo, hi, nt.leaf(sv.cs[i]));
    }
    em.finish();
    size_t total = 0;
    for (uint32_t k = 0; k < d1 - d0; ++k) total += out.bins[k].size();
    out.spans.reserve(total);
    for (uint32_t k = 0; k < d1 - d0; ++k)
      out.spans.insert(out.spans.end(), out.bins[k].begin(), out.bins[k].end());
    return {};
  }

  const int lv = q.mode == QueryMode::Direct ? -1 : im.selectLevel(span, q.pixelWidth);
  if (lv >= 0) {
    out.level = static_cast<uint8_t>(im.geo.levels[static_cast<size_t>(lv)].level);
    if (!im.isReady(t)) {
      out.pending = true;
      return {};
    }
  }
  if (d0 >= d1) return {};

  FlameEmitter em(nt, d0, d1 - d0, out.scratch);
  Sweep sw(q.t0, q.t1, q.pixelWidth);
  FlameAcc acc;
  auto flush = [&](const ColumnCursor& c) {
    if (acc.any) em.column(c.column(), true, acc.lca, acc.maxDepth);
    acc = FlameAcc{};
  };
  auto fold = [&](uint32_t node, uint32_t md) {
    if (!acc.any) {
      acc.any = true;
      acc.lca = node;
      acc.maxDepth = md;
    } else {
      if (acc.lca != node && !nt.isAncestor(acc.lca, node)) acc.lca = nt.lca(acc.lca, node);
      if (md > acc.maxDepth) acc.maxDepth = md;
    }
  };

  if (lv >= 0) {
    const LodLevelInfo& li = im.geo.levels[static_cast<size_t>(lv)];
    const LevelArrays& arr = im.threads[t].levels[static_cast<size_t>(lv)];
    const uint64_t start = im.geo.start, end = im.geo.end, w = li.bucketWidth;
    const uint64_t lo = std::max(q.t0, start), hi = std::min(q.t1, end);
    if (lo < hi) {
      // Every bucket overlapping the window counts for every column it overlaps (conservative: a column is
      // only Uniform if all of its buckets are).
      const uint64_t i0 = (lo - start) / w;
      const uint64_t i1 = std::min<uint64_t>(li.bucketCount, (hi - start + w - 1) / w);
      const uint32_t* lcas = arr.lca.data();
      const uint16_t* mds = arr.maxDepth.data();
      const uint32_t* tin = nt.tin.data();
      const uint32_t* tout = nt.tout.data();
      // The fold of the current column is kept in locals and written back around column changes.
      bool fAny = false;
      uint32_t fLca = kInvalid, fMd = 0;
      uint64_t colEnd = sw.colEnd;
      for (uint64_t i = i0; i < i1; ++i) {
        const uint32_t bl = lcas[i];
        if (bl == kInvalid) continue;
        const uint32_t bmd = mds[i];
        const uint64_t bs = start + i * w;
        const uint64_t phi = std::min(std::min(bs + w, end), q.t1);
        if (phi <= colEnd) {  // the bucket lies inside the current column
          if (!fAny) {
            fAny = true;
            fLca = bl;
            fMd = bmd;
          } else {
            const uint32_t tl = tin[bl];
            if (fLca != bl && !(tin[fLca] <= tl && tl <= tout[fLca])) fLca = nt.lca(fLca, bl);
            fMd = bmd > fMd ? bmd : fMd;
          }
          continue;
        }
        const uint64_t plo = std::max(bs, q.t0);
        if (plo >= phi) continue;
        acc.any = fAny;
        acc.lca = fLca;
        acc.maxDepth = fMd;
        splitInto(sw, plo, phi, [&](uint32_t, uint64_t) { fold(bl, bmd); }, flush);
        colEnd = sw.colEnd;
        fAny = acc.any;
        fLca = acc.lca;
        fMd = acc.maxDepth;
      }
      acc.any = fAny;
      acc.lca = fLca;
      acc.maxDepth = fMd;
    }
    if (sw.active) flush(sw.cur);
    em.finish(
        q.t0, q.t1, q.pixelWidth, out.edges,
        [&](uint32_t, uint32_t, uint64_t a, uint64_t b) {
          const auto* first = sv.ts;
          const auto* last = sv.ts + sv.n;
          const size_t n =
              static_cast<size_t>(std::lower_bound(first, last, b) - std::lower_bound(first, last, a));
          return static_cast<uint32_t>(std::min<size_t>(n, std::numeric_limits<uint32_t>::max()));
        },
        out.spans);
    return {};
  }

  // Direct column path; also counts the samples starting in each column for the merged spans.
  std::vector<uint32_t>& starts = out.startCounts;
  starts.assign(static_cast<size_t>(q.pixelWidth) + 1, 0);
  auto slow = [&](uint64_t lo, uint64_t hi, uint32_t leaf, bool isStart) {
    moveTo(sw, lo, flush);
    if (isStart) ++starts[sw.cur.column() + 1];
    const uint32_t md = nt.depth[leaf];
    splitInto(sw, lo, hi, [&](uint32_t, uint64_t) { fold(leaf, md); }, flush);
  };
  {
    size_t i = sv.firstAfter(q.t0);
    const uint64_t limit = std::min(q.t1, sv.end);
    const uint64_t lowest = std::max(q.t0, sv.start);
    const uint64_t interval = sv.interval;
    const size_t nodes = nt.parent.size();
    for (; i < sv.n && sv.ts[i] < lowest; ++i) {  // extents reaching into the window from before it
      const uint64_t lo = std::max(sv.extentStart(i), q.t0), hi = std::min(sv.extentEnd(i), q.t1);
      if (hi > lo) slow(lo, hi, nt.leaf(sv.cs[i]), false);
    }
    const uint64_t* ts = sv.ts;
    const CallstackIdx* cs = sv.cs;
    uint64_t colEnd = sw.colEnd;
    uint32_t* startsOfNext = starts.data() + (sw.active ? sw.cur.column() + 1 : 1);
    // The fold of the current column is kept in locals and written back around the (rare) column changes.
    bool fAny = acc.any;
    uint32_t fLca = acc.lca, fMd = acc.maxDepth;
    const uint32_t* tin = nt.tin.data();
    const uint32_t* tout = nt.tout.data();
    const uint32_t* depth = nt.depth.data();
    for (; i + 1 < sv.n; ++i) {
      const uint64_t a = ts[i];
      if (a + interval > limit) break;
      uint64_t b = a + interval;
      const uint64_t next = ts[i + 1];
      b = next < b ? next : b;
      if (b <= a) continue;
      const uint32_t c = cs[i].get();
      const uint32_t leaf = c < nodes ? c : 0;
      if (b <= colEnd) {
        ++*startsOfNext;
        const uint32_t d = depth[leaf];
        if (!fAny) {
          fAny = true;
          fLca = leaf;
          fMd = d;
        } else {
          const uint32_t tl = tin[leaf];
          if (fLca != leaf && !(tin[fLca] <= tl && tl <= tout[fLca])) fLca = nt.lca(fLca, leaf);
          fMd = d > fMd ? d : fMd;
        }
      } else {
        acc.any = fAny;
        acc.lca = fLca;
        acc.maxDepth = fMd;
        slow(a, b, leaf, true);
        colEnd = sw.colEnd;
        startsOfNext = starts.data() + sw.cur.column() + 1;
        fAny = acc.any;
        fLca = acc.lca;
        fMd = acc.maxDepth;
      }
    }
    acc.any = fAny;
    acc.lca = fLca;
    acc.maxDepth = fMd;
    for (; i < sv.n && sv.ts[i] < q.t1; ++i) {
      const uint64_t lo = std::max(sv.extentStart(i), q.t0), hi = std::min(sv.extentEnd(i), q.t1);
      if (hi > lo) slow(lo, hi, nt.leaf(sv.cs[i]), sv.ts[i] == lo);
    }
  }
  if (sw.active) flush(sw.cur);
  for (size_t c = 0; c < q.pixelWidth; ++c) starts[c + 1] += starts[c];
  em.finish(
      q.t0, q.t1, q.pixelWidth, out.edges,
      [&](uint32_t c0, uint32_t c1, uint64_t, uint64_t) { return starts[c1] - starts[c0]; }, out.spans);
  return {};
}

Result<TimelineLod> buildLod(const TraceModel& model, const LodOptions& options, LodCancel cancel,
                             const std::function<void(float)>& progress) {
  auto lod = TimelineLod::prepare(model, options);
  if (!lod.ok()) return lod;
  auto r = lod->build(cancel, progress);
  if (!r.ok()) return r.error();
  return lod;
}

}  // namespace tachyon::analysis
