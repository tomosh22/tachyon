// WP-60 (no privileges, every platform): the SDK trigger path of the flight recorder against a real
// tachyon-agent process with the mock backend. `tachyon-testbed --instrument --flush-on-hitch` calls
// Tachyon_RequestFlush("hitch") after each planted hitch frame; the agent answers at once, flushes in the
// background, names the file `<process>-hitch-<timestamp>` and announces each one with a `flushed` event. The
// mock backend's traces have a fixed made-up window, so what the instrumentation of the flushes contains is
// covered by the privileged test (tests/linux/capture/flight_recorder_test.cpp); this one checks the
// plumbing: SDK request, rate limiting, naming, events, CaptureInfo, and that the capture still ends
// normally.
#include "../../unit/agent/process_util.h"
#include "../../unit/control/test_util.h"
#include "../../unit/inst/trace_builder.h"
#include "control/client.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <set>
#include <sstream>

namespace tachyon {
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
  if (value.empty()) {
    unsetenv(key);
  } else {
    setenv(key, value.c_str(), 1);
  }
#endif
}

TEST(FlightRecorderMock, TestbedHitchTriggersBecomeFlushedTraces) {
  TempDir dir;
  const std::string address = testAddress("flightmock");
  const fs::path fixture = dir.path() / "fixture.tachyon";
  inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
  const fs::path capDir = dir.path() / "captures";
  fs::create_directories(capDir);

  Process agent = Process::spawn(TACHYON_AGENT_EXE,
                                 {"--backend", "mock", "--socket", address, "--mock-fixture",
                                  fixture.string(), "--flush-coalesce-ms", "500", "--flush-seconds", "7"},
                                 dir.path() / "agent.log");
  ASSERT_TRUE(agent.valid());
  auto client = control::Client::connectWithRetry(address, 15000);
  ASSERT_TRUE(client.ok()) << readTextFile(dir.path() / "agent.log");

  control::StartRequest start;
  start.mode = capture::CaptureMode::System;
  start.circular_mb = 64;
  start.out_path = (capDir / "recorder.tachyon").string();
  auto started = client->call(start, 15000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;

  setEnv("TACHYON_AGENT_PIPE", address);
  setEnv("TACHYON_SIDECAR", "off");
  const int code = runToCompletion(TACHYON_TESTBED_EXE,
                                   {"--instrument", "--flush-on-hitch", "--scenario", "frames", "--duration",
                                    "4", "--hitch-interval-ms", "1000"},
                                   dir.path() / "testbed.log", 120000);
  setEnv("TACHYON_AGENT_PIPE", "");
  setEnv("TACHYON_SIDECAR", "");
  ASSERT_EQ(code, 0) << readTextFile(dir.path() / "testbed.log");

  nlohmann::json report;
  {
    std::istringstream lines(readTextFile(dir.path() / "testbed.log"));
    for (std::string line; std::getline(lines, line);)
      if (!line.empty() && line[0] == '{') report = nlohmann::json::parse(line, nullptr, false);
  }
  ASSERT_TRUE(report.is_object());
  const nlohmann::json& details = report["scenarios"][0]["details"];
  const uint64_t requests = details["flush_requests"].get<uint64_t>();
  EXPECT_GE(requests, 2u) << "a 1 s hitch interval over 4 s";
  EXPECT_EQ(details["flushes_accepted"].get<uint64_t>(), requests) << "the agent accepted every trigger";
  EXPECT_EQ(details["hitch_frames"].size(), details["hitches"].get<size_t>());

  // The 1 s hitch period is above the 0.5 s coalescing window, so each trigger is its own flush.
  std::set<std::string> paths;
  for (uint64_t i = 0; i < requests; ++i) {
    auto event = client->waitForEvent(
        [](const control::Event& e) { return std::holds_alternative<control::FlushedEvent>(e); }, 30000);
    ASSERT_TRUE(event.ok()) << "flushed event " << i << ": " << readTextFile(dir.path() / "agent.log");
    const auto& flushed = std::get<control::FlushedEvent>(*event);
    EXPECT_EQ(flushed.reason, "hitch");
    EXPECT_EQ(fs::path(flushed.path).parent_path(), capDir);
    EXPECT_EQ(fs::path(flushed.path).filename().string().rfind("tachyon-testbed-hitch-", 0), 0u)
        << flushed.path;
    EXPECT_TRUE(paths.insert(flushed.path).second);
    ASSERT_TRUE(fs::exists(flushed.path));
    auto reader = trace::TraceReader::open(flushed.path);
    ASSERT_TRUE(reader.ok());
    const std::string info = reader->readCaptureInfo().value();
    EXPECT_NE(info.find("\"flush_reason\":\"hitch\""), std::string::npos) << info;
    EXPECT_NE(info.find("\"flush_window_s\":7"), std::string::npos) << "the agent's --flush-seconds";
  }

  auto stop = client->call(control::StopRequest{}, 15000);
  ASSERT_TRUE(stop.ok());
  EXPECT_TRUE(stop->ok) << stop->error;
  auto done = client->waitForEvent(
      [](const control::Event& e) { return std::holds_alternative<control::CaptureDoneEvent>(e); }, 60000);
  ASSERT_TRUE(done.ok());
  agent.requestTerminate();
  int agentCode = -1;
  if (!agent.waitFor(15000, &agentCode)) agent.kill();
}

}  // namespace
}  // namespace tachyon
