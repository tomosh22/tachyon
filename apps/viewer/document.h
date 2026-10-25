// Document: one open trace plus the services later work packages attach to it.
#pragma once

#include "model/model.h"
#include "symbols/symbol_service.h"

#include <filesystem>
#include <memory>
#include <utility>

namespace tachyon::viewer {

// Extension slots. Each is an empty interface for now; the owning work package replaces the body (adds
// methods) and installs an implementation with Document::setSymbols() etc. App and existing panels need no
// restructuring because they only ever reach a service through the Document.

// WP-33 (analysis query executor): runs Filter-based queries against the model.
class IQueryExecutor {
 public:
  virtual ~IQueryExecutor() = default;
};

// WP-36 (timeline LOD): multi-resolution summaries for the timeline.
class ILodProvider {
 public:
  virtual ~ILodProvider() = default;
};

class Document {
 public:
  // Thread-safe (constructs a value).
  Document(std::filesystem::path path, std::shared_ptr<const model::TraceModel> model);

  // All accessors: thread-safe for concurrent const access. The model is immutable once loaded.
  const std::filesystem::path& path() const { return m_path; }
  const model::TraceModel& model() const { return *m_model; }
  std::shared_ptr<const model::TraceModel> sharedModel() const { return m_model; }

  // Service slots. Getters: thread-safe for const access; setters: main-thread only. Null until installed.
  const std::shared_ptr<symbols::SymbolService>& symbols() const { return m_symbols; }
  const std::shared_ptr<IQueryExecutor>& queries() const { return m_queries; }
  const std::shared_ptr<ILodProvider>& lod() const { return m_lod; }
  void setSymbols(std::shared_ptr<symbols::SymbolService> s) { m_symbols = std::move(s); }
  void setQueries(std::shared_ptr<IQueryExecutor> q) { m_queries = std::move(q); }
  void setLod(std::shared_ptr<ILodProvider> l) { m_lod = std::move(l); }

  // Thread-safe: pure function on the model. Total samples over all threads.
  uint64_t totalSamples() const;

 private:
  std::filesystem::path m_path;
  std::shared_ptr<const model::TraceModel> m_model;
  std::shared_ptr<symbols::SymbolService> m_symbols;  // declared after m_model: destroyed first
  std::shared_ptr<IQueryExecutor> m_queries;
  std::shared_ptr<ILodProvider> m_lod;
};

}  // namespace tachyon::viewer
