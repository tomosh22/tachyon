#include "core/log.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace tachyon::core {
namespace {

TEST(Log, SinkAndLevelFiltering) {
  std::vector<std::string> lines;
  setLogSink([&](LogLevel, std::string_view m) { lines.emplace_back(m); });
  setLogLevel(LogLevel::Warn);
  TACHYON_LOG_INFO("dropped {}", 1);
  TACHYON_LOG_WARN("kept {}", 2);
  TACHYON_LOG_ERROR("kept {}", 3);
  setLogSink(nullptr);
  setLogLevel(LogLevel::Info);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "kept 2");
}

}  // namespace
}  // namespace tachyon::core
