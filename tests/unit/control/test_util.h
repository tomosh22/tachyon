// Shared helpers for control / agent tests.
#pragma once

#include "core/platform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tachyon::control::testing_util {

inline unsigned long currentPid() {
#if TACHYON_PLATFORM_WINDOWS
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

#if !TACHYON_PLATFORM_WINDOWS
// A private (per test process) directory for socket files, removed when the process ends. The agent puts the
// per-process instrumentation sockets next to its own socket, named after the hello pid only
// (tachyon-inst-<pid>.sock, see instrumentationAddress), and the tests say hello with made-up pids (1234, 8,
// ...): in a shared directory two test processes running at once (ctest -j) would fight over the same path.
inline const std::filesystem::path& testSocketDir() {
  struct Dir {
    std::filesystem::path path;
    unsigned long owner = currentPid();
    Dir() {
      path = "/tmp/tachyon-t-" + std::to_string(owner);
      std::error_code ec;
      std::filesystem::remove_all(path, ec);  // left over from a crashed run with the same pid
      std::filesystem::create_directories(path, ec);
    }
    ~Dir() {
      if (currentPid() != owner) return;  // a forked child exiting must not remove its parent's sockets
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };
  static const Dir dir;
  return dir.path;
}
#endif

// Unique, short transport address: a pipe name on Windows, a socket path in a private /tmp directory on Linux
// (sun_path is short).
inline std::string testAddress(const std::string& tag) {
  static std::atomic<int> counter{0};
#if TACHYON_PLATFORM_WINDOWS
  const std::string suffix = std::to_string(currentPid()) + "-" + tag + "-" + std::to_string(counter++);
  return R"(\\.\pipe\tachyon-test-)" + suffix;
#else
  return (testSocketDir() / (tag + "-" + std::to_string(counter++) + ".sock")).string();
#endif
}

// Unique temp directory, removed on destruction.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    m_path = std::filesystem::temp_directory_path() /
             ("tachyon_ctl_" + std::to_string(currentPid()) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(m_path);
    std::filesystem::create_directories(m_path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::filesystem::path& path() const { return m_path; }

 private:
  std::filesystem::path m_path;
};

}  // namespace tachyon::control::testing_util
