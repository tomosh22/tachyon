// `tachyon-cli live` (WP-65a): argument handling and a whole live run in-process against an Agent whose mock
// backend replays a fixture as a live capture.
#include "../control/test_util.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "cli/cli.h"
#include "control/server.h"
#include "trace/trace_writer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sstream>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using control::testing_util::testAddress;

constexpr uint64_t kStart = 1'000'000'000ull;

// One process, one module, one thread and `ms` samples at 1 kHz in two functions (30% / 70%).
fs::path writeFixture(const fs::path& path, uint32_t ms) {
  trace::FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = kStart;
  h.end_ts = kStart + static_cast<uint64_t>(ms) * 1'000'000;
  h.platform = trace::kPlatformLinux;
  auto w = trace::TraceWriter::open(path, h);
  EXPECT_TRUE(w.ok());
  trace::ProcessRec proc{};
  proc.pid = 50;
  proc.start_ts = kStart;
  proc.name = w->internString("livemod");
  w->append(std::span<const trace::ProcessRec>(&proc, 1));
  trace::ModuleRec mod{};
  mod.pid = 50;
  mod.base = 0x400000;
  mod.size = 0x10000;
  mod.path = w->internString("/bin/livemod");
  mod.load_ts = kStart;
  w->append(std::span<const trace::ModuleRec>(&mod, 1));
  trace::ThreadRec th{};
  th.tid = 50;
  th.pid = 50;
  th.start_ts = kStart;
  th.name = w->internString("main");
  th.flags = 1;
  w->append(std::span<const trace::ThreadRec>(&th, 1));
  const FrameIdx hot[] = {w->internFrame(ModuleIdx(0), 0x1000)};
  const FrameIdx cold[] = {w->internFrame(ModuleIdx(0), 0x2000)};
  const CallstackIdx stacks[2] = {w->internCallstack(hot), w->internCallstack(cold)};
  trace::SampleBatch samples;
  for (uint32_t i = 0; i < ms; ++i)
    samples.push(kStart + static_cast<uint64_t>(i) * 1'000'000, 50, stacks[i % 10 < 7 ? 0 : 1], 0);
  w->append(samples);
  w->appendCaptureInfo("{\"sampling_hz\":1000}");
  EXPECT_TRUE(w->finish().ok());
  return path;
}

struct CliResult {
  int code;
  std::string out, err;
};

struct Rig {
  TempDir dir;
  std::string address = testAddress("clilive");
  std::unique_ptr<agent::Agent> core;
  std::unique_ptr<control::Server> server;

  explicit Rig(uint32_t ms = 4000) {
    capture::MockBackendConfig cfg;
    cfg.fixture = writeFixture(dir.path() / "fixture.tachyon", ms);
    cfg.live.speed = 1.0;
    agent::AgentConfig ac;
    ac.backendName = "mock";
    ac.agentAddress = address;
    core = std::make_unique<agent::Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto l = control::listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<control::Server>(std::move(*l), *core);
    core->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
  }
  ~Rig() {
    server->stop();
    core->shutdown();
  }
  CliResult cli(std::vector<std::string> args) {
    args.insert(args.begin(), {"--socket", address});
    std::ostringstream out, err;
    const int code = run(args, out, err);
    return {code, out.str(), err.str()};
  }
};

TEST(CliLive, UsageErrorsAreRejectedBeforeConnecting) {
  const std::vector<std::vector<std::string>> bad = {
      {"live"},                                        // no target
      {"live", "--system"},                            // nothing ends a system capture
      {"live", "--attach", "5"},                       // ... nor an attach
      {"live", "--launch", "a.exe", "--system"},       // two targets
      {"live", "--launch"},                            // missing value
      {"live", "--launch", "a.exe", "--window", "0"},  // not a positive number
      {"live", "--launch", "a.exe", "--window", "abc"},
      {"live", "--launch", "a.exe", "--bogus"},
  };
  for (const auto& args : bad) {
    std::ostringstream out, err;
    EXPECT_EQ(run(args, out, err), kExitUsage) << args.back() << "\n" << err.str();
  }
}

TEST(CliLive, NoAgentIsAConnectionFailure) {
  std::ostringstream out, err;
  EXPECT_EQ(run({"--socket", testAddress("clilive-none"), "live", "--system", "--duration", "1"}, out, err),
            kExitConnectionFailure);
}

TEST(CliLive, PrintsASummaryLineEachIntervalAndTheFinalTrace) {
  Rig rig;
  const CliResult r = rig.cli({"live", "--system", "--duration", "1.5", "--interval", "0.4", "--window", "10",
                               "--out", (rig.dir.path() / "live.tachyon").string()});
  ASSERT_EQ(r.code, 0) << r.out << r.err;
  EXPECT_NE(r.out.find("live capture started"), std::string::npos);
  size_t lines = 0;
  std::istringstream in(r.out);
  uint64_t lastSamples = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] != '[') continue;
    ++lines;
    EXPECT_NE(line.find("window "), std::string::npos) << line;
    EXPECT_NE(line.find("gaps 0"), std::string::npos) << line;
    EXPECT_NE(line.find("livemod+0x1000"), std::string::npos) << line;  // module + RVA, 70% of the samples
    const size_t at = line.find(" samples");
    ASSERT_NE(at, std::string::npos);
    const size_t from = line.rfind(' ', at - 1);
    lastSamples = std::strtoull(line.c_str() + from + 1, nullptr, 10);
  }
  EXPECT_GE(lines, 3u);  // 1.5 s / 0.4 s plus the final line
  EXPECT_GT(lastSamples, 500u);
  EXPECT_NE(r.out.find("capture done: " + (rig.dir.path() / "live.tachyon").string()), std::string::npos)
      << r.out;
  EXPECT_TRUE(fs::exists(rig.dir.path() / "live.tachyon"));
}

TEST(CliLive, JsonOutputIsOneObjectPerLine) {
  Rig rig;
  const CliResult r = rig.cli({"--json", "live", "--system", "--duration", "1.2", "--interval", "0.4",
                               "--out", (rig.dir.path() / "j.tachyon").string()});
  ASSERT_EQ(r.code, 0) << r.out << r.err;
  size_t summaries = 0;
  bool done = false;
  std::istringstream in(r.out);
  for (std::string line; std::getline(in, line);) {
    if (line.empty()) continue;
    const auto j = nlohmann::json::parse(line, nullptr, false);
    ASSERT_TRUE(j.is_object()) << line;
    if (j.contains("event") && j["event"] == "capture_done") {
      done = true;
      EXPECT_EQ(j["path"], (rig.dir.path() / "j.tachyon").string());
    } else if (j.contains("samples")) {
      ++summaries;
      EXPECT_TRUE(j.contains("window_s") && j.contains("gaps") && j.contains("dropped") &&
                  j["top"].is_array());
    }
  }
  EXPECT_GE(summaries, 2u);
  EXPECT_TRUE(done);
}

}  // namespace
}  // namespace tachyon::cli
