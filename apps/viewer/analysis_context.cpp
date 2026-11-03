#include "viewer/analysis_context.h"

#include "analysis/query_cache.h"
#include "viewer/document.h"

#include <algorithm>

namespace tachyon::viewer {

AnalysisSnapshot::AnalysisSnapshot(std::shared_ptr<const model::TraceModel> model,
                                   std::shared_ptr<symbols::SymbolService> symbols,
                                   analysis::AggregationOptions options, uint64_t symbolGeneration,
                                   analysis::QueryExecutor* executor, size_t cacheBudgetBytes)
    : m_model(std::move(model)),
      m_symbols(std::move(symbols)),
      m_options(options),
      m_symbolGeneration(symbolGeneration),
      m_context(*m_model, *m_symbols, options, analysis::QueryCacheConfig{cacheBudgetBytes}) {
  if (executor) m_context.setExecutor(executor);
}

AnalysisContext::AnalysisContext(const Document& owner) : m_owner(owner) {}

AnalysisContext::~AnalysisContext() {
  std::vector<std::pair<analysis::QueryExecutor*, analysis::TaskHandle>> tasks;
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    tasks.swap(m_tasks);
  }
  for (auto& [executor, handle] : tasks) {
    if (!handle.finished()) executor->cancel(handle);
  }
  for (auto& [executor, handle] : tasks) handle.wait();
}

std::shared_ptr<const AnalysisSnapshot> AnalysisContext::snapshot(const analysis::AggregationOptions& options,
                                                                  uint64_t symbolGeneration,
                                                                  analysis::QueryExecutor* executor) const {
  const std::shared_ptr<symbols::SymbolService>& symbols = m_owner.symbols();
  if (!symbols) return nullptr;
  const std::shared_ptr<const model::TraceModel> model = m_owner.sharedModel();
  if (model->statesByThread.size() != model->threads.size()) return nullptr;
  const std::lock_guard<std::mutex> lock(m_mutex);
  std::erase_if(m_snapshots, [&](const auto& s) { return s->symbolGeneration() != symbolGeneration; });
  for (const auto& s : m_snapshots) {
    if (s->options().expandInline == options.expandInline &&
        s->options().kernelFrames == options.kernelFrames &&
        s->options().blendScopes == options.blendScopes && s->options().groupByFiber == options.groupByFiber)
      return s;
  }
  m_snapshots.push_back(std::make_shared<const AnalysisSnapshot>(model, symbols, options, symbolGeneration,
                                                                 executor, m_cacheBudget));
  return m_snapshots.back();
}

size_t AnalysisContext::cacheBytes() const {
  const std::lock_guard<std::mutex> lock(m_mutex);
  size_t bytes = 0;
  for (const auto& s : m_snapshots) bytes += s->context().cacheStats().bytes;
  return bytes;
}

void AnalysisContext::setCacheBudget(size_t bytes) const {
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_cacheBudget = bytes;
  for (const auto& s : m_snapshots) s->context().cache().setBudget(bytes);
}

size_t AnalysisContext::snapshotCount() const {
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_snapshots.size();
}

void AnalysisContext::track(analysis::QueryExecutor& executor, const analysis::TaskHandle& handle) const {
  const std::lock_guard<std::mutex> lock(m_mutex);
  std::erase_if(m_tasks, [](const auto& t) { return t.second.finished(); });
  m_tasks.emplace_back(&executor, handle);
}

}  // namespace tachyon::viewer
