// Analysis query API (spec 3.7). WP-32 implements topDownTree, bottomUpFunctions and butterfly; the remaining
// 3.7 queries are added by their own work packages and are intentionally not declared here yet:
//   WP-33: linesForFunction, instructionsForFunction (LineAgg, InstructionAgg)
//   WP-35 (declared below): functionStats, scopeStats, threadStateTotals, frames, slowestFrames
//
// Deviation from 3.7 (see the WP-32 report): the queries need symbols to turn frames into FunctionKeys, so
// the first parameter is a QueryContext (TraceModel + SymbolService + options + a per-filter memo) instead of
// a bare TraceModel. Build trees after SymbolService::waitAll(): keys change when a module resolves.
//
// Attribution (3.8 rules 1-3) summary:
//  * a sample's weight is sampleIntervalTicks clipped to the end of the Running interval it falls in; samples
//    outside every Running interval weigh 0 (they still count in sample counts);
//  * inclusive time counts once per sample per function (recursion);
//  * exclusive time goes to the leaf frame (the innermost inline frame with inline expansion).
// Filter semantics: samples with t0 <= ts < t1 on the selected threads. Weights are NOT re-clipped to t1.
#pragma once

#include "analysis/function_registry.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"
#include "symbols/function_key.h"
#include "symbols/symbol_service.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::analysis {

using symbols::FunctionKey;

class QueryCache;     // query_cache.h
class QueryExecutor;  // query_executor.h

// Cooperative cancellation token (spec 3.7 "cancellable via a token"). Default: never cancelled.
struct Cancel {
  const std::atomic<bool>* flag = nullptr;
  // Thread-safe.
  bool requested() const noexcept { return flag != nullptr && flag->load(std::memory_order_relaxed); }
};

struct Filter {
  uint64_t t0 = 0, t1 = UINT64_MAX;               // half-open [t0, t1) on sample timestamps
  std::optional<std::vector<ThreadIdx>> threads;  // nullopt = all threads
  std::optional<CallstackIdx> subtreeRoot;        // only samples whose stack passes through this node
  // Only samples whose stack contains this function: the stack as the query sees it, i.e. after the subtree
  // cut, with the context's inline expansion and kernel frame handling applied. A handle issued by
  // QueryContext::functionIdx(); an index the context never issued is rejected with InvalidArgument.
  // Limitation: handles are dense indices without a context id, so an index that happens to be issued by
  // BOTH contexts is not detected as foreign; keep a handle with the context that issued it.
  std::optional<FunctionIdx> function;

  // Thread-safe (value type).
  bool operator==(const Filter&) const = default;
};

struct NodeAgg {
  FunctionKey key;
  uint64_t inclusiveTicks = 0, exclusiveTicks = 0;
  uint32_t sampleCount = 0;  // samples passing through this node (inclusive), zero-weight samples included
  std::vector<NodeAgg> children;  // sorted by inclusiveTicks descending, then key

  // Destruction is iterative (a 300 000-deep tree is safe to destroy and to move). Copying a NodeAgg is still
  // recursive and must not be used on deep trees; consumers (WP-43/44) traverse iteratively as well.
  NodeAgg() = default;
  ~NodeAgg();
  NodeAgg(const NodeAgg&) = default;
  NodeAgg& operator=(const NodeAgg&) = default;
  NodeAgg(NodeAgg&&) noexcept = default;
  NodeAgg& operator=(NodeAgg&&) noexcept = default;
};

struct FunctionRow {
  FunctionKey key;
  uint64_t inclusiveTicks = 0, exclusiveTicks = 0;
  uint32_t instances = 0;  // for sampled data: samples containing the function (recursion counted once)
};

struct Butterfly {
  FunctionRow self;
  std::vector<FunctionRow> callers, callees;
};

