// Runs the real binary end to end: `tachyon-testbed --scenario all --duration 5`.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "testbed/report.h"

#ifdef _WIN32
#define TB_POPEN _popen
#define TB_PCLOSE _pclose
#else
#define TB_POPEN popen
#define TB_PCLOSE pclose
#endif

TEST(TestbedRunAll, AllScenariosForFiveSecondsExitCleanlyWithValidJson) {
  const std::string command = std::string("\"") + TACHYON_TESTBED_EXE + "\" --scenario all --duration 5";
  const auto start = std::chrono::steady_clock::now();
  FILE* pipe = TB_POPEN(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[4096];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, got);
  const int status = TB_PCLOSE(pipe);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  EXPECT_EQ(status, 0);
  EXPECT_LT(seconds, 10.0);
  const nlohmann::json j = nlohmann::json::parse(output, nullptr, /*allow_exceptions=*/false);
  ASSERT_FALSE(j.is_discarded()) << "stdout is not JSON: " << output.substr(0, 200);
  const auto valid = tachyon::testbed::validateReportJson(j);
  EXPECT_TRUE(valid.ok()) << (valid.ok() ? "" : valid.error().toString());
  EXPECT_EQ(j["scenarios"].size(), 9u);
}
