// Launching the target with a stop-on-start handshake (WP-70), the Linux counterpart of CREATE_SUSPENDED.
//
//   parent                                   child
//   ------                                   -----
//   fork() ------------------------------->  (blocks reading the release pipe; nothing of the target has run)
//   open + enable the perf events
//   release() writes the release byte ---->  chdir, execve(exe)   <- the first instruction of the target is
//   release() waits for the exec result      (the exec itself is observed: COMM, MMAP2 of the exe and ld.so)
//
// The exec result travels over a close-on-exec pipe: EOF means execve succeeded, four bytes are the errno of
// its failure. A child whose parent dies before releasing it sees EOF on the release pipe and exits without
// ever running the target. Only async-signal-safe calls are made between fork and execve.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tachyon::capture::perf {

struct LaunchSpec {
  std::string exe;
  std::vector<std::string> args;           // argv[1..]
  std::string cwd;                         // empty: inherit
  std::map<std::string, std::string> env;  // merged over the parent's environment
};

// Thread-safe: pure function. `parent` is a list of "NAME=value" strings; an override replaces the variable
// of the same name (in place) or is appended. Entries without '=' are kept.
std::vector<std::string> mergeEnvironment(const std::vector<std::string>& parent,
                                          const std::map<std::string, std::string>& overrides);

// Thread-safe: pure function. execvp's rule: a name containing '/' is used as is, otherwise the first
// executable regular file along `pathVar` (colon separated; an empty entry means the current directory).
// Relative results are resolved against `cwd` when it is not empty. NotFound when nothing matches.
Result<std::string> resolveExecutable(const std::string& exe, const std::string& pathVar,
                                      const std::string& cwd);

// The launched process. Move-only. Thread-safe for exited() / pid(); release() and terminate() must not race.
class LaunchedProcess {
 public:
  ~LaunchedProcess();
  LaunchedProcess(LaunchedProcess&&) noexcept;
  LaunchedProcess& operator=(LaunchedProcess&&) noexcept;
  LaunchedProcess(const LaunchedProcess&) = delete;
  LaunchedProcess& operator=(const LaunchedProcess&) = delete;

  // Thread-safe. Forks a child that waits for release(). NotFound when the executable cannot be found.
  static Result<LaunchedProcess> spawnParked(const LaunchSpec& spec);

  // Thread-safe. The child's pid (valid from spawnParked on).
  uint32_t pid() const { return m_pid; }
  // Thread-safe. True while the child has not been released.
  bool parked() const { return m_state == State::Parked; }

  // Not thread-safe. Lets the child run execve and waits for its result: Ok once the new program image is
  // running, otherwise the exec error (the child is reaped).
  Result<void> release();

  // Thread-safe. True once the child has terminated (reaps it; the exit status is then available).
  bool exited() const;
  // Thread-safe. Exit status as from waitpid; meaningful after exited() returned true.
  int waitStatus() const { return m_status; }

  // Not thread-safe. SIGKILL and reap. Safe on an exited process.
  void terminate();

 private:
  enum class State { Parked, Running, Dead };
  LaunchedProcess() = default;
  void closePipes();

  uint32_t m_pid = 0;
  State m_state = State::Dead;
  int m_releaseFd = -1;  // write end
  int m_errFd = -1;      // read end of the exec error pipe
  mutable bool m_reaped = false;
  mutable int m_status = 0;
};

}  // namespace tachyon::capture::perf