// Synthetic key of the top-down tree root and of the "caller" of a function that is at the base of a stack.
inline constexpr FunctionKey kRootKey{ModuleIdx::invalid(), 0};
// Key under which KernelFrames::Group merges runs of kernel-space frames.
inline constexpr FunctionKey kKernelKey{ModuleIdx::invalid(), ~uint64_t{0}};

// Kernel frame handling. A kernel frame is an unmapped frame in the upper half of the address space
// (>= 0xFFFF800000000000) or a frame of a kernel module (ntoskrnl.exe, vmlinux, "[kernel...").
enum class KernelFrames : uint8_t {
  Show,   // every kernel frame is its own function (default)
  Group,  // each run of consecutive kernel frames becomes one frame with kKernelKey ("<kernel>")
  Hide,   // kernel frames are dropped; their time is attributed to the nearest user-mode frame. A stack that
          // is left with no frame at all (empty, or made only of kernel frames) is attributed to the single
          // frame kKernelKey ("<kernel>") in every view: bottomUpFunctions gets a <kernel> row (inclusive =
          // exclusive) and topDownTree a <kernel> child of the root, so both views count the same samples.
};

struct AggregationOptions {
  bool expandInline = false;  // expand inline chains into frames (innermost inline = leaf)
  KernelFrames kernelFrames = KernelFrames::Show;
  unsigned threads = 0;  // worker threads; 0 = hardware_concurrency. Results do not depend on it.
};

// hits / misses count every cache lookup of a query, including the lookup of the internal per-leaf-node
// weight table that tree and row queries share; resultHits / resultMisses count the lookups of final results
// only (one per query call). WP-33 additions: the fields after `misses`.
struct CacheStats {
  uint64_t hits = 0, misses = 0;
  uint64_t resultHits = 0, resultMisses = 0;
  uint64_t evictions = 0;
  uint64_t entries = 0, bytes = 0;  // current content
};

struct QueryCacheConfig {
  size_t budgetBytes = size_t{256} << 20;  // approximate bound on the bytes held by cached results
};

// Binds a model and its symbols for the queries below and owns the QueryCache (results keyed by query, extra
// arguments and filter, LRU within a byte budget) and the FunctionRegistry. The model, the symbol service and
// the symbols' module resolution must stay unchanged and alive while the context is used. Queries may run
// concurrently on one context.
class QueryContext {
 public:
  // Thread-safe (constructor). Requires TraceModel::statesByThread to be built (buildThreadStates).
  QueryContext(const model::TraceModel& model, const symbols::SymbolService& symbols,
               AggregationOptions options = {}, QueryCacheConfig cache = {});
  ~QueryContext();
  QueryContext(const QueryContext&) = delete;
  QueryContext& operator=(const QueryContext&) = delete;

  // All accessors: thread-safe.
  const model::TraceModel& model() const;
  const symbols::SymbolService& symbols() const;
  const AggregationOptions& options() const;
  // Drops every cached result (the FunctionRegistry is kept). Results already handed out stay valid.
  void clearCache();
  CacheStats cacheStats() const;
  // Thread-safe. The cache itself (budget, counters per kind); see query_cache.h.
  QueryCache& cache() const;
  // Thread-safe (the cache locks internally). Changes the byte budget and evicts down to it.
  void setCacheBudget(size_t bytes);

  // FunctionIdx handles for Filter::function. Thread-safe; indices are issued per context and never reused.
  FunctionRegistry& functions() const;
  FunctionIdx functionIdx(FunctionKey key) const { return functions().intern(key); }
  std::optional<FunctionKey> functionKey(FunctionIdx idx) const { return functions().key(idx); }

  // Optional executor (owned by the caller, must outlive every query run on this context): the scan of a
  // query then fans out through it instead of starting its own threads. Not thread-safe against running
  // queries: call it before queries start (the pointer itself is read atomically).
  void setExecutor(QueryExecutor* executor);
  // Thread-safe.
  QueryExecutor* executor() const;

  struct Impl;
  Impl& impl() const { return *m_impl; }

