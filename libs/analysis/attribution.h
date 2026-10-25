// Sample weight attribution (spec 3.8 rules 1-3) and per-callstack aggregation of weights.
#pragma once

#include "analysis/query.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace tachyon::analysis {

// Total weight and sample count of one callstack node (the leaf of the samples' stacks). Samples flagged
// kSampleFlagKernelLeaf are accumulated separately (kernelLeaf = true): their stacks are attributed
// differently by the return-address rule (see frameKeyAt).
struct NodeWeight {
  CallstackIdx node;
  uint32_t scopeStack =
      0;  // WP-53: ScopeStackNode open at these samples (scope_blend.h); 0 = none/not blending
  uint64_t ticks = 0;
  uint32_t samples = 0;
  bool kernelLeaf = false;
};

// Thread-safe (const access). True for a kernel frame: an unmapped frame in the upper half of the address
// space (>= 0xFFFF800000000000) or a frame of a kernel module (ntoskrnl.exe, vmlinux, "[kernel...").
bool isKernelFrame(const model::TraceModel& model, const model::Frame& frame);

// Thread-safe (const access). Return-address rule: whether the frame at `position` of the stack whose leaf is
// `leafNode` (position 0 = the leaf, 1 = its caller, ...) is looked up at its exact address (SymbolService::
// lookup) rather than as a return address (lookupCaller). True for the leaf and, when `kernelLeafSample`
// (the sample carries kSampleFlagKernelLeaf), for the first non-kernel frame below the kernel frames (the
// interrupted instruction pointer). Reusable by blocking/unblocking stacks of context switches (pass
// kernelLeafSample = false unless the stack's leaf is in kernel mode and was captured at an interrupt).
bool usesExactAddress(const model::TraceModel& model, CallstackIdx leafNode, size_t position,
                      bool kernelLeafSample);

// Thread-safe (const access, plus the SymbolService const queries). FunctionKey of the physical function of
// the frame at `position` (leaf = 0) of the stack ending in `leafNode`, chosen by usesExactAddress:
// functionKeyFor for exact positions, functionKeyForCaller otherwise. Returns FunctionKey{kInvalidIdx, 0}
// (kRootKey) when the position is beyond the stack. For WP-34/35/46 to share the rule with the aggregation.
FunctionKey frameKeyAt(const QueryContext& ctx, CallstackIdx leafNode, size_t position, bool kernelLeafSample,
                       bool expandInline = false);

// Thread-safe (const access). Checks that the model can be attributed: states built for every thread and
// sampleIntervalTicks set when there are samples.
Result<void> checkAttributable(const model::TraceModel& model);

// Thread-safe for concurrent const access. Calls fn(ts, callstack, weight[, flags]) (flags = the sample's
// trace::kSampleFlag* bits, passed when fn accepts a fourth uint16_t parameter) for the samples of `thread`
// with t0 <= ts < t1, in time order. weight (rule 1) = min(sampleIntervalTicks, end of the enclosing Running
// interval - ts), or 0 when no Running interval encloses the sample. Requires checkAttributable. Returns
// false if cancelled (checked every 65536 samples). Precondition: the thread's samples are sorted by ts (the
// time range is found by binary search); TraceModel::load guarantees it (stable re-sort in its per-thread
// order check), hand-built models must sort their samples.
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
    if constexpr (std::is_invocable_v<Fn&, uint64_t, CallstackIdx, uint64_t, uint16_t>)
      fn(t, cols.callstack[i], w, i < cols.flags.size() ? cols.flags[i] : uint16_t{0});
    else
      fn(t, cols.callstack[i], w);
  }
  return true;
}

// Thread-safe for concurrent const access. Sums weights and sample counts per leaf callstack node over the
// given threads and time range (invalid callstacks count as the empty stack, node 0). Runs on up to `workers`
// threads (0 = hardware_concurrency); the result (sorted by node) does not depend on it. Returns Cancelled if
// `cancel` fires (checked every 65536 samples of a scan and every 4096 nodes of the merge).
//
// With an `executor` the extra workers are tasks submitted to it (at the priority of the calling task)
// instead of new threads. This cannot deadlock even when the caller itself runs on an executor thread and
// every other worker is busy: the calling thread always does a worker's share itself and never waits for a
// helper task that has not started; a helper that starts after the scan is over finds the work closed and
// returns at once.
Result<std::vector<NodeWeight>> accumulateNodeWeights(const model::TraceModel& model, uint64_t t0,
                                                      uint64_t t1, std::span<const ThreadIdx> threads,
                                                      unsigned workers, Cancel cancel,
                                                      QueryExecutor* executor = nullptr);

}  // namespace tachyon::analysis
