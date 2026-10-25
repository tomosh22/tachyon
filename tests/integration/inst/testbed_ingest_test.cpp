// WP-52 integration (Linux with the mock backend; the same flow runs against real ETW in tests/windows/inst):
// `tachyon-testbed --instrument` streams scopes to an in-process agent or a real tachyon-agent process, and
// the finished trace holds them with balanced, ordered scopes per thread.
#include "../../unit/agent/process_util.h"
#include "../../unit/control/test_util.h"
#include "../../unit/inst/packed.h"
#include "../../unit/inst/trace_builder.h"
#include "agent/agent.h"
#include "capture/mock/mock_backend.h"
#include "cli/cli.h"
#include "control/client.h"
#include "inst/merge.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <map>
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
using inst::testing_util::pv;
using namespace trace;

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

// Sets environment variables for the children spawned inside its lifetime.
class ScopedEnv {
 public:
  ScopedEnv(std::map<std::string, std::string> vars) : m_vars(std::move(vars)) {
    for (const auto& [k, v] : m_vars) setEnv(k.c_str(), v);
  }
  ~ScopedEnv() {
    for (const auto& kv : m_vars) setEnv(kv.first.c_str(), "");
  }

 private:
  std::map<std::string, std::string> m_vars;
};

constexpr const char* kScenarios = "spin,lock,jobs,frames,fibers";

// Runs the instrumented testbed to completion; returns its stdout report (the single JSON line).
nlohmann::json runTestbed(const fs::path& log, const std::string& duration = "3") {
  const int code = runToCompletion(
      TACHYON_TESTBED_EXE, {"--instrument", "--scenario", kScenarios, "--duration", duration}, log, 120000);
  EXPECT_EQ(code, 0) << readTextFile(log);
  std::istringstream lines(readTextFile(log));
  std::string line;
  nlohmann::json report;
  while (std::getline(lines, line)) {
    if (!line.empty() && line[0] == '{') report = nlohmann::json::parse(line, nullptr, false);
  }
  EXPECT_FALSE(report.is_discarded());
  EXPECT_TRUE(report.is_object()) << readTextFile(log);
  return report;
}

