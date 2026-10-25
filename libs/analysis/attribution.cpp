#include "analysis/attribution.h"

#include <atomic>
#include <thread>

namespace tachyon::analysis {

namespace {
// Memory allowed for the per-worker accumulation arrays (12 bytes per callstack node per worker).
constexpr size_t kWorkerMemoryBudget = size_t{1} << 30;
}  // namespace

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
  const size_t perWorker = nodeCount * (sizeof(uint64_t) + sizeof(uint32_t));
  w = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(w, kWorkerMemoryBudget / perWorker)));

  struct Local {
    std::vector<uint64_t> ticks;
    std::vector<uint32_t> samples;
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
      const bool done = forEachWeightedSample(m, threads[k], t0, t1, cancel,
                                              [&](uint64_t, CallstackIdx cs, uint64_t weight) {
                                                size_t n = cs.get();
                                                if (n >= limit) n = 0;
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
    uint64_t ticks = 0;
    uint64_t samples = 0;
    for (const Local& loc : locals) {
      if (loc.ticks.empty()) continue;
      ticks += loc.ticks[n];
      samples += loc.samples[n];
    }
    if (samples == 0) continue;
    out.push_back(NodeWeight{CallstackIdx(static_cast<uint32_t>(n)), ticks,
                             static_cast<uint32_t>(std::min<uint64_t>(samples, UINT32_MAX))});
  }
  return out;
}

}  // namespace tachyon::analysis
