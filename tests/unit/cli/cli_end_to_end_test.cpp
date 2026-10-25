// `tachyon-cli capture` end to end against the real `tachyon-agent --backend mock` (WP-13): two separate
// processes talking over the real transport, on Linux and Windows alike.
#include "../agent/fixture_util.h"
#include "../agent/process_util.h"
#include "../control/test_util.h"
#include "control/client.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <sstream>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;
using agent::testing_util::Process;
using agent::testing_util::readTextFile;
using agent::testing_util::runToCompletion;
using control::testing_util::TempDir;
using control::testing_util::testAddress;

void setEnv(const char* key, const std::string& value) {
#if TACHYON_PLATFORM_WINDOWS
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

class CaptureEndToEnd : public ::testing::Test {
 protected:
  void SetUp() override {
    m_fixture = agent::testing_util::writeFixtureTrace(m_dir.path() / "fixture.tachyon");
    setEnv("TACHYON_MOCK_FIXTURE", m_fixture.string());
    m_address = testAddress("e2e");
    m_agent = Process::spawn(TACHYON_AGENT_EXE, {"--backend", "mock", "--socket", m_address},
                             m_dir.path() / "agent.log");
    ASSERT_TRUE(m_agent.valid());
    auto c = control::Client::connectWithRetry(m_address, 15000);
    ASSERT_TRUE(c.ok()) << readTextFile(m_dir.path() / "agent.log");
  }
  void TearDown() override {
    m_agent.requestTerminate();
    int code = 0;
    if (!m_agent.waitFor(10000, &code)) m_agent.kill();
    setEnv("TACHYON_MOCK_FIXTURE", "");
  }

  // Runs tachyon-cli with the agent's address; returns the exit code, stdout in `out`.
  int cli(const std::vector<std::string>& args, std::string* out = nullptr, int timeoutMs = 60000) {
    std::vector<std::string> full = {"--socket", m_address};
    full.insert(full.end(), args.begin(), args.end());
    const fs::path file = m_dir.path() / ("cli-" + std::to_string(m_runs++) + ".txt");
    const int code = runToCompletion(TACHYON_CLI_EXE, full, file, timeoutMs);
    if (out != nullptr) *out = readTextFile(file);
    return code;
  }

  TempDir m_dir;
  fs::path m_fixture;
  std::string m_address;
  Process m_agent;
  int m_runs = 0;
};

TEST_F(CaptureEndToEnd, CaptureLaunchWithDurationWritesTheTrace) {
  const fs::path out = m_dir.path() / "spin.tachyon";
  std::string text;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(cli({"capture", "--launch", "tachyon-testbed", "--args", "--scenario spin --duration 3", "--out",
                 out.string(), "--duration", "1"},
                &text),
            0)
      << text << readTextFile(m_dir.path() / "agent.log");
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_GE(elapsed, 0.9) << "waited for --duration";
  EXPECT_NE(text.find("capture_done: path="), std::string::npos) << text;
  ASSERT_TRUE(fs::exists(out));
  EXPECT_TRUE(trace::TraceReader::open(out).ok());
  EXPECT_EQ(readTextFile(out), readTextFile(m_fixture)) << "the mock copies its fixture";
  EXPECT_EQ(cli({"status"}, &text), 0);
  EXPECT_NE(text.find("state: idle"), std::string::npos) << text;
}

TEST_F(CaptureEndToEnd, JsonOutputIsMachineReadable) {
  const fs::path out = m_dir.path() / "json.tachyon";
  std::string text;
  ASSERT_EQ(cli({"--json", "capture", "--system", "--duration", "0.5", "--out", out.string()}, &text), 0)
      << text;
  std::istringstream lines(text);
  std::string line;
  bool sawDone = false;
  while (std::getline(lines, line)) {
    ASSERT_FALSE(line.empty());
    const auto j = nlohmann::json::parse(line);
    if (j.contains("event") && j["event"] == "capture_done") {
      sawDone = true;
      EXPECT_EQ(j["path"], out.string());
      EXPECT_EQ(j["events_lost"], 0);
      EXPECT_GE(j["duration_s"].get<double>(), 0.0);
    }
  }
  EXPECT_TRUE(sawDone) << text;
}

TEST_F(CaptureEndToEnd, StartStopWaitAndExitCodes) {
  const fs::path out = m_dir.path() / "ss.tachyon";
  std::string text;
  EXPECT_EQ(cli({"stop"}, &text), 1) << "no capture is running";
  ASSERT_EQ(cli({"start", "--attach", "1", "--out", out.string()}, &text), 0) << text;
  EXPECT_EQ(cli({"start", "--system", "--out", out.string()}, &text), 1) << "EBUSY while capturing";
  EXPECT_EQ(cli({"stop", "--wait"}, &text), 0) << text;
  EXPECT_NE(text.find("capture_done"), std::string::npos);
  EXPECT_TRUE(fs::exists(out));
}

TEST_F(CaptureEndToEnd, UsageErrorsExitThree) {
  EXPECT_EQ(cli({"capture"}), 3);
  EXPECT_EQ(cli({"capture", "--system"}), 3);
  EXPECT_EQ(cli({"bogus"}), 3);
}

}  // namespace
}  // namespace tachyon::cli
