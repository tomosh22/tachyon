// Shared helpers for trace unit tests.
#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace tachyon::trace::testing_util {

// Unique temp file path for the current test; removed by the returned guard's destructor.
class TempFile {
 public:
  explicit TempFile(const std::string& suffix = ".tachyon") {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    m_path = std::filesystem::temp_directory_path() /
             (std::string("tachyon_trace_") + info->test_suite_name() + "_" + info->name() + suffix);
    std::error_code ec;
    std::filesystem::remove(m_path, ec);
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(m_path, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  const std::filesystem::path& path() const { return m_path; }

 private:
  std::filesystem::path m_path;
};

}  // namespace tachyon::trace::testing_util
