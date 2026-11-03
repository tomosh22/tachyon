// Windows-only end-to-end test of instrumentation ingestion (WP-52) with REAL ETW: a `tachyon-agent --backend
// etw` process captures `tachyon-testbed --instrument`, which streams Tachyon scopes to the agent over the
// per-process named pipe while the kernel session samples it. The finished trace must hold the sampling data
// and the instrumentation (balanced, ordered scopes on threads that exist in the trace) in one file, in one
// clock domain (QPC). Needs an elevated process (the CI runner is); skips with a message otherwise.
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#include "../../unit/agent/process_util.h"
#include "capture/windows_etw/etw_session.h"
#include "control/client.h"
#include "inst/merge.h"
#include "trace/trace_reader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tachyon::inst {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using agent::testing_util::Process;
using agent::testing_util::readTextFile;
using agent::testing_util::runToCompletion;

std::mutex g_summaryMutex;

// Thread-safe. ctest hides the output of passing tests, so key numbers go to stderr and a summary file.
void note(const std::string& line) {
  std::lock_guard lock(g_summaryMutex);
  std::fprintf(stderr, "[inst-windows] %s\n", line.c_str());
  std::ofstream f(fs::path(TACHYON_BINARY_DIR) / "inst_windows_summary.txt", std::ios::app);
  f << line << '\n';
}

class InstWindowsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto priv = capture::etw::EtwSession::enableProfilePrivilege();
    if (!priv.ok()) GTEST_SKIP() << "no SeSystemProfilePrivilege (not elevated): " << priv.error().message;
    m_dir = fs::path(TACHYON_BINARY_DIR) / "inst_windows_tmp";
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    fs::create_directories(m_dir);
    m_address = "\\\\.\\pipe\\tachyon-insttest-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(static_cast<unsigned>(Clock::now().time_since_epoch().count() & 0xFFFFFF));
    m_agent =
        Process::spawn(TACHYON_AGENT_EXE, {"--backend", "etw", "--socket", m_address}, m_dir / "agent.log");
    ASSERT_TRUE(m_agent.valid());
    auto c = control::Client::connectWithRetry(m_address, 20000);
    ASSERT_TRUE(c.ok()) << "agent did not start:\n" << readTextFile(m_dir / "agent.log");
  }

  void TearDown() override {
    if (m_agent.valid()) {
      m_agent.requestTerminate();
      int code = 0;
      if (!m_agent.waitFor(15000, &code)) m_agent.kill();
    }
    std::error_code ec;
    if (!m_dir.empty()) fs::remove_all(m_dir, ec);  // ETL files are large; the numbers live in the summary
  }

  // Runs tachyon-cli against the agent; returns the exit code, stdout+stderr in `out`.
  int cli(std::vector<std::string> args, std::string* out, int timeoutMs = 240000) {
    args.insert(args.begin(), {"--socket", m_address});
    const fs::path file = m_dir / ("cli-" + std::to_string(m_runs++) + ".txt");
    const int code = runToCompletion(TACHYON_CLI_EXE, args, file, timeoutMs);
    if (out != nullptr) *out = readTextFile(file);
    return code;
  }

  fs::path m_dir;
  std::string m_address;
  Process m_agent;
  int m_runs = 0;
};

