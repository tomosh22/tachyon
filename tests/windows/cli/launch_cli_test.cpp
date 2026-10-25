// Windows-only tests of launch / attach / child following (WP-13). They run the REAL executables: a
// `tachyon-agent --backend etw` spawned by the test, driven by the real `tachyon-cli`, capturing real
// processes through an ETW kernel session. They need an elevated process (the CI runner is) and skip with a
// message otherwise.
//
// ctest hides the output of passing tests, so every test appends its key numbers to a summary (stderr, the
// gtest XML via RecordProperty and <build>/launch_cli_summary.txt).
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#include "../../unit/agent/process_util.h"
#include "capture/windows_etw/etw_session.h"
#include "capture/windows_etw/process_tree.h"
#include "capture/windows_etw/windows_etw_backend.h"
#include "control/client.h"
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
#include <thread>
#include <vector>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using agent::testing_util::Process;
using agent::testing_util::readTextFile;
using agent::testing_util::runToCompletion;

// ---- helpers ----------------------------------------------------------------------------------------------

std::mutex g_summaryMutex;

// Thread-safe. Prints to stderr and appends to the summary file.
void note(const std::string& line) {
  std::lock_guard lock(g_summaryMutex);
  std::fprintf(stderr, "[launch-cli] %s\n", line.c_str());
  std::ofstream f(fs::path(TACHYON_BINARY_DIR) / "launch_cli_summary.txt", std::ios::app);
  f << line << '\n';
}

