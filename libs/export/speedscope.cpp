// speedscope exporter (WP-63). See export.h for the output contract.
#include "analysis/attribution.h"
#include "export/export.h"
#include "export/export_common.h"
#include "export/json_writer.h"
#include "symbols/function_key.h"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tachyon::exporters {

namespace {

using analysis::QueryContext;
using symbols::FunctionKey;

constexpr uint32_t kUnset =
    0xFFFFFFFEu;  // frame index cache: not computed yet (kInvalidIdx = node has no frame)

std::string_view baseName(std::string_view path) {
  const size_t p = path.find_last_of("/\\");
  return p == std::string_view::npos ? path : path.substr(p + 1);
}

std::string hexName(std::string_view prefix, uint64_t v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof buf, v, 16);
  std::string s(prefix);
  s += "0x";
  s.append(buf, static_cast<size_t>(r.ptr - buf));
  return s;
}

struct FrameInfo {
  std::string name, file;
  uint32_t line = 0;
};

// The shared frame table: one entry per FunctionKey, plus per-callstack-node caches of the frame index the
// node's frame has as a leaf (exact address) and as a caller (return address - 1).
class FrameTable {
 public:
  explicit FrameTable(const QueryContext& ctx) : m_model(ctx.model()), m_symbols(ctx.symbols()) {}

  // Frame index of the frame of callstack node `node`, or kInvalidIdx when the node has no frame.
  uint32_t indexOfNode(uint32_t node, bool exact) {
    if (m_exact.empty()) {
      m_exact.assign(m_model.nodes.size(), kUnset);
      m_caller.assign(m_model.nodes.size(), kUnset);
    }
    std::vector<uint32_t>& cache = exact ? m_exact : m_caller;
    if (node >= cache.size()) return kInvalidIdx;
    uint32_t& slot = cache[node];
    if (slot == kUnset) slot = compute(m_model.nodes[node].frame, exact);
    return slot;
  }

  const std::vector<FrameInfo>& frames() const { return m_frames; }

 private:
  uint32_t compute(FrameIdx frame, bool exact) {
    if (!frame.valid() || frame.get() >= m_model.frames.size()) return kInvalidIdx;
    const FunctionKey key = exact ? symbols::functionKeyFor(m_symbols, frame, false)
                                  : symbols::functionKeyForCaller(m_symbols, frame, false);
    if (const auto it = m_index.find(key); it != m_index.end()) return it->second;
    const auto idx = static_cast<uint32_t>(m_frames.size());
    m_frames.push_back(describe(frame, exact));
    m_index.emplace(key, idx);
    return idx;
  }

  FrameInfo describe(FrameIdx frame, bool exact) const {
    const model::Frame& f = m_model.frames[frame.get()];
    const symbols::ResolvedAddress& ra = exact ? m_symbols.lookup(frame) : m_symbols.lookupCaller(frame);
    FrameInfo info;
    if (ra.resolved && ra.function.valid())
      info.name = std::string(m_symbols.symbolStrings().get(ra.function));
    if (info.name.empty()) {
      if (f.module.valid() && f.module.get() < m_model.modules.size()) {
        info.name = hexName(std::string(baseName(m_model.modules[f.module.get()].path)) + "!", f.addr_or_rva);
      } else {
        info.name = hexName("", f.addr_or_rva);
      }
    }
    if (ra.resolved) {
      // The function's own first line, so one frame has one source position whatever call site it came from.
      bool found = false;
      if (f.module.valid() && m_symbols.moduleState(f.module) == model::SymbolState::Resolved) {
        if (symbols::ISymbolProvider* p = m_symbols.provider(f.module)) {
          auto lines = p->linesForFunction(ra.functionStartRva);
          if (lines.ok() && !lines->empty() && lines->front().file.valid()) {
            info.file = std::string(m_symbols.symbolStrings().get(lines->front().file));
            info.line = lines->front().line;
            found = true;
          }
        }
      }
      if (!found && ra.file.valid()) {
        info.file = std::string(m_symbols.symbolStrings().get(ra.file));
        info.line = ra.line;
      }
    }
    return info;
  }

  const model::TraceModel& m_model;
  const symbols::SymbolService& m_symbols;
  std::vector<uint32_t> m_exact, m_caller;
  std::unordered_map<FunctionKey, uint32_t> m_index;
  std::vector<FrameInfo> m_frames;
};

