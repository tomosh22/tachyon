// AnalysisContext (WP-43): the per-document holder of the analysis QueryContext(s) that the viewer's panels
// share (Call Graph now; Function List, Butterfly and the source view reuse it).
//
// Keying decision. FunctionKeys change when a module resolves (spec 3.7 clarifications), so a QueryContext
// belongs to one (AggregationOptions, symbol generation) pair: `snapshot()` hands out the context for that
// pair and drops the contexts of older symbol generations from the holder (queries still running on them keep
// them alive through the shared_ptr and finish normally; their results are dropped by the stale-result
// guard). Different AggregationOptions (inline expansion, kernel frame mode) of the same generation are
// separate contexts, so toggling an option and toggling it back is a cache hit. The executor is owned by the
// App (PanelContext::queryExecutor); the contexts fan their scans out through it.
//
// Lifetime. A snapshot holds the model and the symbol service by shared_ptr. The destructor of the
// AnalysisContext (i.e. closing the document) cancels and waits for every task submitted through submit(), so
// no query outlives the document that is being closed.
#pragma once

#include "analysis/query.h"
#include "analysis/query_executor.h"
#include "core/result.h"
#include "model/model.h"
#include "symbols/symbol_service.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace tachyon::viewer {

class Document;

// Children of every callstack node (compressed sparse rows), built once per model. Used to map a call graph
// path back to a CallstackIdx (Filter::subtreeRoot).
class NodeChildIndex {
 public:
  // Thread-safe (constructor): O(nodes).
  explicit NodeChildIndex(const model::TraceModel& model);
  // Thread-safe (const). Child node indices of `node` in increasing order; empty for an unknown node.
  std::span<const uint32_t> children(uint32_t node) const;
  size_t nodeCount() const { return m_offsets.empty() ? 0 : m_offsets.size() - 1; }

 private:
  std::vector<uint32_t> m_offsets;  // nodeCount + 1
  std::vector<uint32_t> m_children;
};

// A QueryContext plus the objects it refers to. Immutable after construction; thread-safe.
class AnalysisSnapshot {
 public:
  // Thread-safe (constructor). Requires model->statesByThread to be built.
  AnalysisSnapshot(std::shared_ptr<const model::TraceModel> model,
                   std::shared_ptr<symbols::SymbolService> symbols, analysis::AggregationOptions options,
                   uint64_t symbolGeneration, analysis::QueryExecutor* executor);
  // Thread-safe.
  const analysis::QueryContext& context() const { return m_context; }
  const model::TraceModel& model() const { return *m_model; }
  const analysis::AggregationOptions& options() const { return m_options; }
  uint64_t symbolGeneration() const { return m_symbolGeneration; }

 private:
  std::shared_ptr<const model::TraceModel> m_model;
  std::shared_ptr<symbols::SymbolService> m_symbols;
  analysis::AggregationOptions m_options;
  uint64_t m_symbolGeneration;
  analysis::QueryContext m_context;
};

class AnalysisContext {
 public:
  // Thread-safe (constructor). `owner` provides the model and the symbol service (read when a snapshot is
  // created) and must outlive this object (it owns it).
  explicit AnalysisContext(const Document& owner);
  // Cancels and waits for every tracked task.
  ~AnalysisContext();
  AnalysisContext(const AnalysisContext&) = delete;
  AnalysisContext& operator=(const AnalysisContext&) = delete;

  // Main-thread only (reads Document::symbols()). The context for `options` at `symbolGeneration`, created on
  // first use. Contexts of other symbol generations are released. Null if the document has no symbol service
  // or its model has no thread states. `options.threads` is ignored for keying (scans use `executor`).
  std::shared_ptr<const AnalysisSnapshot> snapshot(const analysis::AggregationOptions& options,
                                                   uint64_t symbolGeneration,
                                                   analysis::QueryExecutor* executor) const;

  // Thread-safe. Queues `fn(snapshot context, cancel)` on `executor` and tracks the task so that closing the
  // document cancels and waits for it. The snapshot is kept alive by the task.
  template <typename T>
  analysis::QueryFuture<T> submit(
      analysis::QueryExecutor& executor, const analysis::SubmitOptions& options,
      std::shared_ptr<const AnalysisSnapshot> snapshot,
      std::function<Result<T>(const analysis::QueryContext&, analysis::Cancel)> fn) const {
    analysis::QueryFuture<T> future =
        executor.submitQuery<T>(options, [snapshot = std::move(snapshot), fn = std::move(fn)](
                                             analysis::Cancel c) { return fn(snapshot->context(), c); });
    track(executor, future.handle());
    return future;
  }

  // Thread-safe. The child index of the model, built on first use (O(nodes)).
  std::shared_ptr<const NodeChildIndex> childIndex() const;
  // Thread-safe. Number of snapshots currently held (tests).
  size_t snapshotCount() const;

 private:
  void track(analysis::QueryExecutor& executor, const analysis::TaskHandle& handle) const;

  const Document& m_owner;
  mutable std::mutex m_mutex;
  mutable std::vector<std::shared_ptr<const AnalysisSnapshot>> m_snapshots;
  mutable std::shared_ptr<const NodeChildIndex> m_childIndex;
  mutable std::vector<std::pair<analysis::QueryExecutor*, analysis::TaskHandle>> m_tasks;
};

}  // namespace tachyon::viewer