std::string currentTestName() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string(info->test_suite_name()) + "." + info->name();
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(const std::string& s, const std::string& prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

double seconds(Clock::time_point since) {
  return std::chrono::duration<double>(Clock::now() - since).count();
}

// Everything the tests need from a converted trace.
struct TraceView {
  trace::FileHeader header{};
  trace::StringTable strings;
  std::vector<trace::ProcessRec> processes;
  std::vector<trace::ThreadRec> threads;
  std::vector<trace::ModuleInfo> modules;
  std::vector<trace::FrameRec> frames;
  std::vector<trace::CallstackNodeRec> nodes;
  trace::SampleBatch samples;
  std::string captureInfo;
  bool partial = false;

  static Result<TraceView> load(const fs::path& path) {
    TACHYON_TRY_ASSIGN(trace::TraceReader reader, trace::TraceReader::open(path));
    TraceView v;
    v.header = reader.header();
    v.partial = reader.isPartial();
    TACHYON_TRY_ASSIGN(v.strings, reader.readStrings());
    TACHYON_TRY_ASSIGN(v.processes, reader.readProcesses());
    TACHYON_TRY_ASSIGN(v.threads, reader.readThreads());
    TACHYON_TRY_ASSIGN(v.modules, reader.readModuleInfos());
    TACHYON_TRY_ASSIGN(v.frames, reader.readFrames());
    TACHYON_TRY_ASSIGN(v.nodes, reader.readCallstackNodes());
    TACHYON_TRY_ASSIGN(v.samples, reader.readSamples());
    auto info = reader.readCaptureInfo();
    if (info.ok()) v.captureInfo = *info;
    return v;
  }

  std::string str(StringIdx idx) const { return std::string(strings.get(idx)); }

  // Pids of the processes whose threads are flagged as target-process threads.
  std::set<uint32_t> targetPids() const {
    std::set<uint32_t> pids;
    for (const trace::ThreadRec& t : threads)
      if ((t.flags & trace::kThreadFlagTargetProcess) != 0) pids.insert(t.pid);
    return pids;
  }

  // The pid of the process whose image (or command line executable) starts with `prefix` (lower case).
  uint32_t pidOfImage(const std::string& prefix) const {
    for (const trace::ProcessRec& p : processes) {
      const std::string name = lower(str(p.name));
      const std::string exe = lower(etw::baseNameOf(etw::executablePathOf(str(p.command_line))));
      if (startsWith(name, prefix) || startsWith(exe, prefix)) return p.pid;
    }
    return 0;
  }

  std::set<uint32_t> tidsOf(uint32_t pid) const {
    std::set<uint32_t> tids;
    for (const trace::ThreadRec& t : threads)
      if (t.pid == pid) tids.insert(t.tid);
    return tids;
  }

  uint64_t samplesOf(uint32_t pid) const {
    const std::set<uint32_t> tids = tidsOf(pid);
    uint64_t n = 0;
    for (uint32_t tid : samples.tid) n += tids.count(tid);
    return n;
  }

  // Module indices of `pid` whose path ends with `suffix` (lower case, e.g. "ntdll.dll").
  std::set<uint32_t> moduleIdsOf(uint32_t pid, const std::string& suffix) const {
    std::set<uint32_t> ids;
    for (size_t i = 0; i < modules.size(); ++i)
      if (modules[i].pid == pid && endsWith(lower(str(modules[i].path)), suffix))
        ids.insert(static_cast<uint32_t>(i));
    return ids;
  }

  // True when any frame of the callstack lies in one of `moduleIds`.
  bool stackHasModule(CallstackIdx cs, const std::set<uint32_t>& moduleIds) const {
    uint32_t i = cs.get();
    for (int guard = 0; i != 0 && i != kInvalidIdx && i < nodes.size() && guard < 512; ++guard) {
      const trace::CallstackNodeRec node = nodes[i];
      if (node.frame.get() < frames.size()) {
        const trace::FrameRec f = frames[node.frame.get()];
        if (f.module.valid() && moduleIds.count(f.module.get()) != 0) return true;
      }
      i = node.parent.get();
    }
    return false;
  }
};

struct EarlyFacts {
  uint64_t pidSamples = 0;
  uint64_t ntdllSamples = 0;
  uint64_t exeSamples = 0;
  uint64_t firstSampleTs = ~0ull;
  uint64_t firstNtdllTs = ~0ull;
  uint64_t firstExeTs = ~0ull;
  bool earliestHasNtdll = false;
  size_t ntdllModules = 0;
  size_t exeModules = 0;
};

// Per-sample module facts for one process: samples with an ntdll frame vs samples with a frame in its exe.
EarlyFacts earlyFacts(const TraceView& v, uint32_t pid, const std::string& exeSuffix) {
  EarlyFacts f;
  const std::set<uint32_t> tids = v.tidsOf(pid);
  const std::set<uint32_t> ntdll = v.moduleIdsOf(pid, "ntdll.dll");
  const std::set<uint32_t> exe = v.moduleIdsOf(pid, exeSuffix);
  f.ntdllModules = ntdll.size();
  f.exeModules = exe.size();
  for (size_t i = 0; i < v.samples.size(); ++i) {
    if (tids.count(v.samples.tid[i]) == 0) continue;
    const uint64_t ts = v.samples.ts[i];
    const bool inNtdll = v.stackHasModule(v.samples.callstack[i], ntdll);
    const bool inExe = v.stackHasModule(v.samples.callstack[i], exe);
    ++f.pidSamples;
    if (ts < f.firstSampleTs) {
      f.firstSampleTs = ts;
      f.earliestHasNtdll = inNtdll;
    }
    if (inNtdll) {
      ++f.ntdllSamples;
      f.firstNtdllTs = std::min(f.firstNtdllTs, ts);
    }
    if (inExe) {
      ++f.exeSamples;
      f.firstExeTs = std::min(f.firstExeTs, ts);
    }
  }
  return f;
}

double microsBetween(const TraceView& v, uint64_t from, uint64_t to) {
  const double freq =
      v.header.clock_frequency_hz != 0 ? static_cast<double>(v.header.clock_frequency_hz) : 1.0;
  return (static_cast<double>(to) - static_cast<double>(from)) * 1e6 / freq;
}

struct DoneEvent {
  bool found = false;
  std::string path;
  uint64_t eventsLost = ~0ull;
  double durationS = 0.0;
};

// Finds the capture_done line in `--json` output.
DoneEvent parseDone(const std::string& text) {
  DoneEvent d;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty() || line.front() != '{') continue;
    const auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.contains("event") || j["event"] != "capture_done") continue;
    d.found = true;
    d.path = j.value("path", std::string());
    d.eventsLost = j.value("events_lost", ~0ull);
    d.durationS = j.value("duration_s", 0.0);
  }
  return d;
}

// ---- fixture: a real agent process ------------------------------------------------------------------------

class LaunchCliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto priv = etw::EtwSession::enableProfilePrivilege();
    if (!priv.ok()) GTEST_SKIP() << "no SeSystemProfilePrivilege (not elevated): " << priv.error().message;
    m_dir = fs::path(TACHYON_BINARY_DIR) / "launch_cli_tmp" / currentTestName();
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    fs::create_directories(m_dir);
    m_address = R"(\\.\pipe\tachyon-launchtest-)" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(static_cast<unsigned>(Clock::now().time_since_epoch().count() & 0xFFFFFF));
    m_agent =
        Process::spawn(TACHYON_AGENT_EXE, {"--backend", "etw", "--socket", m_address}, m_dir / "agent.log");
    ASSERT_TRUE(m_agent.valid());
    auto c = control::Client::connectWithRetry(m_address, 20000);
    ASSERT_TRUE(c.ok()) << "agent did not start:\n" << readTextFile(m_dir / "agent.log");
    m_started = true;
  }

  void TearDown() override {
    if (m_agent.valid()) {
      m_agent.requestTerminate();
      int code = 0;
      if (!m_agent.waitFor(15000, &code)) m_agent.kill();
    }
    for (uint32_t pid : m_leftovers) killPid(pid);
    std::error_code ec;
    if (!m_dir.empty()) fs::remove_all(m_dir, ec);  // ETL files are large; the numbers live in the summary
  }

  fs::path dir() const { return m_dir; }
  std::string agentLog() const { return readTextFile(m_dir / "agent.log"); }

  void report(const std::string& line) {
    note(currentTestName() + ": " + line);
    m_summary += line + "\n";
    RecordProperty("summary", m_summary);
  }

  // Runs tachyon-cli against the agent; returns the exit code, stdout+stderr in `out`.
  int cli(std::vector<std::string> args, std::string* out = nullptr, int timeoutMs = 180000) {
    args.insert(args.begin(), {"--socket", m_address});
    const fs::path file = m_dir / ("cli-" + std::to_string(m_runs++) + ".txt");
    const int code = runToCompletion(TACHYON_CLI_EXE, args, file, timeoutMs);
    if (out != nullptr) *out = readTextFile(file);
    return code;
  }

  std::string agentState() {
    auto c = control::Client::connect(m_address);
    if (!c.ok()) return "unreachable";
    auto r = c->call(control::StatusRequest{});
    if (!r.ok() || !r->ok) return "error";
    return std::string(capture::captureStateName(std::get<control::StatusBody>(r->body).state));
  }

  bool waitForAgentState(const std::string& want, double timeoutS) {
    const auto begin = Clock::now();
    while (seconds(begin) < timeoutS) {
      if (agentState() == want) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  static void killPid(uint32_t pid) {
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid); h != nullptr) {
      TerminateProcess(h, 1);
      CloseHandle(h);
    }
  }

  // Copies the spawner under three different image names (the child filter matches on them).
  struct Tree {
    std::string match, grand, skip;
    std::vector<std::string> args;  // for tachyon-cli: --arg pairs describing the whole tree
  };
  Tree makeTree(double busySeconds) {
    Tree t;
    t.match = (m_dir / "tachyon-child-match.exe").string();
    t.grand = (m_dir / "tachyon-child-grand.exe").string();
    t.skip = (m_dir / "other-child-skip.exe").string();
    for (const std::string& p : {t.match, t.grand, t.skip})
      fs::copy_file(TACHYON_SPAWNER_EXE, p, fs::copy_options::overwrite_existing);
    std::ostringstream busy;
    busy << busySeconds;
    t.args = {"--arg", "--busy",  "--arg", busy.str(), "--arg", "--spawn", "--arg", t.match + ";" + t.grand,
              "--arg", "--spawn", "--arg", t.skip};
    return t;
  }

  // What the tree test looks at per process name.
  struct NodeFacts {
    uint32_t pid = 0;
    bool target = false;
    uint64_t samples = 0;
  };
  std::map<std::string, NodeFacts> nodeFacts(const TraceView& v) {
    std::map<std::string, NodeFacts> out;
    const std::set<uint32_t> targets = v.targetPids();
    for (const std::string& name :
         {"tachyon-test-spawner", "tachyon-child-match", "tachyon-child-grand", "other-child-skip"}) {
      NodeFacts n;
      n.pid = v.pidOfImage(name);
      n.target = targets.count(n.pid) != 0;
      n.samples = n.pid != 0 ? v.samplesOf(n.pid) : 0;
      out[name] = n;
    }
    return out;
  }

  std::string describe(const std::map<std::string, NodeFacts>& facts) const {
    std::ostringstream s;
    for (const auto& [name, n] : facts)
      s << name << "{pid " << n.pid << ", target " << (n.target ? "yes" : "no") << ", samples " << n.samples
        << "} ";
    return s.str();
  }

  fs::path m_dir;
  std::string m_address;
  Process m_agent;
  bool m_started = false;
  int m_runs = 0;
  std::string m_summary;
  std::vector<uint32_t> m_leftovers;
};

