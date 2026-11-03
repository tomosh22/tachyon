// WarningsBanner (WP-48): a dismissible banner below the menu bar listing the capture warnings of the open
// trace (see capture_warnings.h). Dismissal is remembered per document (by path, for the session).
#pragma once

#include "viewer/document.h"
#include "viewer/status/capture_warnings.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tachyon::viewer {

class WarningsBanner {
 public:
  // Main-thread only. Recomputes the warnings when `document` or `documentGeneration` changed.
  void update(const Document* document, uint64_t documentGeneration);
  // Main-thread only. Warnings of the current document (also while dismissed).
  const std::vector<CaptureWarning>& warnings() const { return m_warnings; }
  // Main-thread only. True while the banner is on screen: there are warnings and they were not dismissed.
  bool visible() const { return !m_warnings.empty() && !m_dismissed; }
  bool dismissed() const { return m_dismissed; }
  // Main-thread only. Hides the banner for the current document.
  void dismiss();
  // Main-thread only (WP-64). Adds a warning that does not come from the model (e.g. the memory ceiling made
  // the viewer free data); replaces an earlier one of the same kind and shows the banner again even if it was
  // dismissed. Lives until the document changes.
  void addNotice(CaptureWarningKind kind, std::string text);
  // Main-thread only (WP-67). Remembers the free-text warnings of the agent's `capture_done` for the trace
  // file `path` (spool caps hit, refused connections, failed merges). They join the banner of that trace when
  // it is open (now or later in the session), deduplicated against the trace's own warnings.
  void addAgentWarnings(const std::filesystem::path& path, std::vector<std::string> warnings);
  // Main-thread only. Draws the banner as a top side bar (below the main menu bar) when visible().
  void draw();
  // Main-thread only. Height used by the last draw (tests).
  float lastHeight() const { return m_lastHeight; }

 private:
  static std::string keyOf(const std::filesystem::path& path);
  void recollect(const Document& document);
  bool m_haveDocument = false;
  uint64_t m_generation = 0;
  std::string m_key;
  bool m_dismissed = false;
  std::vector<CaptureWarning> m_warnings;
  std::set<std::string> m_dismissedDocuments;
  std::map<std::string, std::vector<std::string>> m_agentWarnings;  // by document key
  std::vector<CaptureWarning> m_notices;                            // addNotice() entries of this document
  bool m_agentDirty = false;  // agent warnings for the open document arrived after update() collected
  float m_lastHeight = 0.0f;
};

}  // namespace tachyon::viewer
