#include "viewer/panel.h"

#include <utility>

namespace tachyon::viewer {

bool PanelRegistry::add(std::unique_ptr<IPanel> panel, bool openByDefault) {
  if (!panel || find(panel->name()) != nullptr) return false;
  m_entries.push_back({std::move(panel), openByDefault});
  return true;
}

PanelEntry* PanelRegistry::find(const std::string& name) {
  for (PanelEntry& e : m_entries) {
    if (name == e.panel->name()) return &e;
  }
  return nullptr;
}

}  // namespace tachyon::viewer