// ---- the acceptance test ----------------------------------------------------------------------------------

// WP-13 acceptance (3 s on CI instead of 20 s): `tachyon-cli capture --launch tachyon-testbed.exe --args
// "--scenario spin --duration N" --out spin.tachyon`; the trace must contain samples inside ntdll (the
// loader) of the target pid before the first sample in the testbed's own code.
TEST_F(LaunchCliTest, CaptureLaunchTestbedSpinHasLoaderSamplesBeforeTestbedCode) {
  constexpr int kSeconds = 3;
  const fs::path out = dir() / "spin.tachyon";
  std::string text;
  const auto begin = Clock::now();
  const int code = cli({"--json", "capture", "--launch", TACHYON_TESTBED_EXE, "--args",
                        "--scenario spin --duration " + std::to_string(kSeconds), "--out", out.string()},
                       &text);
  const double elapsed = seconds(begin);
  ASSERT_EQ(code, 0) << "cli output:\n" << text << "\nagent log:\n" << agentLog();
  const DoneEvent done = parseDone(text);
  ASSERT_TRUE(done.found) << text;
  report("cli capture --launch testbed --duration " + std::to_string(kSeconds) + ": exit " +
         std::to_string(code) + ", " + std::to_string(elapsed) + " s wall, capture_done events_lost=" +
         std::to_string(done.eventsLost) + " duration_s=" + std::to_string(done.durationS));
  EXPECT_EQ(done.eventsLost, 0u);
  EXPECT_GE(elapsed, static_cast<double>(kSeconds) - 0.5) << "the CLI waited for the testbed to exit";
  EXPECT_LT(elapsed, 90.0);
  EXPECT_EQ(done.path, out.string());

  auto view = TraceView::load(out);
  ASSERT_TRUE(view.ok()) << view.error().toString();
  EXPECT_FALSE(view->partial);

  const std::set<uint32_t> targets = view->targetPids();
  ASSERT_EQ(targets.size(), 1u) << "no --follow-children: only the launched process is a target";
  const uint32_t pid = *targets.begin();
  EXPECT_EQ(pid, view->pidOfImage("tachyon-testbed"));
  const std::set<uint32_t> tids = view->tidsOf(pid);
  std::set<uint32_t> sampledTids;
  for (uint32_t tid : view->samples.tid)
    if (tids.count(tid) != 0) sampledTids.insert(tid);
  EXPECT_GE(tids.size(), 1u);
  EXPECT_GE(sampledTids.size(), 1u);

  const EarlyFacts f = earlyFacts(*view, pid, "tachyon-testbed.exe");
  const std::string numbers =
      "pid " + std::to_string(pid) + ": " + std::to_string(tids.size()) + " threads (" +
      std::to_string(sampledTids.size()) + " sampled), " + std::to_string(f.pidSamples) + " samples of " +
      std::to_string(view->samples.size()) + " total, ntdll modules " + std::to_string(f.ntdllModules) +
      ", exe modules " + std::to_string(f.exeModules) + ", samples with an ntdll frame " +
      std::to_string(f.ntdllSamples) + ", samples with a testbed-exe frame " + std::to_string(f.exeSamples) +
      "; first sample -> first ntdll sample " +
      std::to_string(f.ntdllSamples != 0 ? microsBetween(*view, f.firstSampleTs, f.firstNtdllTs) : -1.0) +
      " us, -> first testbed-exe sample " +
      std::to_string(f.exeSamples != 0 ? microsBetween(*view, f.firstSampleTs, f.firstExeTs) : -1.0) +
      " us; earliest sample has an ntdll frame: " + (f.earliestHasNtdll ? "yes" : "no");
  report(numbers);

  EXPECT_GT(f.pidSamples, 5000u) << numbers << " (a spinning thread for " << kSeconds << " s at 8 kHz)";
  ASSERT_GE(f.ntdllModules, 1u) << "ntdll.dll is not among the modules of the target\n" << numbers;
  ASSERT_GE(f.exeModules, 1u) << numbers;
  ASSERT_GE(f.ntdllSamples, 1u) << "no sample of the target has an ntdll frame\n" << numbers;
  ASSERT_GE(f.exeSamples, 1u) << numbers;
  EXPECT_LT(f.firstNtdllTs, f.firstExeTs)
      << "an ntdll (loader) sample must precede the first sample in the testbed's code\n"
      << numbers;
  EXPECT_NE(view->captureInfo.find("target_pids"), std::string::npos) << view->captureInfo;
}

