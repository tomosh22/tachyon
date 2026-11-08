#include "viewer/document.h"

#include "viewer/analysis_context.h"

#include <atomic>
#include <utility>

namespace tachyon::viewer {

namespace {
std::atomic<int> g_alive{0};
}  // namespace

Document::Document(std::filesystem::path path, std::shared_ptr<const model::TraceModel> model)
    : m_path(std::move(path)),
      m_model(std::move(model)),
      m_analysis(std::make_unique<AnalysisContext>(*this)) {
  ++g_alive;
}

Document::~Document() {
  m_analysis.reset();  // cancels and waits for its queries while the rest of the document is still intact
  --g_alive;
}

int Document::aliveCount() {
  return g_alive.load();
}

const AnalysisContext& Document::analysis() const {
  return *m_analysis;
}

uint64_t Document::totalSamples() const {
  uint64_t total = 0;
  for (const model::SampleColumns& s : m_model->samplesByThread) total += s.ts.size();
  return total;
}

}  // namespace tachyon::viewer
