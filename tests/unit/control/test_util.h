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

// Unique, short transport address: a pipe name on Windows, a /tmp socket path on Linux (sun_path is short).
inline std::string testAddress(const std::string& tag) {
  static std::atomic<int> counter{0};
  const std::string suffix = std::to_string(currentPid()) + "-" + tag + "-" + std::to_string(counter++);
#if TACHYON_PLATFORM_WINDOWS
  return R"(\\.\pipe\tachyon-test-)" + suffix;
#else
  return "/tmp/tachyon-t-" + suffix + ".sock";
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