 private:
  std::unique_ptr<Impl> m_impl;
};

// Shared-result variants (WP-33). Same queries and semantics, but the result is the cache's own immutable
// copy: a hit costs a lookup and a reference count, not a copy. The by-value functions below return a copy of
// it. All three are thread-safe like their by-value forms; the returned object is immutable and may be held
// and read from any thread for as long as the pointer lives (it survives eviction and clearCache()).
Result<std::shared_ptr<const NodeAgg>> topDownTreeShared(const QueryContext& ctx, const Filter& filter,
                                                         Cancel cancel = {});
Result<std::shared_ptr<const std::vector<FunctionRow>>> bottomUpFunctionsShared(const QueryContext& ctx,
                                                                                const Filter& filter,
                                                                                Cancel cancel = {});
Result<std::shared_ptr<const Butterfly>> butterflyShared(const QueryContext& ctx, const Filter& filter,
                                                         FunctionKey key, Cancel cancel = {});

// Thread-safe. Merged top-down call tree (merge by FunctionKey per depth). The root has key kRootKey,
// inclusive = total weight of the selected samples, exclusive = weight of samples with an empty stack. With
// Filter::subtreeRoot the returned root is the subtree root's function (stacks are cut above it).
Result<NodeAgg> topDownTree(const QueryContext& ctx, const Filter& filter, Cancel cancel = {});

// Thread-safe. One row per function, sorted by exclusiveTicks descending, then inclusiveTicks descending,
// then key. Memoized per filter in the context.
Result<std::vector<FunctionRow>> bottomUpFunctions(const QueryContext& ctx, const Filter& filter,
                                                   Cancel cancel = {});

// Thread-safe. Callers and callees of `key`, relative to the outermost occurrence of the function in each
// sample's stack (recursion counted once): callers' inclusive (and exclusive, and instances) sum to self's;
// a stack in which the function is the base has the caller kRootKey. Rows sorted by inclusiveTicks
// descending, then key. Callee exclusive time is the part where the callee itself is the stack leaf.
Result<Butterfly> butterfly(const QueryContext& ctx, const Filter& filter, FunctionKey key,
                            Cancel cancel = {});

// ---------------------------------------------------------------------------------------------------------
// WP-35: per-instance statistics, thread state totals and frames.
//
// Stats histogram scheme (log2 buckets of the duration in ticks). bucket(d) = 0 for d == 0, else
// floor(log2(d)) + 1: bucket 1 holds {1}, bucket 2 holds [2, 4), bucket 3 [4, 8), ... bucket b >= 1 holds
// [2^(b-1), 2^b), up to bucket 64 = [2^63, 2^64). `Stats::histogram[b]` is the number of instances in bucket
// b; the vector runs from bucket 0 to the highest non-empty bucket (empty when count == 0). The edges are
// derivable with the helpers below; ticks convert to time with TraceHeader.clock_frequency_hz at the UI edge.
struct Stats {
  uint32_t count = 0;  // number of instances
  // All in ticks. Quartiles are lower order statistics, without interpolation: the q quantile is
  // d[floor(q * (count - 1))] of the sorted durations, so the median of an even count is the lower of the two
  // middle values (matches tracegen's median_ticks). mean = totalTicks / count. All zero when count == 0.
  double min = 0, p25 = 0, median = 0, p75 = 0, max = 0, mean = 0;
  std::vector<uint32_t> histogram;
  // WP-35 additions to 3.7:
  uint64_t totalTicks = 0;    // sum of all durations (wraps modulo 2^64)
  uint32_t unterminated = 0;  // scopeStats: instances counted in `count` whose Begin has no End (they end at
                              // the trace end); 0 for functionStats
  uint32_t orphanEnds = 0;    // scopeStats: End events with no open Begin on the selected threads (ignored,
                              // not counted in `count`), with ts in [t0, t1)
};

