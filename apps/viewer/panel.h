// IPanel: the extension point for WP-41..48. Panels register with the PanelRegistry; App never changes.
#pragma once

#include "viewer/selection_bus.h"
#include "viewer/settings.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::viewer {

class Document;
class JobQueue;

// What a panel may touch while drawing. `document` is null when no trace is open. Valid only during the call.
// Panels must not cache anything derived from the document across calls except in explicit caches that they
// invalidate when `documentGeneration` changes.
struct PanelContext {
  const Document* document = nullptr;
  SelectionBus* selection = nullptr;
  const Settings* settings = nullptr;
  // Main-thread only. Background jobs (completions are delivered on the main thread). Null in bare test
  // setups.
  JobQueue* jobs = nullptr;
  // Increments every time a document is opened or closed; 0 while none was ever opened.
  uint64_t documentGeneration = 0;
  // Hook behind requestRedraw(); set by App.
  void (*redrawFn)(void*) = nullptr;
  void* redrawUser = nullptr;

  // Main-thread only. Asks App to draw a few more frames even if no input arrives (e.g. a job finished).
  void requestRedraw() const {
    if (redrawFn) redrawFn(redrawUser);
  }
};

class IPanel {
 public:
  virtual ~IPanel() = default;

  // Main-thread only. Unique, human readable; used as the window title and in the View menu.
  virtual const char* name() const = 0;
  // Main-thread only. Called every frame while the panel is visible; must open its own ImGui window
  // (ImGui::Begin(name()...)) and honour *open so the View menu and the window close button work.
  virtual void draw(const PanelContext& context, bool* open) = 0;
  // Main-thread only. Optional: called after the SelectionBus changes (also for Hovered).
  virtual void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                  SelectionField field) {
    (void)context;
    (void)state;
    (void)field;
  }
  // Main-thread only. Optional: called when a document finished loading (non-null) or closed (null).
  virtual void onDocumentChanged(const Document* document) { (void)document; }
};

struct PanelEntry {
  std::unique_ptr<IPanel> panel;
  bool open = true;
};

class PanelRegistry {
 public:
  // Main-thread only. Takes ownership. Returns false (and discards the panel) if the name is already used.
  bool add(std::unique_ptr<IPanel> panel, bool openByDefault = true);
  // Main-thread only.
  std::vector<PanelEntry>& entries() { return m_entries; }
  const std::vector<PanelEntry>& entries() const { return m_entries; }
  // Main-thread only. nullptr if unknown.
  PanelEntry* find(const std::string& name);

 private:
  std::vector<PanelEntry> m_entries;
};

}  // namespace tachyon::viewer