// Memoized "a,b,c" strings of the caller-variant frame indices of a node and all its ancestors (root first).
// Bounded: once the arena is full no new entries are made and callers use the slow path.
class PathCache {
 public:
  struct Ref {
    size_t off = 0, len = 0;
  };
  static constexpr size_t kMaxArena = size_t{256} << 20;

  PathCache(const model::TraceModel& m, FrameTable& frames) : m_model(m), m_frames(frames) {}

  // False when the cache is full. Node 0 and invalid nodes give an empty path.
  bool lookup(uint32_t node, Ref& out) {
    if (node == 0 || node >= m_model.nodes.size()) {
      out = {};
      return true;
    }
    if (const auto it = m_refs.find(node); it != m_refs.end()) {
      out = it->second;
      return true;
    }
    // Walk up to the nearest cached ancestor (or the root), then build the missing paths downwards.
    m_chain.clear();
    uint32_t cur = node;
    Ref base;
    while (cur != 0 && cur < m_model.nodes.size()) {
      if (const auto it = m_refs.find(cur); it != m_refs.end()) {
        base = it->second;
        break;
      }
      m_chain.push_back(cur);
      cur = m_model.nodes[cur].parent.get();
    }
    for (size_t k = m_chain.size(); k-- > 0;) {
      const uint32_t n = m_chain[k];
      const uint32_t idx = m_frames.indexOfNode(n, false);
      if (m_arena.size() + base.len + 12 > kMaxArena) return false;
      m_arena.reserve(m_arena.size() + base.len + 12);  // no reallocation below: base may point into it
      Ref ref;
      ref.off = m_arena.size();
      m_arena.append(m_arena.data() + base.off, base.len);
      if (idx != kInvalidIdx) {
        if (base.len != 0) m_arena.push_back(',');
        char buf[12];
        const auto r = std::to_chars(buf, buf + sizeof buf, idx);
        m_arena.append(buf, static_cast<size_t>(r.ptr - buf));
      }
      ref.len = m_arena.size() - ref.off;
      m_refs.emplace(n, ref);
      base = ref;
    }
    out = base;
    return true;
  }

  std::string_view view(const Ref& r) const { return std::string_view(m_arena).substr(r.off, r.len); }

 private:
  const model::TraceModel& m_model;
  FrameTable& m_frames;
  std::string m_arena;
  std::unordered_map<uint32_t, Ref> m_refs;
  std::vector<uint32_t> m_chain;
};

// Builds "[root,...,leaf]" for a sample.
class StackFormatter {
 public:
  StackFormatter(const model::TraceModel& m, FrameTable& frames)
      : m_model(m), m_frames(frames), m_paths(m, frames) {}

  // Returns the text of the sample's stack as a JSON array; valid until the next call.
  const std::string& format(CallstackIdx cs, bool kernelLeaf) {
    m_out.clear();
    m_out.push_back('[');
    uint32_t node = cs.valid() ? cs.get() : 0;
    if (node >= m_model.nodes.size()) node = 0;
    if (node != 0) {
      if (kernelLeaf || !fast(node)) slow(node, kernelLeaf);
    }
    m_out.push_back(']');
    return m_out;
  }

 private:
  void appendIndex(uint32_t idx) {
    char buf[12];
    const auto r = std::to_chars(buf, buf + sizeof buf, idx);
    m_out.append(buf, static_cast<size_t>(r.ptr - buf));
  }

  bool fast(uint32_t node) {
    PathCache::Ref ref;
    if (!m_paths.lookup(m_model.nodes[node].parent.get(), ref)) return false;
    const std::string_view path = m_paths.view(ref);
    m_out.append(path.data(), path.size());
    const uint32_t leaf = m_frames.indexOfNode(node, true);
    if (leaf != kInvalidIdx) {
      if (!path.empty()) m_out.push_back(',');
      appendIndex(leaf);
    }
    return true;
  }

  // Walks the stack once, leaf first. Exact addresses: the leaf, and for a kernel-leaf sample the first
  // non-kernel frame below the kernel frames (the same rule as analysis::usesExactAddress).
  void slow(uint32_t node, bool kernelLeaf) {
    m_indices.clear();
    bool allKernelSoFar = true;
    uint32_t cur = node;
    for (size_t pos = 0; cur != 0 && cur < m_model.nodes.size() && pos <= m_model.nodes.size(); ++pos) {
      const FrameIdx fi = m_model.nodes[cur].frame;
      bool exact = pos == 0;
      if (kernelLeaf) {
        const bool kernel = fi.valid() && fi.get() < m_model.frames.size() &&
                            analysis::isKernelFrame(m_model, m_model.frames[fi.get()]);
        if (pos > 0) exact = allKernelSoFar && !kernel;
        allKernelSoFar = allKernelSoFar && kernel;
      }
      const uint32_t idx = m_frames.indexOfNode(cur, exact);
      if (idx != kInvalidIdx) m_indices.push_back(idx);
      cur = m_model.nodes[cur].parent.get();
    }
    for (size_t i = m_indices.size(); i-- > 0;) {
      appendIndex(m_indices[i]);
      if (i != 0) m_out.push_back(',');
    }
  }