inline constexpr uint32_t kStatsMaxBuckets = 65;
// Thread-safe: pure functions. Histogram bucket of a duration, and the half-open tick range [lower, upper)
// of a bucket (the upper edge of bucket 64 saturates at UINT64_MAX and is inclusive there).
constexpr uint32_t statsBucketOf(uint64_t ticks) {
  return ticks == 0 ? 0u : 64u - static_cast<uint32_t>(std::countl_zero(ticks));
}
constexpr uint64_t statsBucketLower(uint32_t bucket) {
  return bucket == 0 ? 0 : uint64_t{1} << (bucket - 1);
}
constexpr uint64_t statsBucketUpper(uint32_t bucket) {
  return bucket >= 64 ? UINT64_MAX : uint64_t{1} << bucket;
}

// Time in each thread state over the filter's [t0, t1) (intervals clipped to the range; the range is also
// clipped to the thread's coverage, so t1 = UINT64_MAX is fine).
struct ThreadStateTotals {
  uint64_t runningTicks = 0, readyTicks = 0, waitingTicks = 0, unknownTicks = 0;
  uint32_t runningIntervals = 0, readyIntervals = 0, waitingIntervals = 0;  // non-empty clipped intervals
};

// A series of frame marks: the marks of one thread (invalid for global marks) with one name. Frames are
// consecutive marks of a series: frame i = [mark_i, mark_{i+1}); the last mark opens no frame.
struct FrameSeries {
  ThreadIdx thread;
  StringIdx name;
  uint32_t markCount = 0;  // ignored when a FrameSeries selects a series in FrameOptions

  // Thread-safe (value type).
  bool sameSeries(const FrameSeries& o) const { return thread == o.thread && name == o.name; }
};

struct FrameOptions {
  // nullopt = the default series: the one with the most marks (ties: earliest first mark, then lowest
  // thread, then lowest name). An explicit series that has no marks gives InvalidArgument.
  std::optional<FrameSeries> series;
  uint32_t topFunctions = 5;  // exclusive functions reported per frame
};

// Running time of one thread within a frame.
struct FrameThreadTime {
  ThreadIdx thread;
  uint64_t runningTicks =
      0;                     // Running state time clipped to the frame (not affected by subtreeRoot/function)
  uint64_t sampleTicks = 0;  // weight (rule 1) of the samples inside the frame that pass the filter
};

// Exclusive time of one function within a frame (leaf of the stack as the query sees it).
struct FrameFunctionTime {
  FunctionKey key;
  uint64_t exclusiveTicks = 0;
  uint32_t samples = 0;  // samples with this leaf, zero-weight samples included (like FunctionRow::instances)
};

struct FrameSummary {
  uint64_t index = 0;                         // ordinal within the series, 0 = first frame of the series
  uint64_t number = 0;                        // FrameMark::frame_number of the mark that opens the frame
  uint64_t start = 0, end = 0, duration = 0;  // [start, end) in ticks, duration = end - start
  ThreadIdx markThread;
  StringIdx seriesName;
  // Sorted by thread. Threads with no Running time and no samples in the frame are omitted. Only the threads
  // selected by Filter::threads are reported.
  std::vector<FrameThreadTime> threads;
  // Top FrameOptions::topFunctions by exclusiveTicks (then samples, then key), over the selected threads.
  std::vector<FrameFunctionTime> topFunctions;
};

// Shared-result variants (same contract as the WP-33 ones above).
Result<std::shared_ptr<const Stats>> functionStatsShared(const QueryContext& ctx, const Filter& filter,
                                                         FunctionKey key, Cancel cancel = {});
Result<std::shared_ptr<const Stats>> scopeStatsShared(const QueryContext& ctx, const Filter& filter,
                                                      StringIdx scopeId, Cancel cancel = {});
Result<std::shared_ptr<const ThreadStateTotals>> threadStateTotalsShared(const QueryContext& ctx,
                                                                         const Filter& filter,
                                                                         ThreadIdx thread,
                                                                         Cancel cancel = {});
