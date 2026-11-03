// IProcessLauncher (WP-41): starts detached helper processes (the capture agent, with or without elevation;
// the file manager for "Reveal"). Injectable so tests never run pkexec or open a file manager.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct LaunchSpec {
  // Program to run. A bare name ("xdg-open", "explorer.exe") is looked up on PATH.
  std::filesystem::path exe;
  std::vector<std::string> args;  // UTF-8, no shell involved
  // Ask for elevation: Linux runs the program through `pkexec`, Windows uses ShellExecuteEx with the "runas"
  // verb (a UAC prompt).
  bool elevate = false;
  std::string description;  // for logs and error messages
};

struct LaunchedProcess {
  // Process id of what was started (with pkexec: of pkexec itself; 0 if the platform does not tell).
  uint32_t pid = 0;
};

class IProcessLauncher {
 public:
  virtual ~IProcessLauncher() = default;
  // Thread-safe for the system launcher. Starts the process and returns without waiting for it. Fails if it
  // cannot be started at all (not found, elevation refused).
  virtual Result<LaunchedProcess> launch(const LaunchSpec& spec) = 0;
};

// Thread-safe: factory. The real launcher (posix_spawn / ShellExecuteEx). Children are not waited for; the
// Linux implementation reaps the ones that exited whenever it starts another and when it is destroyed.
std::unique_ptr<IProcessLauncher> makeSystemProcessLauncher();

}  // namespace tachyon::viewer
