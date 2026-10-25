// Launching and watching the target process of a capture (WP-13).
//
// The pure helpers (command line quoting, environment merging) compile and are unit-tested everywhere.
// TargetProcess wraps the Win32 handles; elsewhere every factory returns ErrorCode::Unsupported.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tachyon::capture::etw {

// The variable through which a launched process finds the agent (the SDK connects to it).
inline constexpr std::string_view kAgentPipeEnvVar = "TACHYON_AGENT_PIPE";

// Thread-safe: pure function. Quotes one argument following the MSVC CRT / CommandLineToArgvW rules: empty
// arguments and arguments with space, tab, newline or '"' are wrapped in quotes; backslashes that precede a
// quote (or the closing quote) are doubled and embedded quotes are escaped.
std::string quoteArgument(std::string_view arg);

// Thread-safe: pure function. `"exe" arg1 arg2 ...` with each element quoted as needed.
std::string buildCommandLine(std::string_view exe, const std::vector<std::string>& args);

// Thread-safe: pure function. Inverse of the quoting rules above (CommandLineToArgvW for one string). Used by
// the CLI to turn `--args "a b \"c d\""` into tokens, and by the tests as a round-trip check.
std::vector<std::string> splitCommandLine(std::string_view commandLine);

using EnvVar = std::pair<std::string, std::string>;

// Thread-safe: pure function. parent environment + overrides (an override replaces a variable of the same
// name, compared case-insensitively when `caseInsensitive`; the override's spelling wins). The result is
// sorted by upper-cased name, the order CreateProcess expects for an environment block. Entries of `parent`
// with an empty name are dropped; a leading '=' belongs to the name (the hidden "=C:" variables).
std::vector<EnvVar> mergeEnvironment(const std::vector<EnvVar>& parent,
                                     const std::map<std::string, std::string>& overrides,
                                     bool caseInsensitive = true);

struct LaunchSpec {
  std::string exe;
  std::vector<std::string> args;
  std::string cwd;                         // empty: the agent's working directory
  std::map<std::string, std::string> env;  // merged over the agent's environment
};

// A process the backend launched or attached to; the handle used to watch it. Move-only.
// Thread-safe for const members; resume() and terminate() must not race with each other.
class TargetProcess {
 public:
  ~TargetProcess();
  TargetProcess(TargetProcess&&) noexcept;
  TargetProcess& operator=(TargetProcess&&) noexcept;
  TargetProcess(const TargetProcess&) = delete;
  TargetProcess& operator=(const TargetProcess&) = delete;

  // Thread-safe. CreateProcessW(CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT) with the merged environment.
  // The process is placed in a job object when possible, so descendants can be watched (see allExited()).
  // The main thread stays suspended until resume().
  static Result<TargetProcess> launchSuspended(const LaunchSpec& spec);
  // Thread-safe. Opens an existing process for exit watching only.
  static Result<TargetProcess> open(uint32_t pid);

  // Thread-safe.
  uint32_t pid() const;
  // Thread-safe. ResumeThread on the main thread of a process created by launchSuspended().
  Result<void> resume();
  // Thread-safe. True once the process itself has exited.
  bool exited() const;
  // Thread-safe. True once the process and, when the job object could be used, every descendant exited.
  bool allExited() const;
  // Thread-safe. True when descendants are tracked through a job object.
  bool tracksDescendants() const;
  // Thread-safe. TerminateProcess on the root (best effort).
  void terminate();

 private:
  struct Impl;
  explicit TargetProcess(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

}  // namespace tachyon::capture::etw