// Everything the instrumented testbed run must have left in the trace.
void expectTestbedInstrumentation(const fs::path& tracePath, const nlohmann::json& report) {
  auto opened = TraceReader::open(tracePath);
  ASSERT_TRUE(opened.ok()) << opened.error().toString();
  TraceReader& reader = *opened;
  EXPECT_FALSE(reader.isPartial());
  EXPECT_NE(reader.header().flags & kFlagHasInstrumentation, 0u);

  const StringTable strings = reader.readStrings().value();
  const auto scopes = reader.readScopeEvents().value();
  ASSERT_GT(scopes.size(), 1000u);

  // Per-thread Begin/End balance and order (the SDK guarantees nesting; the process exited cleanly).
  const inst::ScopeBalance balance = inst::checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);
  EXPECT_EQ(balance.begins, balance.ends);
  uint64_t last = 0;
  for (const auto& rec : scopes) {
    const ScopeEventRec r = rec;
    const uint64_t ts = r.ts;
    EXPECT_GE(ts, last);  // globally sorted, which keeps each thread's order
    last = ts;
  }

  std::set<std::string> scopeNames;
  std::set<uint32_t> scopeTids;
  size_t lockWaits = 0;
  for (const auto& rec : scopes) {
    const ScopeEventRec r = rec;
    scopeTids.insert(r.tid);
    if (r.kind != kScopeBegin) continue;
    scopeNames.insert(std::string(strings.get(r.id)));
    if (r.color_rgba == 0x4C4F43FFu) {
      ++lockWaits;
      EXPECT_EQ(strings.get(r.id), "testbed.lock");
      EXPECT_EQ(strings.get(r.data).substr(0, 2), "0x");
    }
  }
  for (const char* expected :
       {"spin_cycle", "hot_a", "hot_b", "hot_c", "spin_rest", "lock_critical_section", "job_wait",
        "job_execute", "frame", "frame_update", "frame_render", "frame_wait", "fiber_slice"})
    EXPECT_TRUE(scopeNames.count(expected) != 0) << "missing scope " << expected;
  EXPECT_GT(lockWaits, 0u);

  // Thread names carry the OS thread ids the testbed reported itself, and every scope thread has one.
  std::map<uint32_t, std::string> names;
  const auto nameRecs = reader.readThreadNames().value();
  for (const auto& rec : nameRecs) {
    const ThreadNameRec r = rec;
    names[r.tid] = std::string(strings.get(r.name));
  }
  std::set<uint32_t> reportTids;
  for (const auto& scenario : report["scenarios"]) {
    for (const auto& thread : scenario["threads"]) {
      const uint32_t tid = thread["tid"].get<uint32_t>();
      reportTids.insert(tid);
      ASSERT_TRUE(names.count(tid) != 0) << "no ThreadName for tid " << tid;
      EXPECT_EQ(names[tid], thread["name"].get<std::string>());
    }
  }
  for (uint32_t tid : scopeTids) EXPECT_TRUE(names.count(tid) != 0) << "scope thread without a name: " << tid;
  for (uint32_t tid : scopeTids)
    EXPECT_TRUE(reportTids.count(tid) != 0 || tid == static_cast<uint32_t>(report["pid"].get<uint64_t>()))
        << "scope on a thread the testbed does not know: " << tid;

  // Frame marks, plot, markers, fibers.
  const auto marks = reader.readFrameMarks().value();
  ASSERT_GT(marks.size(), 20u);
  uint64_t previous = 0;
  bool first = true;
  for (const auto& rec : marks) {
    const FrameMarkRec m = rec;
    EXPECT_EQ(strings.get(m.name), "testbed.frame");
    if (!first) EXPECT_EQ(pv(m.frame_number), previous + 1);
    previous = m.frame_number;
    first = false;
  }
  const auto plots = reader.readPlotSamples().value();
  ASSERT_FALSE(plots.empty());
  const PlotSampleRec plot = plots[0];
  EXPECT_EQ(strings.get(plot.name), "testbed.frame_ms");
  EXPECT_GT(pv(plot.value), 0.0);

  std::map<std::string, int> markerCounts;
  const auto markerRecs = reader.readMarkers().value();
  for (const auto& rec : markerRecs) {
    const MarkerRec m = rec;
    ++markerCounts[std::string(strings.get(m.name))];
  }
  EXPECT_EQ(markerCounts["testbed.scenario.begin"], 5);
  EXPECT_EQ(markerCounts["testbed.scenario.end"], 5);

  const auto fibers = reader.readFiberEvents().value();
  std::map<uint8_t, int> kinds;
  for (const auto& rec : fibers) {
    const FiberEventRec f = rec;
    ++kinds[f.kind];
  }
  EXPECT_EQ(kinds[kFiberRegister], 5);
  EXPECT_EQ(kinds[kFiberUnregister], 5);
  EXPECT_GT(kinds[kFiberSwitchBegin], 10);
  EXPECT_GT(kinds[kFiberSwitchEnd], 10);

  // The sampling side of the (mock) capture is intact.
  EXPECT_EQ(reader.readSamples().value().size(), 50u);
}

struct InProcessAgent {
  TempDir dir;
  fs::path fixture;
  std::string address = testAddress("tbagent");
  std::unique_ptr<agent::Agent> agentCore;
  std::unique_ptr<control::Server> server;

  InProcessAgent() {
    fixture = dir.path() / "fixture.tachyon";
    inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
    capture::MockBackendConfig cfg;
    cfg.fixture = fixture;
    agent::AgentConfig ac;
    ac.backendName = "mock";
    ac.agentAddress = address;
    agentCore = std::make_unique<agent::Agent>(std::make_unique<capture::MockBackend>(cfg), ac);
    auto listener = control::listenOn(address);
    EXPECT_TRUE(listener.ok());
    server = std::make_unique<control::Server>(std::move(*listener), *agentCore);
    agentCore->setEventSink([this](const control::Event& e) { server->broadcast(e); });
    server->start();
  }
  ~InProcessAgent() {
    server->stop();
    agentCore->shutdown();
  }
};

