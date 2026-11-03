#include "viewer/capture/process_list.h"

#include "control/process_list.h"

#include <algorithm>
#include <cctype>

namespace tachyon::viewer {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

// The enumeration itself lives in libs/control (the agent's `processes` command serves the same list).
Result<std::vector<ProcessInfo>> enumerateProcesses(const std::filesystem::path& procRoot) {
  auto listed = control::listProcesses(procRoot);
  if (!listed.ok()) return std::move(listed).error();
  std::vector<ProcessInfo> out;
  out.reserve(listed->size());
  for (control::ProcessEntry& e : *listed)
    out.push_back({e.pid, std::move(e.name), std::move(e.user), std::move(e.cmdline)});
  return out;
}

std::vector<size_t> filterProcesses(const std::vector<ProcessInfo>& list, std::string_view text) {
  std::vector<size_t> out;
  const std::string needle = lower(text);
  for (size_t i = 0; i < list.size(); ++i) {
    const ProcessInfo& p = list[i];
    if (needle.empty() || std::to_string(p.pid).find(needle) != std::string::npos ||
        lower(p.name).find(needle) != std::string::npos || lower(p.user).find(needle) != std::string::npos ||
        lower(p.commandLine).find(needle) != std::string::npos)
      out.push_back(i);
  }
  return out;
}

}  // namespace tachyon::viewer
