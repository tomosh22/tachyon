// Sample weight attribution (spec 3.8 rules 1-3) and per-callstack aggregation of weights.
#pragma once

#include "analysis/query.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace tachyon::analysis {

// Total weight and sample count of one callstack node (the leaf of the samples' stacks).
struct NodeWeight {
  CallstackIdx node;
  uint64_t ticks = 0;
  uint32_t samples = 0;
};

// Thread-safe (const access). Checks that the model can be attributed: states built for every thread and
// sampleIntervalTicks set when there are samples.
Result<void> checkAttributable(const model::TraceModel& model);

// Thread-safe for concurrent const access. Calls fn(ts, callstack, weight) for the samples of `thread` with
// t0 <= ts < t1, in time order. weight (rule 1) = min(sampleIntervalTicks, end of the enclosing Running
// interval - ts), or 0 when no Running interval encloses the sample. Requires checkAttributable. Returns
// false if cancelled (checked every 65536 samples).
template <typename Fn>
bool forEachWeightedSample(const model::TraceModel& m, ThreadIdx thread, uint64_t t0, uint64_t t1,
                           Cancel cancel, Fn&& fn) {
  const model::SampleColumns& cols = m.samplesByThread[thread.get()];
  const std::vector<model::ThreadStateInterval>& ivs = m.statesByThread[thread.get()];
  const uint64_t* ts = cols.ts.data();
  const size_t n = cols.ts.size();
  const size_t lo = static_cast<size_t>(std::lower_bound(ts, ts + n, t0) - ts);
  const size_t hi = static_cast<size_t>(std::lower_bound(ts + lo, ts + n, t1) - ts);
  if (lo >= hi) return true;
  // First interval that ends after the first sample.
  size_t si = static_cast<size_t>(
      std::partition_point(ivs.begin(), ivs.end(),
                           [&](const model::ThreadStateInterval& iv) { return iv.end <= ts[lo]; }) -
      ivs.begin());
  const uint64_t interval = m.sampleIntervalTicks;
  for (size_t i = lo; i < hi; ++i) {
    if (((i - lo) & 0xFFFFu) == 0 && cancel.requested()) return false;
    const uint64_t t = ts[i];
    while (si < ivs.size() && ivs[si].end <= t) ++si;
    uint64_t w = 0;
    if (si < ivs.size() && ivs[si].state == model::ThreadState::Running && ivs[si].start <= t)
      w = std::min(interval, ivs[si].end - t);
    fn(t, cols.callstack[i], w);
  }
  return true;
}

// Thread-safe for concurrent const access. Sums weights and sample counts per leaf callstack node over the
// given threads and time range (invalid callstacks count as the empty stack, node 0). Runs on up to `workers`
// threads (0 = hardware_concurrency); the result (sorted by node) does not depend on it. Returns Cancelled if
// `cancel` fires.
Result<std::vector<NodeWeight>> accumulateNodeWeights(const model::TraceModel& model, uint64_t t0,
                                                      uint64_t t1, std::span<const ThreadIdx> threads,
                                                      unsigned workers, Cancel cancel);

}  // namespace tachyon::analysis
