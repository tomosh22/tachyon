// Local process enumeration for the Attach dialog (WP-41). Linux reads /proc; Windows uses Toolhelp32.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::viewer {

struct ProcessInfo {
  uint32_t pid = 0;
  std::string name;         // executable / comm name
  std::string user;         // account name ("" if unknown)
  std::string commandLine;  // arguments joined by spaces (Windows: the image path)
};

// Thread-safe (reads the OS; call it off the UI thread). Lists the processes visible to this user, sorted by
// pid. Kernel threads (empty command line on Linux) are left out. `procRoot` is "/proc" except in tests;
// ignored on Windows.
Result<std::vector<ProcessInfo>> enumerateProcesses(const std::filesystem::path& procRoot = "/proc");

// Thread-safe: pure function. Indices of the processes whose pid, name, user or command line contain `text`
// (case-insensitive; empty text matches all), in list order.
std::vector<size_t> filterProcesses(const std::vector<ProcessInfo>& list, std::string_view text);

}  // namespace tachyon::viewer
