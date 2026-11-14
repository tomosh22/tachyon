// Document: one open trace plus the services later work packages attach to it.
#pragma once

#include "model/model.h"
#include "symbols/symbol_service.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <utility>

namespace tachyon::viewer {

class AnalysisContext;  // analysis_context.h

namespace timeline {
struct LodShared;  // timeline/timeline_data.h
}  // namespace timeline

// WP-65b: what a Live document knows about the snapshot it shows.
struct LiveSnapshotInfo {
  uint64_t generation = 0;      // LiveModel::generation() the snapshot was taken at
  uint64_t nowTs = 0;           // newest timestamp the live model had seen (trace clock)
  uint64_t newestSampleTs = 0;  // newest sample in the window (0: none)
  uint64_t droppedLive = 0;     // live events the encoder / decoder dropped so far (not trace data)
  uint64_t samplesInWindow = 0;
  bool ended = false;  // the capture is over: this is the last snapshot
};

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
  // Cancels and waits for the queries running through analysis().
  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;
  // Thread-safe. Documents alive in this process (leak guard for the live view, which makes one per
  // snapshot).
  static int aliveCount();

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

  // WP-43: the analysis contexts shared by the panels (see analysis_context.h). Thread-safe.
  const AnalysisContext& analysis() const;

  // Thread-safe: pure function on the model. Total samples over all threads.
  uint64_t totalSamples() const;

  // WP-64 (streaming load): a preview document holds a time prefix of the file (model().loadStatus) and is
  // replaced by the complete document of the same stream. Panels that want to keep the user's place across
  // the swap compare streamId(): documents of one open share it (0 = not part of a stream).
  bool isPreview() const { return !m_model->loadStatus.complete; }
  uint64_t streamId() const { return m_streamId; }
  // Main-thread only, before the document is shown.
  void setStreamId(uint64_t id) { m_streamId = id; }

  // WP-65b (live view): a Live document is one snapshot of a LiveModel's window. Successive snapshots of one
  // live capture share streamId(), like the previews of a streaming load, so panels can keep the user's
  // place across the swap. Main-thread only, before the document is shown.
  bool isLive() const { return m_isLive; }
  const LiveSnapshotInfo& live() const { return m_live; }
  void setLive(const LiveSnapshotInfo& info) {
    m_isLive = true;
    m_live = info;
  }
  // The timeline index built off the UI thread together with the snapshot (null: the timeline builds its
  // own). Main-thread only, before the document is shown.
  const std::shared_ptr<timeline::LodShared>& prebuiltLod() const { return m_prebuiltLod; }
  void setPrebuiltLod(std::shared_ptr<timeline::LodShared> lod) { m_prebuiltLod = std::move(lod); }

 private:
  std::filesystem::path m_path;
  uint64_t m_streamId = 0;
  bool m_isLive = false;
  LiveSnapshotInfo m_live;
  std::shared_ptr<timeline::LodShared> m_prebuiltLod;  // owns a reference to the model it was built for
  std::shared_ptr<const model::TraceModel> m_model;
  std::shared_ptr<symbols::SymbolService> m_symbols;  // declared after m_model: destroyed first
  std::shared_ptr<IQueryExecutor> m_queries;
  std::shared_ptr<ILodProvider> m_lod;
  std::unique_ptr<AnalysisContext> m_analysis;  // last: destroyed first (cancels and waits for its queries)
};

}  // namespace tachyon::viewer
