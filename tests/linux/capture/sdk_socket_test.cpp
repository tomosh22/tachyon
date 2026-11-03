// WP-73: the SDK's Unix socket transport verified end to end against the real agent with the perf backend.
// Label linux_privileged: skips itself, with the reason, when the kernel refuses system-wide perf_event_open
// (the CI privileged step fails when anything skips).
//
// `tachyon-agent --backend perf` and `tachyon-cli capture --launch tachyon-testbed --instrument` run as
// separate processes. The testbed's SDK connects to the agent socket (inst_hello, per-process socket,
// SO_PEERCRED check) while perf samples it; the finished trace must hold sampled data and the SDK's scopes,
// merged, with balanced Begin/End per thread, and no sidecar file may be left behind.
#include "../../unit/agent/process_util.h"
#include "capture/linux_perf/linux_perf_backend.h"
#include "control/client.h"
#include "inst/merge.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
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
    m_path = fs::temp_directory_path() / ("tachyon-sdk-sock-" + std::to_string(::getpid()));
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

TEST(SdkSocketPerf, InstrumentedTestbedThroughTheRealAgentWithThePerfBackend) {
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
  const fs::path workDir = dir.path() / "work";  // the testbed's cwd: where a sidecar file would appear
  fs::create_directories(sockDir);
  fs::create_directories(workDir);
  ::chmod(sockDir.c_str(), 0700);
  const std::string address = (sockDir / "tachyon-agent.sock").string();
  const fs::path out = dir.path() / "sdk.tachyon";

  Process agent =
      Process::spawn(TACHYON_AGENT_EXE, {"--backend", "perf", "--socket", address}, dir.path() / "agent.log");
  ASSERT_TRUE(agent.valid());
  auto probeClient = control::Client::connectWithRetry(address, 15000);
  ASSERT_TRUE(probeClient.ok()) << readTextFile(dir.path() / "agent.log");
  struct stat st {};
  ASSERT_EQ(::stat(address.c_str(), &st), 0);
  EXPECT_TRUE(S_ISSOCK(st.st_mode));
  EXPECT_EQ(st.st_mode & 0777, 0600u);

  const int code = runToCompletion(TACHYON_CLI_EXE, {"--socket",   address,
                                                     "--json",     "capture",
                                                     "--launch",   TACHYON_TESTBED_EXE,
                                                     "--cwd",      workDir.string(),
                                                     "--arg",      "--instrument",
                                                     "--arg",      "--scenario",
                                                     "--arg",      "spin,lock",
                                                     "--arg",      "--duration",
                                                     "--arg",      "3",
                                                     "--env",      "TACHYON_AGENT_PIPE=" + address,
                                                     "--out",      out.string(),
                                                     "--duration", "60"},
                                   dir.path() / "cli.log", 180000);
  const std::string cliLog = readTextFile(dir.path() / "cli.log");
  ASSERT_EQ(code, 0) << cliLog << "\n--- agent ---\n" << readTextFile(dir.path() / "agent.log");

  // capture_done: path, nothing lost, and no "refused" warning (the peer-pid check let the testbed through).
  nlohmann::json done;
  std::istringstream lines(cliLog);
  for (std::string line; std::getline(lines, line);) {
    if (line.find("\"capture_done\"") == std::string::npos) continue;
    done = nlohmann::json::parse(line, nullptr, false);
  }
  ASSERT_TRUE(done.is_object()) << cliLog;
  EXPECT_EQ(fs::path(done["path"].get<std::string>()), out);
  EXPECT_EQ(done["events_lost"].get<uint64_t>(), 0u);
  if (done.contains("warnings"))
    for (const auto& w : done["warnings"])
      EXPECT_EQ(w.get<std::string>().find("refused"), std::string::npos) << w;
  agent.requestTerminate();
  int agentCode = -1;
  if (!agent.waitFor(15000, &agentCode)) agent.kill();

  // No sidecar file anywhere: the stream travelled over the socket.
  for (const auto& entry : fs::recursive_directory_iterator(dir.path()))
    EXPECT_NE(entry.path().extension(), ".tachyon-inst") << "sidecar file " << entry.path();

  auto opened = trace::TraceReader::open(out);
  ASSERT_TRUE(opened.ok()) << opened.error().toString();
  trace::TraceReader& reader = *opened;
  EXPECT_FALSE(reader.isPartial());
  EXPECT_NE(reader.header().flags & trace::kFlagHasInstrumentation, 0u);

  // Sampled data: the testbed's threads (the target process) carry thousands of samples.
  const auto threads = reader.readThreads().value();
  std::set<uint32_t> targetTids;
  for (const trace::ThreadRec& t : threads)
    if ((t.flags & trace::kThreadFlagTargetProcess) != 0) targetTids.insert(t.tid);
  ASSERT_FALSE(targetTids.empty());
  const trace::SampleBatch samples = reader.readSamples().value();
  size_t targetSamples = 0;
  for (uint32_t tid : samples.tid) targetSamples += targetTids.count(tid);
  EXPECT_GT(targetSamples, 2000u) << "samples on the testbed's threads";

  // The SDK's scopes, merged: balanced per thread, on threads the sampler knows, with names.
  const trace::StringTable strings = reader.readStrings().value();
  const auto scopes = reader.readScopeEvents().value();
  ASSERT_GT(scopes.size(), 1000u);
  const inst::ScopeBalance balance = inst::checkScopeBalance(scopes);
  EXPECT_EQ(balance.unmatchedEnds, 0u);
  EXPECT_EQ(balance.unclosedBegins, 0u);
  EXPECT_EQ(balance.outOfOrder, 0u);
  EXPECT_EQ(balance.begins, balance.ends);
  std::set<std::string> names;
  std::set<uint32_t> scopeTids;
  for (const auto& rec : scopes) {
    const trace::ScopeEventRec r = rec;
    scopeTids.insert(r.tid);
    if (r.kind == trace::kScopeBegin) names.insert(std::string(strings.get(r.id)));
  }
  for (const char* expected : {"spin_cycle", "hot_a", "hot_b", "hot_c", "lock_critical_section"})
    EXPECT_TRUE(names.count(expected) != 0) << "missing scope " << expected;
  for (uint32_t tid : scopeTids)
    EXPECT_TRUE(targetTids.count(tid) != 0)
        << "scope thread " << tid << " is not a sampled thread of the target";
  std::set<uint32_t> named;
  const auto nameRecs = reader.readThreadNames().value();
  for (const auto& rec : nameRecs) named.insert(trace::ThreadNameRec(rec).tid);
  for (uint32_t tid : scopeTids) EXPECT_TRUE(named.count(tid) != 0) << "scope thread without a name: " << tid;
}

}  // namespace
}  // namespace tachyon::capture
