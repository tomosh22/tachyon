#include "viewer/document.h"

#include "viewer/analysis_context.h"

#include <utility>

namespace tachyon::viewer {

Document::Document(std::filesystem::path path, std::shared_ptr<const model::TraceModel> model)
    : m_path(std::move(path)),
      m_model(std::move(model)),
      m_analysis(std::make_unique<AnalysisContext>(*this)) {}

Document::~Document() = default;

const AnalysisContext& Document::analysis() const {
  return *m_analysis;
}

uint64_t Document::totalSamples() const {
  uint64_t total = 0;
  for (const model::SampleColumns& s : m_model->samplesByThread) total += s.ts.size();
  return total;
}

}  // namespace tachyon::viewer
