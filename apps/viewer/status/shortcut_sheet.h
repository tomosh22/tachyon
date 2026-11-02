// Keyboard shortcut cheat sheet (WP-48), opened with `?` (Shift+/). Its content is generated from the
// CommandRegistry (every registered command with its shortcut, grouped by the title prefix before ": ", which
// is the panel or area name) plus the shortcuts that panels document through ShortcutRegistry::add().
#pragma once

#include "viewer/command_registry.h"

#include <string>
#include <vector>

namespace tachyon::viewer {

struct ShortcutEntry {
  std::string group;  // panel or area, e.g. "Timeline"
  std::string keys;   // display text, e.g. "A / Left"
  std::string description;
};

// Registration API for panel-local shortcuts that are not commands (plain keys handled inside a panel).
// Main-thread only. A panel (or App on its behalf) registers once; duplicates (same group and keys) replace.
class ShortcutRegistry {
 public:
  // Main-thread only.
  void add(std::string group, std::string keys, std::string description);
  // Main-thread only.
  const std::vector<ShortcutEntry>& entries() const { return m_entries; }

 private:
  std::vector<ShortcutEntry> m_entries;
};

// Main-thread only. Registers the keys of the built-in panels (timeline, Function Statistics, tables).
void registerBuiltinPanelShortcuts(ShortcutRegistry& registry);

struct ShortcutGroup {
  std::string name;
  std::vector<ShortcutEntry> entries;  // `group` repeated for convenience
};

// Thread-safe: pure function. Groups in order of first appearance (commands first, in registration order,
// then panel shortcuts); entries keep their registration order. A command without a shortcut is listed with
// empty `keys`.
std::vector<ShortcutGroup> buildShortcutSheet(const CommandRegistry& commands,
                                              const ShortcutRegistry& shortcuts);

class ShortcutSheet {
 public:
  // Main-thread only. True once `?` was pressed (or open() called) and until it is closed.
  bool isOpen() const { return m_open; }
  void open() {
    m_open = true;
    m_requestOpen = true;
  }
  void close() { m_open = false; }
  // Main-thread only. Handles the `?` key (ignored while a text input has the keyboard or a modal/popup is
  // open) and draws the modal. Call once per frame.
  void draw(const CommandRegistry& commands, const ShortcutRegistry& shortcuts, bool textInputActive);

  // Main-thread only. The rows drawn the last time (tests).
  const std::vector<ShortcutGroup>& lastGroups() const { return m_groups; }

 private:
  bool m_open = false;
  bool m_requestOpen = false;
  std::vector<ShortcutGroup> m_groups;
};

}  // namespace tachyon::viewer
