// Analysis query API (spec 3.7). WP-32 implements topDownTree, bottomUpFunctions and butterfly; the remaining
// 3.7 queries are added by their own work packages and are intentionally not declared here yet:
//   WP-33: linesForFunction, instructionsForFunction (LineAgg, InstructionAgg)
//   WP-34: functionStats, scopeStats (Stats)
//   WP-35: threadStateTotals, frames (ThreadStateTotals, FrameSummary)
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

#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"
#include "symbols/function_key.h"
#include "symbols/symbol_service.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::analysis {

using symbols::FunctionKey;

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
  std::optional<FunctionIdx> function;            // not supported by WP-32: setting it returns Unsupported

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

struct CacheStats {
  uint64_t hits = 0, misses = 0;
};

// Binds a model and its symbols for the queries below and owns the per-filter memo (WP-33 adds the general
// query cache). The model, the symbol service and the symbols' module resolution must stay unchanged and
// alive while the context is used. Queries may run concurrently on one context.
class QueryContext {
 public:
  // Thread-safe (constructor). Requires TraceModel::statesByThread to be built (buildThreadStates).
  QueryContext(const model::TraceModel& model, const symbols::SymbolService& symbols,
               AggregationOptions options = {});
  ~QueryContext();
  QueryContext(const QueryContext&) = delete;
  QueryContext& operator=(const QueryContext&) = delete;

  // All accessors: thread-safe.
  const model::TraceModel& model() const;
  const symbols::SymbolService& symbols() const;
  const AggregationOptions& options() const;
  // Drops every memoized result.
  void clearCache();
  CacheStats cacheStats() const;

  struct Impl;
  Impl& impl() const { return *m_impl; }

 private:
  std::unique_ptr<Impl> m_impl;
};

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

// Thread-safe. Display name: "module!function", "module!0x<rva>" for unresolved frames,
// "<unmapped>!0x<addr>",
// "<kernel>", "<root>", or "scope:<id>" for instrumentation scopes.
std::string functionKeyName(const QueryContext& ctx, FunctionKey key);

}  // namespace tachyon::analysis