TEST_F(InstWindowsTest, LaunchedInstrumentedTestbedYieldsScopesAndSamplesInOneTrace) {
  const fs::path out = m_dir / "inst.tachyon";
  std::string text;
  const int code = cli({"--json", "capture", "--launch", TACHYON_TESTBED_EXE, "--args",
                        "--instrument --scenario spin,lock,jobs,frames --duration 4", "--env",
                        "TACHYON_SIDECAR=off", "--out", out.string()},
                       &text);
  ASSERT_EQ(code, 0) << "cli output:\n" << text << "\nagent log:\n" << readTextFile(m_dir / "agent.log");

  auto opened = trace::TraceReader::open(out);
  ASSERT_TRUE(opened.ok()) << opened.error().toString();
  trace::TraceReader& reader = *opened;
  EXPECT_FALSE(reader.isPartial());
  EXPECT_NE(reader.header().flags & trace::kFlagHasInstrumentation, 0u);
  EXPECT_EQ(reader.header().platform, trace::kPlatformWindows);

  const trace::StringTable strings = reader.readStrings().value();
  const auto threads = reader.readThreads().value();
  const trace::SampleBatch samples = reader.readSamples().value();
  const auto scopes = reader.readScopeEvents().value();
  const auto threadNames = reader.readThreadNames().value();

  // Which threads belong to the launched process in the sampling data.
  std::set<uint32_t> targetTids;
  for (const trace::ThreadRec& rec : threads) {
    const trace::ThreadRec t = rec;
    const uint32_t tid = t.tid;
    if ((t.flags & trace::kThreadFlagTargetProcess) != 0) targetTids.insert(tid);
  }
  ASSERT_FALSE(targetTids.empty());

  uint64_t targetSamples = 0, minSample = ~0ull, maxSample = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    if (targetTids.count(samples.tid[i]) == 0) continue;
    ++targetSamples;
    minSample = std::min(minSample, samples.ts[i]);
    maxSample = std::max(maxSample, samples.ts[i]);
  }
  EXPECT_GT(targetSamples, 1000u);
  ASSERT_GT(scopes.size(), 1000u);

  // Balanced, ordered, and nested scopes per thread.
  const ScopeBalance balance = checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);

  // The SDK's thread ids are the same OS ids ETW reports, and its clock is the trace clock: every scope is on
  // a thread of the target process and lies inside the sampled time span (with some slack for process
  // start-up).
  const double slack = static_cast<double>(reader.header().clock_frequency_hz) * 3.0;
  std::set<uint32_t> scopeTids;
  std::set<std::string> names;
  uint64_t minScope = ~0ull, maxScope = 0;
  for (const auto& rec : scopes) {
    const trace::ScopeEventRec r = rec;
    const uint32_t tid = r.tid;
    const uint64_t ts = r.ts;
    scopeTids.insert(tid);
    minScope = std::min(minScope, ts);
    maxScope = std::max(maxScope, ts);
    if (r.kind == trace::kScopeBegin) names.insert(std::string(strings.get(r.id)));
  }
  for (uint32_t tid : scopeTids) EXPECT_TRUE(targetTids.count(tid) != 0) << "scope on unknown thread " << tid;
  EXPECT_GE(static_cast<double>(minScope), static_cast<double>(minSample) - slack);
  EXPECT_LE(static_cast<double>(maxScope), static_cast<double>(maxSample) + slack);
  for (const char* expected : {"hot_a", "lock_critical_section", "job_execute", "frame_update"})
    EXPECT_TRUE(names.count(expected) != 0) << "missing scope " << expected;
  EXPECT_FALSE(threadNames.empty());
  EXPECT_FALSE(reader.readFrameMarks().value().empty());

  const auto info = nlohmann::json::parse(reader.readCaptureInfo().value(), nullptr, false);
  ASSERT_FALSE(info.is_discarded());
  EXPECT_TRUE(info.contains("instrumentation"));
  EXPECT_TRUE(info.contains("target_pids"));  // the sampling side's CaptureInfo survived the rewrite

  note("samples of the target " + std::to_string(targetSamples) + ", scope events " +
       std::to_string(scopes.size()) + " on " + std::to_string(scopeTids.size()) + " threads, begins " +
       std::to_string(balance.begins) + ", unmatched ends " + std::to_string(balance.unmatchedEnds) +
       ", unclosed " + std::to_string(balance.unclosedBegins) + "; scope span " +
       std::to_string(static_cast<double>(maxScope - minScope) /
                      static_cast<double>(reader.header().clock_frequency_hz)) +
       " s, sample span " +
       std::to_string(static_cast<double>(maxSample - minSample) /
                      static_cast<double>(reader.header().clock_frequency_hz)) +
       " s");
  RecordProperty("scope_events", static_cast<int>(scopes.size()));
  RecordProperty("target_samples", static_cast<int>(targetSamples));
}

