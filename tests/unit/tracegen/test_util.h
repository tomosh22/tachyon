// Shared helpers for tracegen tests.
#pragma once

#include "generator.h"
#include "scenario.h"
#include "sha256.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace tachyon::tracegen::testing_util {

// Unique scratch directory for the current test, removed on destruction.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string("tachyon_tracegen_") + info->test_suite_name() + "_" + info->name() + "_" +
                       std::to_string(counter++);
    for (char& c : name)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';  // parameterized names: '/', '"'
    m_path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
    std::filesystem::create_directories(m_path, ec);
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

inline std::filesystem::path scenarioDir() {
  return std::filesystem::path(TACHYON_FIXTURE_DIR) / "tracegen";
}

inline nlohmann::json readJson(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return nlohmann::json::parse(ss.str(), nullptr, false);
}

inline std::string sha(const std::filesystem::path& p) {
  auto r = sha256File(p);
  return r.ok() ? *r : std::string("<error: ") + r.error().toString() + ">";
}

// A tiny valid scenario that individual tests tweak through JSON patching.
inline nlohmann::json miniScenario() {
  return nlohmann::json::parse(R"({
    "name": "mini", "seed": 11, "platform": "windows", "clock_frequency_hz": 10000000,
    "sampling_hz": 8000, "start_ts": 500, "duration_ms": 100,
    "modules": [ { "name": "m.exe", "functions": [
        { "name": "main", "rva": "0x1000", "size": 256 },
        { "name": "work", "rva": "0x1100", "size": 256 } ] } ],
    "stacks": [ { "name": "a", "frames": [ "m.exe!main", "m.exe!work+0x10" ] },
                { "name": "b", "frames": [ "m.exe!main" ] },
                { "name": "w", "frames": [ "m.exe!main", "m.exe!work+0x80" ] } ],
    "threads": [ { "tid": 1, "name": "T1", "from_start": true,
        "mix": [ { "stack": "a", "weight": 3 }, { "stack": "b", "weight": 1 } ],
        "intervals": [ { "state": "running", "dur_us": 30010 },
                       { "state": "waiting", "dur_us": 10000, "blocking_stack": "w",
                         "ready_after_us": 9000, "waker_tid": 2, "waker_stack": "b" },
                       { "state": "running" } ] },
      { "tid": 2, "name": "T2", "start_us": 5000,
        "mix": [ { "stack": "b", "weight": 1 } ],
        "intervals": [ { "state": "running" } ] } ]
  })");
}

}  // namespace tachyon::tracegen::testing_util
