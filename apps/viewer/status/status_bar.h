// StatusBar (WP-48): the content of the bottom bar of the main window.
//   <trace file> | capture duration | threads | samples | active filter      Symbols: resolved/total (N
//   failed)
// "Symbols:" shows a progress bar while modules resolve; hovering it lists every module with its state
// (Pending / Loading / Resolved / NotFound / Failed) and the reason of a failure. App::drawStatusBar owns the
// window and the job spinner and calls draw() for the left part.
#pragma once

#include "viewer/document.h"
#include "viewer/selection_bus.h"
#include "viewer/status/symbol_status.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tachyon::viewer {

// Thread-safe: pure functions.
// "tiny.tachyon | 1.25 s | 4 threads | 12,345 samples" (capture duration from the file header).
std::string describeDocument(const Document& document);
// Duration "850 ms" / "1.25 s" / "2 min 5 s" for a tick count at `clockHz` (empty if the clock is unknown).
std::string formatDuration(uint64_t ticks, uint64_t clockHz);
// "none", or e.g. "time 0.250-1.500 s, 2 threads, subtree #7" for the bus's time filter and selections.
std::string describeFilter(const SelectionState& state, const model::TraceModel& model);

class StatusBar {
 public:
  // Main-thread only. Call once per frame before draw(): recomputes the per-document module table when the
  // document changed.
  void update(const Document* document, uint64_t documentGeneration);
  // Main-thread only. Draws into the current menu bar (the caller opened it). `error` (non-empty) replaces
  // the document text.
  void draw(const Document* document, const SelectionState& state, const std::string& error);

  // Main-thread only. The numbers of the last update()/draw().
  const SymbolSummary& summary() const { return m_summary; }
  // Main-thread only. The text drawn for "Symbols: ..." in the last frame ("" without a document).
  const std::string& symbolsText() const { return m_symbolsText; }
  // Main-thread only. The rows the hover table showed the last time it was drawn.
  const std::vector<ModuleSymbolRow>& tooltipRows() const { return m_tooltipRows; }
  uint64_t tooltipDraws() const { return m_tooltipDraws; }
  // Main-thread only. Screen rectangle of the symbols text in the last frame (tests hover it).
  struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };
  const Rect& symbolsRect() const { return m_symbolsRect; }
  const std::vector<bool>& referenced() const { return m_referenced; }
  // Main-thread only. True in the frame in which the "Symbols:" text was clicked (App opens the Symbols
  // panel).
  bool symbolsClicked() const { return m_symbolsClicked; }

 private:
  bool m_haveDocument = false;
  uint64_t m_generation = 0;
  std::vector<bool> m_referenced;
  SymbolSummary m_summary;
  std::string m_symbolsText;
  std::vector<ModuleSymbolRow> m_tooltipRows;
  uint64_t m_tooltipDraws = 0;
  Rect m_symbolsRect;
  bool m_symbolsClicked = false;
};

}  // namespace tachyon::viewer
