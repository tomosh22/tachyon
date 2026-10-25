// Minimal cross-platform child-process helper for the acceptance tests.
#pragma once

#include "core/platform.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::agent::testing_util {

class Process {
 public:
  // Spawns exe with args (inheriting the environment). stdout/stderr go to `outFile` when non-empty.
  // Returns an invalid Process (valid() == false) on failure.
  static Process spawn(const std::string& exe, const std::vector<std::string>& args,
                       const std::filesystem::path& outFile = {});

  Process() = default;
  Process(Process&& other) noexcept;
  Process& operator=(Process&& other) noexcept;
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  // Kills the child if it is still running.
  ~Process();

  bool valid() const;
  // Waits up to timeoutMs for exit. Returns true and sets *exitCode if the child exited.
  bool waitFor(int timeoutMs, int* exitCode);
  // Asks the child to shut down gracefully (SIGTERM / CTRL_BREAK_EVENT).
  void requestTerminate();
  // Forcefully kills the child and reaps it.
  void kill();

 private:
  void reset();
#if TACHYON_PLATFORM_WINDOWS
  void* m_handle = nullptr;
  unsigned long m_pid = 0;
#else
  int m_pid = -1;
#endif
  bool m_reaped = false;
};

// Runs a process to completion with a timeout. Returns its exit code or -1 on spawn failure/timeout.
int runToCompletion(const std::string& exe, const std::vector<std::string>& args,
                    const std::filesystem::path& outFile, int timeoutMs = 60000);

// Reads a whole text file.
std::string readTextFile(const std::filesystem::path& path);

}  // namespace tachyon::agent::testing_util