// WP-60: the flight recorder with real ETW. The testbed's frames scenario asks for a flush right after each
// planted hitch frame (Tachyon_RequestFlush("hitch")); the agent flushes the circular ETW session in the
// background. Every flushed trace must load, carry instrumentation and frame marks and cover no more than the
// window; at least one must hold the hitch frame (a frame of 40 ms or more between two frame marks). An ETW
// flush restarts the session (a short gap), so which hitch lands in which trace depends on timing.
TEST_F(InstWindowsTest, HitchTriggersFlushTracesThatHoldTheHitchFrame) {
  const fs::path out = m_dir / "recorder.tachyon";
  std::string text;
  const int code = cli({"--json",        "capture",
                        "--launch",      TACHYON_TESTBED_EXE,
                        "--arg",         "--instrument",
                        "--arg",         "--flush-on-hitch",
                        "--arg",         "--scenario",
                        "--arg",         "frames",
                        "--arg",         "--duration",
                        "--arg",         "12",
                        "--arg",         "--hitch-interval-ms",
                        "--arg",         "2500",
                        "--env",         "TACHYON_SIDECAR=off",
                        "--circular-mb", "64",
                        "--out",         out.string()},
                       &text);
  ASSERT_EQ(code, 0) << "cli output:\n" << text << "\nagent log:\n" << readTextFile(m_dir / "agent.log");

  std::vector<std::string> flushedPaths;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (line.empty() || line[0] != '{') continue;
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_object() && j.value("event", std::string()) == "flushed")
      flushedPaths.push_back(j["path"].get<std::string>());
  }
  ASSERT_GE(flushedPaths.size(), 2u) << "cli output:\n"
                                     << text << "\nagent log:\n"
                                     << readTextFile(m_dir / "agent.log");

  size_t withHitch = 0;
  for (const std::string& path : flushedPaths) {
    SCOPED_TRACE(path);
    ASSERT_TRUE(fs::exists(path));
    EXPECT_NE(fs::path(path).filename().string().find("-hitch-"), std::string::npos);
    auto opened = trace::TraceReader::open(path);
    ASSERT_TRUE(opened.ok()) << opened.error().toString();
    trace::TraceReader& reader = *opened;
    const double hz = static_cast<double>(reader.header().clock_frequency_hz);
    EXPECT_LE(static_cast<double>(reader.header().end_ts - reader.header().start_ts) / hz, 10.75);
    EXPECT_NE(reader.header().flags & trace::kFlagHasInstrumentation, 0u);
    const trace::StringTable strings = reader.readStrings().value();
    std::vector<uint64_t> marks;
    const auto frameMarks = reader.readFrameMarks().value();
    for (const auto& rec : frameMarks) {
      const trace::FrameMarkRec m = rec;
      if (strings.get(m.name) == "testbed.frame") marks.push_back(m.ts);
    }
    std::sort(marks.begin(), marks.end());
    EXPECT_GT(marks.size(), 10u);
    double longest = 0.0;
    for (size_t i = 1; i < marks.size(); ++i)
      longest = std::max(longest, static_cast<double>(marks[i] - marks[i - 1]) / hz);
    if (longest >= 0.040) ++withHitch;
    const auto scopes = reader.readScopeEvents().value();
    const ScopeBalance balance = checkScopeBalance(scopes);
    EXPECT_EQ(balance.unmatchedEnds, 0u);
    EXPECT_EQ(balance.unclosedBegins, 0u);
    note("flushed " + fs::path(path).filename().string() + ": " + std::to_string(marks.size()) +
         " frame marks, longest frame " + std::to_string(longest * 1e3) + " ms, " +
         std::to_string(scopes.size()) + " scope events");
  }
  EXPECT_GE(withHitch, 1u) << "no flushed trace contains the hitch frame";
}

}  // namespace
}  // namespace tachyon::inst

#endif  // TACHYON_PLATFORM_WINDOWS