Result<std::shared_ptr<const std::vector<FrameSummary>>> framesShared(const QueryContext& ctx,
                                                                      const Filter& filter,
                                                                      FrameOptions options = {},
                                                                      Cancel cancel = {});
Result<std::shared_ptr<const std::vector<FrameSummary>>> slowestFramesShared(const QueryContext& ctx,
                                                                             const Filter& filter, uint32_t n,
                                                                             FrameOptions options = {},
                                                                             Cancel cancel = {});

// Thread-safe. Per-instance duration statistics of a sampled function. An instance is APPROXIMATED from
// samples: per thread, a maximal run of consecutive samples (in time order, among the samples the filter
// selects) whose stack contains the function at the same depth, with no gap greater than 2 sample intervals
// between consecutive samples of the run. Depth is the position of the outermost occurrence of the function
// counted from the root, after the context's inline expansion and kernel options and Filter::subtreeRoot
// (recursion at a different depth therefore starts a new instance). The duration of an instance is
// (last sample ts - first sample ts) + the weight (rule 1) of its last sample. Samples with weight 0 (the
// thread was not Running) are skipped before runs are formed: they neither extend, break nor lengthen a run.
// A key with module kScopeModule is answered as scopeStats of that scope id. Zero instances give count = 0.
Result<Stats> functionStats(const QueryContext& ctx, const Filter& filter, FunctionKey key,
                            Cancel cancel = {});

// Thread-safe. Exact per-instance durations of an instrumentation scope id (StringIdx of the trace string
// table). Begin/End events are paired per thread in timestamp order with stack discipline (see
// scope_pairing.h); an unterminated Begin ends at header.end_ts and is counted in Stats::unterminated; an
// orphan End is ignored and counted in Stats::orphanEnds. Filter: an instance is selected when its Begin lies
// in [t0, t1) and its thread is selected; subtreeRoot and function do not apply to scopes and are ignored.
Result<Stats> scopeStats(const QueryContext& ctx, const Filter& filter, StringIdx scopeId,
                         Cancel cancel = {});

// Thread-safe. State totals of one thread over [t0, t1). Filter::threads, subtreeRoot and function are
// ignored (the thread is the argument).
Result<ThreadStateTotals> threadStateTotals(const QueryContext& ctx, const Filter& filter, ThreadIdx thread,
                                            Cancel cancel = {});

// Thread-safe. Series of frame marks in the trace, sorted by markCount descending (the first is the default
// series of frames()), then earliest first mark, thread, name.
std::vector<FrameSeries> frameSeries(const QueryContext& ctx);

// Thread-safe. The frames of one series (rule 6) that overlap the filter's [t0, t1) (frame.start < t1 and
// frame.end > t0), in time order, each reported whole (not clipped to the filter). Per frame: per-thread
// Running time (state intervals clipped to the frame) and weighted sample time, and the top-N exclusive
// functions of the samples in [start, end). Filter::threads selects the threads reported; subtreeRoot and
// function restrict the samples used for sampleTicks and topFunctions (they never change runningTicks).
// A trace without frame marks gives an empty list.
Result<std::vector<FrameSummary>> frames(const QueryContext& ctx, const Filter& filter,
                                         FrameOptions options = {}, Cancel cancel = {});

// Thread-safe. The n longest of those frames, sorted by duration descending, then start ascending.
Result<std::vector<FrameSummary>> slowestFrames(const QueryContext& ctx, const Filter& filter, uint32_t n,
                                                FrameOptions options = {}, Cancel cancel = {});

// Thread-safe. Display name: "module!function", "module!0x<rva>" for unresolved frames,
// "<unmapped>!0x<addr>",
// "<kernel>", "<root>", or "scope:<id>" for instrumentation scopes.
std::string functionKeyName(const QueryContext& ctx, FunctionKey key);

}  // namespace tachyon::analysis
