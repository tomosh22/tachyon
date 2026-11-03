// WP-60 acceptance (Linux, label linux_privileged): the flight recorder end to end. A real tachyon-agent with
// the perf backend, `tachyon-cli capture --launch tachyon-testbed --scenario frames --instrument
// --flush-on-hitch --circular-mb 256` for about 20 s. The testbed asks for a flush right after each planted
// hitch frame; every flushed trace must load in TraceModel, span at most the 10 s window, contain the hitch
// frame (a frame of at least 40 ms between two frame marks) and have balanced scopes.
// Skips itself, with the reason, when the kernel refuses system-wide perf_event_open (the CI privileged step
// fails when anything skips).
#include "../../unit/agent/process_util.h"
#include "capture/linux_perf/linux_perf_backend.h"
#include "control/client.h"
#include "inst/merge.h"
#include "model/model.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;
using agent::testing_util::Process;
using agent::testing_util::readTextFile;
using agent::testing_util::runToCompletion;

class TempDir {
 public:
  TempDir() {
    m_path = fs::temp_directory_path() / ("tachyon-flight-" + std::to_string(::getpid()));
    fs::remove_all(m_path);
    fs::create_directories(m_path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(m_path, ec);
  }
  const fs::path& path() const { return m_path; }

 private:
  fs::path m_path;
};

constexpr double kHitchSeconds = 0.040;  // the planted hitch frame is 57 ms; anything above this is the hitch
constexpr double kWindowSeconds = 10.0;

TEST(FlightRecorderPerf, TestbedHitchTriggersProduceTracesWithTheHitchInTheLastTenSeconds) {
  {
    LinuxPerfBackend probe;
    auto priv = probe.checkPrivileges();
    if (!priv.ok())
      GTEST_SKIP() << "system-wide perf_event_open is not available here: " << priv.error().toString();
  }
  {
    std::ifstream in("/proc/sys/kernel/perf_event_max_sample_rate");
    long long maxRate = 0;
    if (in >> maxRate && maxRate > 0 && maxRate < 8000)
      GTEST_SKIP() << "kernel.perf_event_max_sample_rate is " << maxRate << ", below the default 8000 Hz";
  }

  TempDir dir;
  const fs::path sockDir = dir.path() / "run";
  const fs::path workDir = dir.path() / "work";
  const fs::path capDir = dir.path() / "captures";
  fs::create_directories(sockDir);
  fs::create_directories(workDir);
  fs::create_directories(capDir);
  ::chmod(sockDir.c_str(), 0700);
  const std::string address = (sockDir / "tachyon-agent.sock").string();
  const fs::path out = capDir / "recorder.tachyon";

  Process agent =
      Process::spawn(TACHYON_AGENT_EXE, {"--backend", "perf", "--socket", address}, dir.path() / "agent.log");
  ASSERT_TRUE(agent.valid());
  auto probeClient = control::Client::connectWithRetry(address, 15000);
  ASSERT_TRUE(probeClient.ok()) << readTextFile(dir.path() / "agent.log");

  const int code = runToCompletion(TACHYON_CLI_EXE, {"--socket",      address,
                                                     "--json",        "capture",
                                                     "--launch",      TACHYON_TESTBED_EXE,
                                                     "--cwd",         workDir.string(),
                                                     "--arg",         "--instrument",
                                                     "--arg",         "--flush-on-hitch",
                                                     "--arg",         "--scenario",
                                                     "--arg",         "frames",
                                                     "--arg",         "--duration",
                                                     "--arg",         "20",
                                                     "--arg",         "--hitch-interval-ms",
                                                     "--arg",         "2500",
                                                     "--env",         "TACHYON_AGENT_PIPE=" + address,
                                                     "--circular-mb", "256",
                                                     "--out",         out.string(),
                                                     "--duration",    "90"},
                                   dir.path() / "cli.log", 240000);
  const std::string cliLog = readTextFile(dir.path() / "cli.log");
  const std::string agentLog = readTextFile(dir.path() / "agent.log");
  ASSERT_EQ(code, 0) << cliLog << "\n--- agent ---\n" << agentLog;

  // The flushed events the CLI printed while it waited.
  std::vector<nlohmann::json> flushed;
  nlohmann::json done;
  std::istringstream lines(cliLog);
  for (std::string line; std::getline(lines, line);) {
    if (line.empty() || line[0] != '{') continue;
    nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
    if (!j.is_object() || !j.contains("event")) continue;
    if (j["event"] == "flushed") flushed.push_back(j);
    if (j["event"] == "capture_done") done = j;
  }
  agent.requestTerminate();
  int agentCode = -1;
  if (!agent.waitFor(15000, &agentCode)) agent.kill();

  ASSERT_GE(flushed.size(), 5u) << cliLog << "\n--- agent ---\n" << agentLog;
  ASSERT_TRUE(done.is_object());
  for (const fs::path& p : {workDir}) {
    for (const auto& entry : fs::recursive_directory_iterator(p))
      EXPECT_NE(entry.path().extension(), ".tachyon-inst") << "sidecar file " << entry.path();
  }

  std::set<std::string> paths;
  size_t withHitch = 0, fullWindows = 0;
  for (const nlohmann::json& event : flushed) {
    const fs::path path = event["path"].get<std::string>();
    SCOPED_TRACE(path.string());
    EXPECT_EQ(path.parent_path(), capDir);
    EXPECT_EQ(path.filename().string().rfind("tachyon-testbed-hitch-", 0), 0u)
        << "<process>-<reason>-<timestamp>";
    EXPECT_TRUE(paths.insert(path.string()).second) << "every flush is its own file";
    ASSERT_TRUE(fs::exists(path));

    auto loaded = model::TraceModel::load(path);
    ASSERT_TRUE(loaded.ok()) << loaded.error().toString();

    auto opened = trace::TraceReader::open(path);
    ASSERT_TRUE(opened.ok());
    trace::TraceReader& reader = *opened;
    const trace::FileHeader header = reader.header();
    const double hz = static_cast<double>(header.clock_frequency_hz);
    const double span = static_cast<double>(header.end_ts - header.start_ts) / hz;
    EXPECT_LE(span, kWindowSeconds + 0.75) << "a flush covers the last 10 s";
    EXPECT_GT(span, 0.5);
    EXPECT_NE(header.flags & trace::kFlagHasInstrumentation, 0u);
    const std::string info = reader.readCaptureInfo().value();
    EXPECT_NE(info.find("\"flush_reason\":\"hitch\""), std::string::npos) << info;

    // Sampled data in the window.
    const trace::SampleBatch samples = reader.readSamples().value();
    EXPECT_GT(samples.size(), 100u);
    // Samples of the target's threads: once the capture is older than the window, the samples must cover the
    // window too (the backend keeps the segments of the last `seconds` between flushes).
    std::set<uint32_t> targetTids;
    const auto threads = reader.readThreads().value();  // a local: the range must outlive the Result
    for (const auto& rec : threads) {
      const trace::ThreadRec t = rec;
      if ((t.flags & trace::kThreadFlagTargetProcess) != 0) targetTids.insert(t.tid);
    }
    uint64_t firstSample = ~0ull, lastSample = 0, targetSamples = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
      if (targetTids.count(samples.tid[i]) == 0) continue;
      ++targetSamples;
      firstSample = std::min(firstSample, samples.ts[i]);
      lastSample = std::max(lastSample, samples.ts[i]);
    }
    const double sampleSpan = targetSamples > 1 ? static_cast<double>(lastSample - firstSample) / hz : 0.0;
    if (span >= kWindowSeconds - 0.01) {
      EXPECT_GE(sampleSpan, 0.9 * span) << "target samples cover the whole window, not just the time since "
                                           "the previous flush";
      EXPECT_GE(firstSample, header.start_ts);
      EXPECT_LE(lastSample, header.end_ts);
      ++fullWindows;
    }

    // Scopes: balanced, inside the window.
    const auto scopes = reader.readScopeEvents().value();
    ASSERT_GT(scopes.size(), 100u);
    const inst::ScopeBalance balance = inst::checkScopeBalance(scopes);
    EXPECT_EQ(balance.unmatchedEnds, 0u);
    EXPECT_EQ(balance.unclosedBegins, 0u);
    EXPECT_EQ(balance.outOfOrder, 0u);
    for (const auto& rec : scopes) {
      const trace::ScopeEventRec r = rec;
      EXPECT_GE(r.ts, header.start_ts);
      EXPECT_LE(r.ts, header.end_ts);
    }

    // The hitch: a frame of at least 40 ms between two frame marks, and the testbed's own marker.
    const trace::StringTable strings = reader.readStrings().value();
    std::vector<uint64_t> marks;
    const auto frameMarks = reader.readFrameMarks().value();
    for (const auto& rec : frameMarks) {
      const trace::FrameMarkRec m = rec;
      if (strings.get(m.name) == "testbed.frame") marks.push_back(m.ts);
      EXPECT_GE(m.ts, header.start_ts);
      EXPECT_LE(m.ts, header.end_ts);
    }
    std::sort(marks.begin(), marks.end());
    ASSERT_GT(marks.size(), 100u) << "about 60 frames per second for the window";
    double longest = 0.0;
    for (size_t i = 1; i < marks.size(); ++i)
      longest = std::max(longest, static_cast<double>(marks[i] - marks[i - 1]) / hz);
    EXPECT_GE(longest, kHitchSeconds) << "the hitch frame is in the window";
    if (longest >= kHitchSeconds) ++withHitch;
    std::printf(
        "[flight] %s: span %.2f s, %zu samples (%llu of the target over %.2f s), %zu scope events, %zu frame "
        "marks, longest frame %.1f ms\n",
        path.filename().string().c_str(), span, samples.size(),
        static_cast<unsigned long long>(targetSamples), sampleSpan, scopes.size(), marks.size(),
        longest * 1e3);
    bool hitchMarker = false;
    const auto markers = reader.readMarkers().value();
    for (const auto& rec : markers)
      hitchMarker = hitchMarker || strings.get(trace::MarkerRec(rec).name) == "testbed.hitch";
    EXPECT_TRUE(hitchMarker) << "the testbed.hitch marker is in the window";
  }
  EXPECT_EQ(withHitch, flushed.size());
  EXPECT_GE(fullWindows, 2u) << "flushes 4..N have a full 10 s window";

  // The capture went on through all of the flushes and ended with its own trace.
  EXPECT_EQ(fs::path(done["path"].get<std::string>()), out);
  auto final = model::TraceModel::load(out);
  ASSERT_TRUE(final.ok()) << final.error().toString();
}

}  // namespace
}  // namespace tachyon::capture