// ---- start_immediately=false: park, resume, stop ----------------------------------------------------------

TEST_F(LaunchCliTest, NoStartParksTheProcessAndResumeRecordsIt) {
  const fs::path out = dir() / "parked.tachyon";
  std::string text;
  ASSERT_EQ(cli({"start", "--launch", TACHYON_SPAWNER_EXE, "--args", "--busy 12", "--no-start", "--out",
                 out.string()},
                &text),
            0)
      << text << agentLog();
  EXPECT_EQ(agentState(), "starting") << "parked: no session yet";
  EXPECT_EQ(cli({"stop"}, &text), 1) << "a parked launch cannot be stopped: " << text;
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  ASSERT_EQ(cli({"resume"}, &text), 0) << text << agentLog();
  EXPECT_EQ(agentState(), "capturing");
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // the process is still alive here
  ASSERT_EQ(cli({"--json", "stop", "--wait"}, &text), 0) << text << agentLog();
  const DoneEvent done = parseDone(text);
  ASSERT_TRUE(done.found) << text;

  auto view = TraceView::load(out);
  ASSERT_TRUE(view.ok()) << view.error().toString();
  const std::set<uint32_t> targets = view->targetPids();
  ASSERT_EQ(targets.size(), 1u);
  const uint32_t pid = *targets.begin();
  m_leftovers.push_back(pid);  // it still runs its 12 s of work: stopping a capture never kills the target
  EXPECT_EQ(pid, view->pidOfImage("tachyon-test-spawner"));

  const EarlyFacts f = earlyFacts(*view, pid, "tachyon-test-spawner.exe");
  const std::string numbers =
      "parked pid " + std::to_string(pid) + ": " + std::to_string(f.pidSamples) + " samples, ntdll samples " +
      std::to_string(f.ntdllSamples) + ", exe samples " + std::to_string(f.exeSamples) + ", events_lost " +
      std::to_string(done.eventsLost) + ", trace span " +
      std::to_string(microsBetween(*view, view->header.start_ts, view->header.end_ts) / 1e6) + " s";
  report(numbers);
  EXPECT_EQ(done.eventsLost, 0u);
  EXPECT_GT(f.pidSamples, 5000u) << numbers << " (about 2.5 s of spinning after the resume)";
  EXPECT_GE(f.ntdllSamples, 1u) << numbers;
  EXPECT_GE(f.exeSamples, 1u) << numbers;
  EXPECT_LT(f.firstNtdllTs, f.firstExeTs) << "loader samples after the resume precede the program code\n"
                                          << numbers;
}

TEST_F(LaunchCliTest, CancelTerminatesAParkedProcess) {
  std::string text;
  ASSERT_EQ(cli({"start", "--launch", TACHYON_SPAWNER_EXE, "--args", "--busy 30", "--no-start", "--out",
                 (dir() / "cancelled.tachyon").string()},
                &text),
            0)
      << text;
  EXPECT_EQ(agentState(), "starting");
  ASSERT_EQ(cli({"cancel"}, &text), 0) << text;
  EXPECT_EQ(agentState(), "idle");
  EXPECT_FALSE(fs::exists(dir() / "cancelled.tachyon"));
  EXPECT_FALSE(fs::exists(dir() / "cancelled.tachyon.etl")) << "no session ever ran";
  // The agent is usable again.
  EXPECT_EQ(cli({"capture", "--launch", TACHYON_SPAWNER_EXE, "--args", "--busy 0.3", "--out",
                 (dir() / "after.tachyon").string()},
                &text),
            0)
      << text << agentLog();
}

