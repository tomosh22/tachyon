#include "viewer/command_registry.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tachyon::viewer {

namespace {

std::string lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

// 0 = no match; higher is better.
int score(const std::string& titleLower, const std::string& queryLower) {
  if (queryLower.empty()) return 1;
  const size_t pos = titleLower.find(queryLower);
  if (pos == 0) return 4;
  if (pos != std::string::npos) return 3;
  size_t at = 0;
  for (const char c : queryLower) {
    at = titleLower.find(c, at);
    if (at == std::string::npos) return 0;
    ++at;
  }
  return 2;
}

}  // namespace

void CommandRegistry::add(Command command) {
  for (Command& c : m_commands) {
    if (c.id == command.id) {
      c = std::move(command);
      return;
    }
  }
  m_commands.push_back(std::move(command));
}

std::vector<const Command*> CommandRegistry::match(const std::string& query) const {
  const std::string q = lower(query);
  std::vector<std::pair<int, const Command*>> scored;
  for (const Command& c : m_commands) {
    const int s = score(lower(c.title), q);
    if (s > 0) scored.emplace_back(s, &c);
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<const Command*> out;
  out.reserve(scored.size());
  for (const auto& [s, c] : scored) out.push_back(c);
  return out;
}

bool CommandRegistry::run(const std::string& id) const {
  for (const Command& c : m_commands) {
    if (c.id == id) {
      if (c.action) c.action();
      return true;
    }
  }
  return false;
}

}  // namespace tachyon::viewer
