// Fixture for tests that need a fresh SDK runtime per test: clean environment, temp working directory, and a
// restart of the process-wide runtime before and after.
#pragma once

#include "fake_agent.h"
#include "inst_reader.h"
#include "sdk_internal.h"
#include "test_env.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tachyon::sdk_testing {

inline uint32_t osThreadId() {
#if defined(_WIN32)
  return static_cast<uint32_t>(GetCurrentThreadId());
#else
  return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

inline std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class RuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_oldCwd = std::filesystem::current_path();
    static std::atomic<int> counter{0};
    m_dir = std::filesystem::temp_directory_path() /
            ("tachyon_sdk_rt_" + std::to_string(currentPidValue()) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(m_dir);
    std::filesystem::create_directories(m_dir);
    std::filesystem::current_path(m_dir);
    for (const char* name : {"TACHYON_DISABLE", "TACHYON_SIDECAR", "TACHYON_RING_KB"}) unsetEnv(name);
    setEnv("TACHYON_AGENT_PIPE", "no-such-agent-for-sdk-tests");
    Tachyon_Internal_Restart();
  }

  void TearDown() override {
    Tachyon_Internal_Restart();
    for (const char* name : {"TACHYON_DISABLE", "TACHYON_SIDECAR", "TACHYON_RING_KB"}) unsetEnv(name);
    setEnv("TACHYON_AGENT_PIPE", "no-such-agent-for-sdk-tests");
    setEnv("TACHYON_SIDECAR", "sdk_tests.tachyon-inst");
    std::filesystem::current_path(m_oldCwd);
    std::error_code ec;
    std::filesystem::remove_all(m_dir, ec);
  }

  // Starts the runtime with the sidecar at <tempdir>/<name>; no agent. Returns the sidecar path.
  std::filesystem::path startSidecar(const std::string& name = "out.tachyon-inst") {
    const std::filesystem::path path = m_dir / name;
    setEnv("TACHYON_SIDECAR", path.string());
    Tachyon_Internal_Restart();
    EXPECT_EQ(Tachyon_Internal_WaitReady(5000), 2);
    return path;
  }

  // Shuts the runtime down (final drain) and parses the file.
  inline inst::test_support::ParsedStream finishAndParse(const std::filesystem::path& path) {
    Tachyon_Internal_Restart();
    return inst::test_support::ParsedStream::parse(readFile(path));
  }

  std::filesystem::path m_dir;
  std::filesystem::path m_oldCwd;
};

}  // namespace tachyon::sdk_testing
