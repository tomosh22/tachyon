// LiveStreamTracker (WP-65b): how an analysis panel keeps the user's place when a Live document is replaced
// by the next snapshot of the same live capture (every ~500 ms).
//
// A file document is replaced rarely, and the panels then start from scratch (the tree collapses, the
// selection goes). A live capture replaces its document constantly; the panels must instead keep what the
// user did (expanded nodes, scroll, selected function, sort order) and only refresh their numbers: the old
// result stays on screen until the query on the new snapshot delivers. Documents of one live capture share a
// stream id (Document::streamId()); a panel asks the tracker, from onDocumentChanged(), whether the new
// document continues the stream of the previous one.
#pragma once

#include "viewer/document.h"

#include <cstdint>

namespace tachyon::viewer::common {

class LiveStreamTracker {
 public:
  // Main-thread only. Call from IPanel::onDocumentChanged(). True when `document` is a Live document of the
  // same capture as the previous one (keep the state, drop only what refers to the old snapshot).
  bool onDocument(const Document* document) {
    const bool live = document != nullptr && document->isLive();
    m_keepPending = live && m_stream != 0 && document->streamId() == m_stream;
    m_stream = live ? document->streamId() : 0;
    return m_keepPending;
  }
  // Main-thread only. Call from update() when the document generation changed: true once after onDocument()
  // returned true (the generation change it announced).
  bool consumeKeep() {
    const bool keep = m_keepPending;
    m_keepPending = false;
    return keep;
  }

 private:
  uint64_t m_stream = 0;
  bool m_keepPending = false;
};

}  // namespace tachyon::viewer::common