// ---- environment and working directory --------------------------------------------------------------------

TEST_F(LaunchCliTest, LaunchedProcessGetsTheAgentPipeUserEnvironmentAndCwd) {
  const fs::path work = dir() / "work dir";
  fs::create_directories(work);
  const fs::path dump = dir() / "env.txt";
  std::string text;
  ASSERT_EQ(
      cli({"capture", "--launch", TACHYON_SPAWNER_EXE, "--arg", "--dump", "--arg", dump.string(), "--arg",
           "--busy", "--arg", "0.5", "--cwd", work.string(), "--env", "TACHYON_USER_VAR=hello world", "--env",
           "TACHYON_AGENT_PIPE=user-value-must-lose", "--out", (dir() / "env.tachyon").string()},
          &text),
      0)
      << text << agentLog();
  const std::string content = readTextFile(dump);
  report("env dump:\n" + content.substr(0, std::min<size_t>(content.size(), 600)));
  ASSERT_FALSE(content.empty()) << "the spawner wrote nothing";
  EXPECT_NE(content.find("ENV:TACHYON_AGENT_PIPE=" + m_address + "\n"), std::string::npos)
      << "expected the agent's own pipe name " << m_address << "\n"
      << content;
  EXPECT_EQ(content.find("user-value-must-lose"), std::string::npos);
  EXPECT_NE(content.find("ENV:TACHYON_USER_VAR=hello world\n"), std::string::npos) << content;
  EXPECT_NE(lower(content).find("env:path="), std::string::npos)
      << "the agent's own environment is inherited";
  EXPECT_NE(content.find("work dir\n"), std::string::npos) << "CWD line:\n" << content;
}

// ---- child following --------------------------------------------------------------------------------------

TEST_F(LaunchCliTest, FollowChildrenWithFilterKeepsMatchingDescendantsOnly) {
  const Tree tree = makeTree(2.0);
  const fs::path out = dir() / "filter.tachyon";
  std::string text;
  std::vector<std::string> args = {"--json", "capture", "--launch", TACHYON_SPAWNER_EXE};
  args.insert(args.end(), tree.args.begin(), tree.args.end());
  args.insert(args.end(),
              {"--follow-children", "--child-filter", "tachyon-child-*.exe", "--out", out.string()});
  ASSERT_EQ(cli(args, &text), 0) << text << agentLog();
  const DoneEvent done = parseDone(text);
  ASSERT_TRUE(done.found) << text;

  auto view = TraceView::load(out);
  ASSERT_TRUE(view.ok()) << view.error().toString();
  const auto facts = nodeFacts(*view);
  const std::string numbers = describe(facts) + "events_lost " + std::to_string(done.eventsLost);
  report(numbers);
  EXPECT_EQ(done.eventsLost, 0u);
  EXPECT_TRUE(facts.at("tachyon-test-spawner").target) << numbers;
  EXPECT_TRUE(facts.at("tachyon-child-match").target) << numbers;
  EXPECT_TRUE(facts.at("tachyon-child-grand").target) << "a matching grandchild of a followed child\n"
                                                      << numbers;
  EXPECT_FALSE(facts.at("other-child-skip").target) << "rejected by the filter\n" << numbers;
  EXPECT_GT(facts.at("tachyon-test-spawner").samples, 500u) << numbers;
  EXPECT_GT(facts.at("tachyon-child-match").samples, 500u) << numbers;
  EXPECT_GT(facts.at("tachyon-child-grand").samples, 500u) << numbers;
  EXPECT_EQ(facts.at("other-child-skip").samples, 0u) << numbers;
}

TEST_F(LaunchCliTest, FollowChildrenWithoutFilterKeepsEveryDescendant) {
  const Tree tree = makeTree(2.0);
  const fs::path out = dir() / "all.tachyon";
  std::string text;
  std::vector<std::string> args = {"--json", "capture", "--launch", TACHYON_SPAWNER_EXE};
  args.insert(args.end(), tree.args.begin(), tree.args.end());
  args.insert(args.end(), {"--follow-children", "--out", out.string()});
  ASSERT_EQ(cli(args, &text), 0) << text << agentLog();

  auto view = TraceView::load(out);
  ASSERT_TRUE(view.ok()) << view.error().toString();
  const auto facts = nodeFacts(*view);
  const std::string numbers = describe(facts);
  report(numbers);
  for (const auto& [name, n] : facts) {
    EXPECT_TRUE(n.target) << name << "\n" << numbers;
    EXPECT_GT(n.samples, 500u) << name << "\n" << numbers;
  }
}

