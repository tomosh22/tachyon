#include "analysis/attribution.h"

#include "trace/trace_format.h"

#include <atomic>
#include <string_view>
#include <thread>

namespace tachyon::analysis {

namespace {
// Memory allowed for the per-worker accumulation arrays (12 bytes per callstack node per worker).
constexpr size_t kWorkerMemoryBudget = size_t{1} << 30;
constexpr uint64_t kKernelBase = 0xFFFF800000000000ull;

std::string_view baseName(std::string_view path) {
  const size_t p = path.find_last_of("/\\");
  return p == std::string_view::npos ? path : path.substr(p + 1);
}

bool isKernelModulePath(std::string_view path) {
  const std::string_view b = baseName(path);
  return b == "ntoskrnl.exe" || b == "ntkrnlmp.exe" || b == "ntkrnlpa.exe" || b == "vmlinux" ||
         b.substr(0, 7) == "[kernel";
}
}  // namespace

bool isKernelFrame(const model::TraceModel& m, const model::Frame& f) {
  if (!f.module.valid()) return f.addr_or_rva >= kKernelBase;
  return f.module.get() < m.modules.size() && isKernelModulePath(m.modules[f.module.get()].path);
}

bool usesExactAddress(const model::TraceModel& m, CallstackIdx leafNode, size_t position,
                      bool kernelLeafSample) {
  if (position == 0) return true;
  if (!kernelLeafSample) return false;
  // Exact iff every frame above (towards the leaf) is a kernel frame and this one is not.
  size_t n = leafNode.valid() ? leafNode.get() : 0;
  for (size_t i = 0; n != 0 && n < m.nodes.size() && i <= position; ++i) {
    const FrameIdx fi = m.nodes[n].frame;
    const bool kernel = fi.valid() && fi.get() < m.frames.size() && isKernelFrame(m, m.frames[fi.get()]);
    if (i == position) return !kernel;
    if (!kernel) return false;
    n = m.nodes[n].parent.get();
  }
  return false;
}

FunctionKey frameKeyAt(const QueryContext& ctx, CallstackIdx leafNode, size_t position, bool kernelLeafSample,
                       bool expandInline) {
  const model::TraceModel& m = ctx.model();
  size_t n = leafNode.valid() ? leafNode.get() : 0;
  for (size_t i = 0; i < position && n != 0 && n < m.nodes.size(); ++i) n = m.nodes[n].parent.get();
  if (n == 0 || n >= m.nodes.size() || !m.nodes[n].frame.valid()) return kRootKey;
  const FrameIdx frame = m.nodes[n].frame;
  return usesExactAddress(m, leafNode, position, kernelLeafSample)
             ? symbols::functionKeyFor(ctx.symbols(), frame, expandInline)
             : symbols::functionKeyForCaller(ctx.symbols(), frame, expandInline);
}

Result<void> checkAttributable(const model::TraceModel& m) {
  if (m.statesByThread.size() != m.threads.size() || m.samplesByThread.size() != m.threads.size())
    return makeError(ErrorCode::InvalidArgument, "thread states are not built (call buildThreadStates)");
  bool anySamples = false;
  for (size_t t = 0; t < m.threads.size(); ++t) {
    if (m.samplesByThread[t].ts.empty()) continue;
    anySamples = true;
    if (m.statesByThread[t].empty())
      return makeError(ErrorCode::InvalidArgument, "thread states are not built (call buildThreadStates)");
  }
  if (anySamples && m.sampleIntervalTicks == 0)
    return makeError(ErrorCode::InvalidArgument, "sampleIntervalTicks is 0 (no sampling_hz in CaptureInfo)");
  return {};
}

Result<std::vector<NodeWeight>> accumulateNodeWeights(const model::TraceModel& m, uint64_t t0, uint64_t t1,
                                                      std::span<const ThreadIdx> threads, unsigned workers,
                                                      Cancel cancel) {
  if (auto ok = checkAttributable(m); !ok) return ok.error();
  for (ThreadIdx t : threads)
    if (!t.valid() || t.get() >= m.threads.size())
      return makeError(ErrorCode::InvalidArgument, "thread index out of range");
  const size_t nodeCount = std::max<size_t>(m.nodes.size(), 1);

  unsigned w = workers != 0 ? workers : std::max(1u, std::thread::hardware_concurrency());
  w = static_cast<unsigned>(std::min<size_t>(w, std::max<size_t>(threads.size(), 1)));
  // Budget the regular and the lazily allocated kernel-leaf arrays so the worst case stays in the cap.
  const size_t perWorker = 2 * nodeCount * (sizeof(uint64_t) + sizeof(uint32_t));
  w = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(w, kWorkerMemoryBudget / perWorker)));

  struct Local {
    std::vector<uint64_t> ticks;
    std::vector<uint32_t> samples;
    std::vector<uint64_t> kticks;  // kernel-leaf samples only; allocated on first use
    std::vector<uint32_t> ksamples;
  };
  std::vector<Local> locals(w);
  std::atomic<size_t> next{0};
  std::atomic<bool> cancelled{false};

  auto work = [&](unsigned id) {
    Local& loc = locals[id];
    for (;;) {
      const size_t k = next.fetch_add(1, std::memory_order_relaxed);
      if (k >= threads.size() || cancelled.load(std::memory_order_relaxed)) return;
      if (loc.ticks.empty()) {
        loc.ticks.assign(nodeCount, 0);
        loc.samples.assign(nodeCount, 0);
      }
      uint64_t* tk = loc.ticks.data();
      uint32_t* sc = loc.samples.data();
      const size_t limit = nodeCount;
      const bool done = forEachWeightedSample(
          m, threads[k], t0, t1, cancel, [&](uint64_t, CallstackIdx cs, uint64_t weight, uint16_t flags) {
            size_t n = cs.get();
            if (n >= limit) n = 0;
            if ((flags & trace::kSampleFlagKernelLeaf) != 0) {
              if (loc.kticks.empty()) {
                loc.kticks.assign(nodeCount, 0);
                loc.ksamples.assign(nodeCount, 0);
              }
              loc.kticks[n] += weight;
              ++loc.ksamples[n];
              return;
            }
            tk[n] += weight;
            ++sc[n];
          });
      if (!done) {
        cancelled.store(true, std::memory_order_relaxed);
        return;
      }
    }
  };
  if (w == 1) {
    work(0);
  } else {
    std::vector<std::thread> pool;
    pool.reserve(w - 1);
    for (unsigned i = 1; i < w; ++i) pool.emplace_back(work, i);
    work(0);
    for (auto& th : pool) th.join();
  }
  if (cancelled.load() || cancel.requested()) return makeError(ErrorCode::Cancelled, "query cancelled");

  std::vector<NodeWeight> out;
  for (size_t n = 0; n < nodeCount; ++n) {
    for (const bool kernel : {false, true}) {
      uint64_t ticks = 0;
      uint64_t samples = 0;
      for (const Local& loc : locals) {
        const auto& tv = kernel ? loc.kticks : loc.ticks;
        if (tv.empty()) continue;
        ticks += tv[n];
        samples += (kernel ? loc.ksamples : loc.samples)[n];
      }
      if (samples == 0) continue;
      out.push_back(NodeWeight{CallstackIdx(static_cast<uint32_t>(n)), ticks,
                               static_cast<uint32_t>(std::min<uint64_t>(samples, UINT32_MAX)), kernel});
    }
  }
  return out;
}

}  // namespace tachyon::analysis
