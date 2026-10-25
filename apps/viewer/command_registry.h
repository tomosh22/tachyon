// CommandRegistry: named actions listed by the Ctrl+P command palette.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct Command {
  std::string id;        // stable, e.g. "file.open"
  std::string title;     // shown in the palette
  std::string shortcut;  // display only
  std::function<void()> action;
};

class CommandRegistry {
 public:
  // Main-thread only. Replaces an existing command with the same id.
  void add(Command command);
  // Main-thread only.
  const std::vector<Command>& commands() const { return m_commands; }
  // Main-thread only. Case-insensitive subsequence match on the title; best (prefix/substring) matches first.
  std::vector<const Command*> match(const std::string& query) const;
  // Main-thread only. Returns false if no such command.
  bool run(const std::string& id) const;

 private:
  std::vector<Command> m_commands;
};

}  // namespace tachyon::viewer
