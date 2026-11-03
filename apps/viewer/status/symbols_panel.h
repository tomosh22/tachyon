// The "Symbols" panel (WP-48): the modules of the open trace whose symbols are not (yet) resolved, with the
// reason (SymbolService::moduleError). Per module: "Retry" and "Copy module info". Above the table:
// "Add symbol path..." (opens the PathPicker; the chosen directory or symbol file is persisted in
// Settings::symbolSearchPaths) and "Retry all". The list of configured search paths is shown below.
//
// Cost of Retry / Add symbol path: the document's symbol resolution runs again for ALL modules (a fresh
// SymbolService; modules that resolved before are loaded again), because SymbolService has no per-module
// retry. Resolved modules stay usable meanwhile.
#pragma once

#include "viewer/panel.h"
#include "viewer/status/symbol_status.h"
#include "viewer/status/toast.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::viewer {

// What the panel asks of the application (all main-thread only).
struct SymbolsPanelHost {
  std::function<void()> addSymbolPath;  // open the picker
  std::function<void()> retry;          // re-run the symbol resolution of the document
  std::function<void(ToastLevel, std::string)> toast;
  // WP-67: what the last scan of the search paths found wrong (stopped scans, unreadable directories).
  std::function<const std::vector<std::string>&()> pathProblems;
  std::function<bool()> scanRunning;  // the search paths are still being scanned
};

class SymbolsPanel final : public IPanel {
 public:
  static constexpr const char* kName = "Symbols";

  explicit SymbolsPanel(SymbolsPanelHost host);

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // Main-thread only. Rebuilds the rows from the document and the symbol service (draw() calls it).
  void update(const PanelContext& context);
  // Main-thread only. Copies "name, path, id, state, reason" of the row to the clipboard.
  void copyModuleInfo(const PanelContext& context, const ModuleSymbolRow& row);

  // ---- state (tests); main-thread only ----
  // The unresolved referenced modules as of the last update().
  const std::vector<ModuleSymbolRow>& rows() const { return m_rows; }
  const SymbolSummary& summary() const { return m_summary; }
  // Screen rectangle of the "Add symbol path..." button in the last frame (zero until drawn).
  struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };
  const Rect& addButton() const { return m_addButton; }
  uint64_t focusRequests() const { return m_focusRequests; }

 private:
  SymbolsPanelHost m_host;
  bool m_haveDocument = false;
  uint64_t m_generation = 0;
  std::vector<bool> m_referenced;
  std::vector<ModuleSymbolRow> m_rows;
  SymbolSummary m_summary;
  bool m_focus = false;
  uint64_t m_focusRequests = 0;
  Rect m_addButton;
};

// Thread-safe: factory.
std::unique_ptr<IPanel> makeSymbolsPanel(SymbolsPanelHost host);

}  // namespace tachyon::viewer