TEST_F(LaunchCliTest, WithoutFollowChildrenOnlyTheRootIsCaptured) {
  const Tree tree = makeTree(1.5);
  const fs::path out = dir() / "root.tachyon";
  std::string text;
  std::vector<std::string> args = {"--json", "capture", "--launch", TACHYON_SPAWNER_EXE};
  args.insert(args.end(), tree.args.begin(), tree.args.end());
  args.insert(args.end(), {"--out", out.string(), "--duration", "4"});
  ASSERT_EQ(cli(args, &text), 0) << text << agentLog();

  auto view = TraceView::load(out);
  ASSERT_TRUE(view.ok()) << view.error().toString();
  const auto facts = nodeFacts(*view);
  const std::string numbers = describe(facts);
  report(numbers);
  EXPECT_TRUE(facts.at("tachyon-test-spawner").target) << numbers;
  for (const char* child : {"tachyon-child-match", "tachyon-child-grand", "other-child-skip"}) {
    EXPECT_FALSE(facts.at(child).target) << child << "\n" << numbers;
    EXPECT_EQ(facts.at(child).samples, 0u) << child << "\n" << numbers;
  }
}

TEST_F(LaunchCliTest, AttachComposesWithChildFollowing) {
  const Tree tree = makeTree(1.5);
  // The root starts its children 3 s after it started, i.e. after the attach below is recording.
  std::wstring cmd = L"\"" + fs::path(TACHYON_SPAWNER_EXE).wstring() + L"\" --delay 3 --busy 1.5 --spawn \"" +
                     fs::path(tree.match + ";" + tree.grand).wstring() + L"\" --spawn \"" +
                     fs::path(tree.skip).wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  ASSERT_TRUE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi));
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  m_leftovers.push_back(pi.dwProcessId);

  const fs::path out = dir() / "attach.tachyon";
  std::string text;
  ASSERT_EQ(cli({"--json", "capture", "--attach", std::to_string(pi.dwProcessId), "--follow-children",
                 "--child-filter", "tachyon-child-*.exe", "--out", out.string()},
                &text),
            0)
      << text << agentLog();
  const DoneEvent done = parseDone(text);
  ASSERT_TRUE(done.found) << text;

  auto view = TraceView::load(out);
  ASSERT_TRUE(view.ok()) << view.error().toString();
  const auto facts = nodeFacts(*view);
  const std::string numbers = describe(facts) + "attached pid " + std::to_string(pi.dwProcessId) +
                              ", events_lost " + std::to_string(done.eventsLost);
  report(numbers);
  EXPECT_EQ(facts.at("tachyon-test-spawner").pid, pi.dwProcessId) << numbers;
  EXPECT_TRUE(facts.at("tachyon-test-spawner").target) << numbers;
  EXPECT_TRUE(facts.at("tachyon-child-match").target) << numbers;
  EXPECT_TRUE(facts.at("tachyon-child-grand").target) << numbers;
  EXPECT_FALSE(facts.at("other-child-skip").target) << numbers;
  EXPECT_GT(facts.at("tachyon-child-match").samples, 200u) << numbers;
  EXPECT_EQ(facts.at("other-child-skip").samples, 0u) << numbers;
}

// ---- backend level: no agent, no CLI ---------------------------------------------------------------------

class LaunchBackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto priv = etw::EtwSession::enableProfilePrivilege();
    if (!priv.ok()) GTEST_SKIP() << "no SeSystemProfilePrivilege (not elevated): " << priv.error().message;
    m_dir = fs::path(TACHYON_BINARY_DIR) / "launch_backend_tmp" / currentTestName();
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    fs::create_directories(m_dir);
  }
  void TearDown() override {
    std::error_code ec;
    if (!m_dir.empty()) fs::remove_all(m_dir, ec);
  }
  fs::path m_dir;
};