TEST(InstIntegration, InstrumentedTestbedAgainstAnInProcessAgentWithTheMockBackend) {
  InProcessAgent a;
  auto client = control::Client::connectWithRetry(a.address, 5000);
  ASSERT_TRUE(client.ok());
  control::StartRequest start;
  start.mode = capture::CaptureMode::System;
  start.out_path = (a.dir.path() / "tb.tachyon").string();
  auto started = client->call(start, 10000);
  ASSERT_TRUE(started.ok());
  ASSERT_TRUE(started->ok) << started->error;

  nlohmann::json report;
  {
    ScopedEnv env({{"TACHYON_AGENT_PIPE", a.address}, {"TACHYON_SIDECAR", "off"}});
    report = runTestbed(a.dir.path() / "testbed.log");
  }
  auto stop = client->call(control::StopRequest{}, 10000);
  ASSERT_TRUE(stop.ok());
  ASSERT_TRUE(stop->ok) << stop->error;
  auto done = client->waitForEvent(
      [](const control::Event& e) { return std::holds_alternative<control::CaptureDoneEvent>(e); }, 60000);
  ASSERT_TRUE(done.ok());
  const std::string path = std::get<control::CaptureDoneEvent>(*done).summary.path;
  ASSERT_EQ(fs::path(path), a.dir.path() / "tb.tachyon");
  expectTestbedInstrumentation(path, report);
  EXPECT_FALSE(fs::exists(a.dir.path() / "tb.tachyon.inst"));
}

TEST(InstIntegration, RealAgentProcessAndCliWithTheMockBackend) {
  TempDir dir;
  const fs::path fixture = dir.path() / "fixture.tachyon";
  inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
  const std::string address = testAddress("tbproc");
  Process agentProc = Process::spawn(
      TACHYON_AGENT_EXE, {"--backend", "mock", "--socket", address, "--mock-fixture", fixture.string()},
      dir.path() / "agent.log");
  ASSERT_TRUE(agentProc.valid());
  auto c = control::Client::connectWithRetry(address, 15000);
  ASSERT_TRUE(c.ok()) << readTextFile(dir.path() / "agent.log");

  const fs::path out = dir.path() / "cli.tachyon";
  ASSERT_EQ(
      runToCompletion(TACHYON_CLI_EXE, {"--socket", address, "start", "--system", "--out", out.string()},
                      dir.path() / "start.log"),
      0)
      << readTextFile(dir.path() / "start.log");
  nlohmann::json report;
  {
    ScopedEnv env({{"TACHYON_AGENT_PIPE", address}, {"TACHYON_SIDECAR", "off"}});
    report = runTestbed(dir.path() / "testbed.log");
  }
  ASSERT_EQ(
      runToCompletion(TACHYON_CLI_EXE, {"--socket", address, "stop", "--wait"}, dir.path() / "stop.log"), 0)
      << readTextFile(dir.path() / "stop.log");
  expectTestbedInstrumentation(out, report);

  agentProc.requestTerminate();
  int code = -1;
  if (!agentProc.waitFor(10000, &code)) agentProc.kill();
}

TEST(InstIntegration, SidecarFileMergedOfflineWithTheCliMergeCommand) {
  TempDir dir;
  const fs::path fixture = dir.path() / "fixture.tachyon";
  inst::testing_util::writeBaseTrace(fixture, 1000, 2'000'000);
  const fs::path sidecar = dir.path() / "testbed.tachyon-inst";
  nlohmann::json report;
  {
    // No agent behind this address: the SDK falls back to the sidecar file.
    ScopedEnv env({{"TACHYON_AGENT_PIPE", testAddress("nobody")}, {"TACHYON_SIDECAR", sidecar.string()}});
    report = runTestbed(dir.path() / "testbed.log");
  }
  ASSERT_TRUE(fs::exists(sidecar));
  const fs::path merged = dir.path() / "merged.tachyon";
  std::ostringstream out, err;
  ASSERT_EQ(cli::run({"merge", fixture.string(), sidecar.string(), "--out", merged.string()}, out, err),
            cli::kExitOk)
      << err.str();
  EXPECT_NE(out.str().find("merged 1 instrumentation file(s)"), std::string::npos) << out.str();
  expectTestbedInstrumentation(merged, report);
}

}  // namespace
}  // namespace tachyon
