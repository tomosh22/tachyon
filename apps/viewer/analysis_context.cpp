#include "viewer/analysis_context.h"

#include "viewer/document.h"

#include <algorithm>

namespace tachyon::viewer {

NodeChildIndex::NodeChildIndex(const model::TraceModel& model) {
  const size_t n = model.nodes.size();
  m_offsets.assign(n + 1, 0);
  for (size_t i = 1; i < n; ++i) {
    const uint32_t p = model.nodes[i].parent.get();
    if (p < n) ++m_offsets[p + 1];
  }
  for (size_t i = 0; i < n; ++i) m_offsets[i + 1] += m_offsets[i];
  m_children.resize(m_offsets[n]);
  std::vector<uint32_t> fill(m_offsets.begin(), m_offsets.begin() + static_cast<std::ptrdiff_t>(n));
  for (size_t i = 1; i < n; ++i) {
    const uint32_t p = model.nodes[i].parent.get();
    if (p < n) m_children[fill[p]++] = static_cast<uint32_t>(i);
  }
}

std::span<const uint32_t> NodeChildIndex::children(uint32_t node) const {
  if (node >= nodeCount()) return {};
  return std::span<const uint32_t>(m_children)
      .subspan(m_offsets[node], m_offsets[node + 1] - m_offsets[node]);
}

AnalysisSnapshot::AnalysisSnapshot(std::shared_ptr<const model::TraceModel> model,
                                   std::shared_ptr<symbols::SymbolService> symbols,
                                   analysis::AggregationOptions options, uint64_t symbolGeneration,
                                   analysis::QueryExecutor* executor)
    : m_model(std::move(model)),
      m_symbols(std::move(symbols)),
      m_options(options),
      m_symbolGeneration(symbolGeneration),
      m_context(*m_model, *m_symbols, options) {
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
  m_snapshots.push_back(
      std::make_shared<const AnalysisSnapshot>(model, symbols, options, symbolGeneration, executor));
  return m_snapshots.back();
}

std::shared_ptr<const NodeChildIndex> AnalysisContext::childIndex() const {
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_childIndex) m_childIndex = std::make_shared<const NodeChildIndex>(m_owner.model());
  return m_childIndex;
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