bool processAlive(uint32_t pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (h == nullptr) return false;
  const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return alive;
}

TEST_F(LaunchBackendTest, ParkedLaunchIsTerminatedByStopAndNeverRecords) {
  WindowsEtwBackend backend;
  CaptureOptions o;
  o.mode = CaptureMode::Launch;
  o.exe = TACHYON_SPAWNER_EXE;
  o.args = {"--busy", "30"};
  o.startImmediately = false;
  o.outPath = m_dir / "parked.tachyon";
  ASSERT_TRUE(backend.start(o).ok());
  EXPECT_EQ(backend.state(), CaptureState::Starting);
  EXPECT_TRUE(backend.launchPending());
  const uint32_t pid = backend.rootPid();
  ASSERT_NE(pid, 0u);
  EXPECT_TRUE(processAlive(pid)) << "launched and parked";
  EXPECT_FALSE(backend.targetExited());
  EXPECT_FALSE(fs::exists(etw::etlSegmentPath(o.outPath, 0))) << "no session has run";

  CaptureOptions again = o;
  again.startImmediately = false;
  EXPECT_EQ(backend.start(again).error().code, ErrorCode::Busy) << "already parked";

  auto stopped = backend.stop();
  ASSERT_FALSE(stopped.ok());
  EXPECT_EQ(stopped.error().code, ErrorCode::NotFound);
  EXPECT_EQ(backend.state(), CaptureState::Idle);
  EXPECT_FALSE(backend.launchPending());
  bool dead = false;
  for (int i = 0; i < 100 && !dead; ++i) {
    dead = !processAlive(pid);
    if (!dead) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_TRUE(dead) << "the parked process should have been terminated";
}

TEST_F(LaunchBackendTest, TriggerStartsTheSessionAndExitIsDetected) {
  WindowsEtwBackend backend;
  CaptureOptions o;
  o.mode = CaptureMode::Launch;
  o.exe = TACHYON_SPAWNER_EXE;
  o.args = {"--busy", "1"};
  o.startImmediately = false;
  o.outPath = m_dir / "trigger.tachyon";
  ASSERT_TRUE(backend.start(o).ok());
  const uint32_t pid = backend.rootPid();
  ASSERT_NE(pid, 0u);

  CaptureOptions trigger;
  trigger.mode = CaptureMode::Launch;
  trigger.exe = "-";
  trigger.startImmediately = true;
  ASSERT_TRUE(backend.start(trigger).ok());
  EXPECT_EQ(backend.state(), CaptureState::Capturing);
  EXPECT_FALSE(backend.launchPending());
  EXPECT_EQ(backend.rootPid(), pid);

  const auto begin = Clock::now();
  while (!backend.targetExited() && seconds(begin) < 30.0)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(backend.targetExited());
  EXPECT_GE(seconds(begin), 0.5) << "the 1 s spin keeps the target alive";
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(result->eventsLost, 0u);
  EXPECT_TRUE(fs::exists(result->tracePath));
  EXPECT_EQ(backend.rootPid(), 0u) << "no live capture";
  EXPECT_FALSE(backend.targetExited());
  EXPECT_EQ(backend.diagnostics().targetPids, std::vector<uint32_t>{pid});
}

TEST_F(LaunchBackendTest, MissingExecutableFailsCleanly) {
  WindowsEtwBackend backend;
  CaptureOptions o;
  o.mode = CaptureMode::Launch;
  o.exe = (m_dir / "no-such-program.exe").string();
  o.outPath = m_dir / "missing.tachyon";
  auto r = backend.start(o);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound) << r.error().toString();
  EXPECT_EQ(backend.state(), CaptureState::Idle);
  EXPECT_FALSE(backend.launchPending());
  EXPECT_FALSE(fs::exists(etw::etlSegmentPath(o.outPath, 0)))
      << "the session started for the launch is removed";
  // And the backend still works.
  o.exe = TACHYON_SPAWNER_EXE;
  o.args = {"--busy", "0.2"};
  ASSERT_TRUE(backend.start(o).ok());
  const auto begin = Clock::now();
  while (!backend.targetExited() && seconds(begin) < 30.0)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(backend.stop().ok());
}

}  // namespace
}  // namespace tachyon::capture

#endif  // TACHYON_PLATFORM_WINDOWS