  const model::TraceModel& m_model;
  FrameTable& m_frames;
  PathCache m_paths;
  std::string m_out;
  std::vector<uint32_t> m_indices;
};

}  // namespace

Result<ExportStats> exportSpeedscope(const QueryContext& ctx, const analysis::Filter& filter,
                                     std::ostream& out) {
  const model::TraceModel& m = ctx.model();
  auto selected = detail::selectThreads(m, filter);
  if (!selected.ok()) return selected.error();
  if (auto ok = analysis::checkAttributable(m); !ok) return ok.error();
  const uint64_t hz = m.header.clock_frequency_hz;
  const uint64_t traceStart = m.header.start_ts;

  const uint64_t winStart = std::max(filter.t0, traceStart);
  uint64_t winEnd = std::min(filter.t1, std::max(m.header.end_ts, winStart));
  if (winEnd < winStart) winEnd = winStart;
  const int64_t startValue = detail::signedNanos(winStart, traceStart, hz);
  const int64_t windowEndValue = detail::signedNanos(winEnd, traceStart, hz);

  ExportStats stats;
  FrameTable frames(ctx);
  StackFormatter formatter(m, frames);
  JsonWriter w(out);
  w.beginObject();
  w.key("$schema");
  w.value("https://www.speedscope.app/file-format-schema.json");
  w.key("name");
  w.value("Tachyon trace");
  w.key("exporter");
  w.value("tachyon-cli export (WP-63)");
  w.key("profiles");
  w.beginArray();

  for (const ThreadIdx t : *selected) {
    // Pass 0: is there anything to write, and how much weight.
    uint64_t count = 0, sumTicks = 0;
    (void)analysis::forEachWeightedSample(m, t, filter.t0, filter.t1, {},
                                          [&](uint64_t, CallstackIdx, uint64_t weight) {
                                            if (weight == 0) return;
                                            ++count;
                                            sumTicks += weight;
                                          });
    if (count == 0) continue;
    const model::Thread& th = m.threads[t.get()];
    std::string name = th.name.empty() ? std::string("thread") : th.name;
    name += " (" + std::to_string(th.tid) + ")";
    const int64_t sumNs = static_cast<int64_t>(detail::ticksToNanos(sumTicks, hz));

    w.beginObject();
    w.key("type");
    w.value("sampled");
    w.key("name");
    w.value(name);
    w.key("unit");
    w.value("nanoseconds");
    w.key("startValue");
    w.value(startValue);
    w.key("endValue");
    w.value(std::max(windowEndValue, startValue + sumNs));
    w.key("samples");
    w.beginArray();
    (void)analysis::forEachWeightedSample(
        m, t, filter.t0, filter.t1, {}, [&](uint64_t, CallstackIdx cs, uint64_t weight, uint16_t flags) {
          if (weight == 0) return;
          w.raw(formatter.format(cs, (flags & trace::kSampleFlagKernelLeaf) != 0));
        });
    w.endArray();
    w.key("weights");
    w.beginArray();
    (void)analysis::forEachWeightedSample(m, t, filter.t0, filter.t1, {},
                                          [&](uint64_t, CallstackIdx, uint64_t weight) {
                                            if (weight == 0) return;
                                            w.value(detail::ticksToNanos(weight, hz));
                                          });
    w.endArray();
    w.endObject();
    ++stats.threads;
    stats.samples += count;
  }
  w.endArray();

  w.key("shared");
  w.beginObject();
  w.key("frames");
  w.beginArray();
  for (const FrameInfo& f : frames.frames()) {
    w.beginObject();
    w.key("name");
    w.value(f.name);
    if (!f.file.empty()) {
      w.key("file");
      w.value(f.file);
      if (f.line != 0) {
        w.key("line");
        w.value(f.line);
      }
    }
    w.endObject();
  }
  w.endArray();
  w.endObject();
  w.endObject();
  stats.frames = frames.frames().size();
  if (!w.flush()) return makeError(ErrorCode::IoError, "writing the speedscope output failed");
  return stats;
}

}  // namespace tachyon::exporters
