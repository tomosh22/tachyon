// WP-65w: the ETW real-time live source against REAL kernel sessions (needs an elevated runner, which the CI
// one is; every test skips with the reason otherwise).
//
//   * AgentSpinLive: the live view's Windows acceptance (docs/specs/live.md section 5) through the real
//     agent (`tachyon-agent --backend etw`), the client library and a LiveModel: 20 s of the testbed's spin
//     scenario, sample latency p95 < 3 s, the window never above 30 s, no live_gap, the shares of the hot
//     functions in the last live snapshot within 5 points of the final trace (symbolised with DIA).
//   * LiveDoesNotChangeTheFinalTrace: a live capture and a plain one of the same workload write the same
//     amount of data (event counts within normal variance, 0 lost events, nothing lost in the real-time
//     buffers); also reports the cpu cost of the live consumer and decoder.
//   * AttachLiveKnowsTheModulesThatWereAlreadyLoaded: an attach starts with a Toolhelp rundown.
//   * CircularFlushKeepsTheLiveViewRunning: a flush restarts the session; the consumer follows.
//   * CliLiveAgainstAWindowsAgent: `tachyon-cli live` end to end, with a latency figure.
//
// ctest hides the output of passing tests, so the numbers go to stderr, the gtest XML (RecordProperty) and
// <build>/etw_live_summary.txt.
#include "../../unit/agent/process_util.h"
#include "capture/windows_etw/etw_session.h"
#include "capture/windows_etw/launch.h"
#include "capture/windows_etw/rt_consumer.h"
#include "capture/windows_etw/windows_etw_backend.h"
#include "control/client.h"
#include "model/live_model.h"
#include "model/model.h"
#include "symbols/dia/dia_provider.h"
#include "symbols/symbol_service.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
using namespace control;

// ---- helpers ----------------------------------------------------------------------------------------------

std::mutex g_summaryMutex;

// Thread-safe. Prints to stderr and appends to the summary file.
void note(const std::string& line) {
  std::lock_guard lock(g_summaryMutex);
  std::fprintf(stderr, "[etw-live] %s\n", line.c_str());
  std::ofstream f(fs::path(TACHYON_BINARY_DIR) / "etw_live_summary.txt", std::ios::app);
  f << line << '\n';
}

std::string currentTestName() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string(info->test_suite_name()) + "." + info->name();
}

double seconds(Clock::time_point since) {
  return std::chrono::duration<double>(Clock::now() - since).count();
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

// Seconds between a trace-clock timestamp and now, on this machine (the agent is local: same QPC).
double ageSeconds(uint64_t ts) {
  const uint64_t now = etw::traceClockNow();
  return now >= ts ? static_cast<double>(now - ts) / static_cast<double>(etw::traceClockFrequency()) : 0.0;
}

class EtwLiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto priv = etw::EtwSession::enableProfilePrivilege();
    if (!priv.ok()) GTEST_SKIP() << "no SeSystemProfilePrivilege (not elevated): " << priv.error().message;
    m_dir = fs::path(TACHYON_BINARY_DIR) / "etw_live_tmp" / currentTestName();
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    fs::create_directories(m_dir);
  }
  void TearDown() override {
    stopAgent();
    std::error_code ec;
    if (!m_dir.empty()) fs::remove_all(m_dir, ec);  // ETL files are large; the numbers live in the summary
  }

  fs::path dir() const { return m_dir; }
  void report(const std::string& line) {
    note(currentTestName() + ": " + line);
    m_summary += line + "\n";
    RecordProperty("summary", m_summary);
  }

  // A real agent on a named pipe.
  void startAgent() {
    m_address = "\\\\.\\pipe\\tachyon-liveetw-" + std::to_string(etw::EtwSession::currentPid()) + "-" +
                std::to_string(static_cast<unsigned>(Clock::now().time_since_epoch().count() & 0xFFFFFF));
    m_agent =
        Process::spawn(TACHYON_AGENT_EXE, {"--backend", "etw", "--socket", m_address}, m_dir / "agent.log");
    ASSERT_TRUE(m_agent.valid());
  }
  void stopAgent() {
    if (m_agent.valid()) {
      m_agent.requestTerminate();
      int code = 0;
      if (!m_agent.waitFor(20000, &code)) m_agent.kill();
    }
  }
  Result<Client> connect() { return Client::connectWithRetry(m_address, 30000); }
  std::string agentLog() const { return readTextFile(m_dir / "agent.log"); }

  StartRequest spinStart(int durationS, const std::string& out) const {
    StartRequest s;
    s.mode = CaptureMode::Launch;
    s.exe = TACHYON_TESTBED_EXE;
    s.args = {"--scenario", "spin", "--duration", std::to_string(durationS)};
    s.sampling_hz = 8000;
    s.out_path = (m_dir / out).string();
    s.live = true;
    return s;
  }

  fs::path m_dir;
  std::string m_summary;
  std::string m_address;
  Process m_agent;
};

// ---- symbolised shares (DIA)
// --------------------------------------------------------------------------------

// Leaf function shares (percent of the target's samples inside [t0, t1)) of a model, symbolised with DIA
// against the testbed's own PDB. Samples in other modules count under the name "".
std::map<std::string, double> leafShares(model::TraceModel m, uint64_t t0, uint64_t t1,
                                         uint64_t* samplesOut) {
  const fs::path exe(TACHYON_TESTBED_EXE);
  fs::path pdb = exe;
  pdb.replace_extension(".pdb");
  symbols::SymbolService svc({}, {});
  svc.setLocate(
      [pdb, exeName = lower(exe.filename().string())](const model::Module& mod) -> Result<fs::path> {
        if (lower(fs::path(mod.path).filename().string()) == exeName) return pdb;
        return makeError(ErrorCode::NotFound, "no symbols for " + mod.path);
      });
  symbols::registerDiaProvider(svc);
  svc.resolveAllAsync(m);
  svc.waitAll(m);
  std::map<std::string, uint64_t> counts;
  uint64_t total = 0;
  for (size_t t = 0; t < m.threads.size(); ++t) {
    if (!m.threads[t].is_target) continue;
    const auto& cols = m.samplesByThread[t];
    for (size_t i = 0; i < cols.ts.size(); ++i) {
      if (cols.ts[i] < t0 || cols.ts[i] >= t1) continue;
      ++total;
      std::string name;
      const uint32_t node = cols.callstack[i].get();
      if (node != 0 && node < m.nodes.size()) {
        const symbols::ResolvedAddress& ra = svc.lookup(m.nodes[node].frame);
        if (ra.resolved) name = std::string(svc.symbolStrings().get(ra.function));
      }
      ++counts[name];
    }
  }
  if (samplesOut != nullptr) *samplesOut = total;
  std::map<std::string, double> shares;
  for (const auto& [name, n] : counts)
    shares[name] = total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0;
  return shares;
}

// ---- a live client (agent)
// ------------------------------------------------------------------------------------

// Applies live events to a LiveModel and records how old each sample was when its batch arrived.
struct LiveSession {
  Client client;
  model::LiveModel model;
  std::vector<double> latencies;  // seconds, one per sample
  uint64_t events = 0, keyframes = 0;
  bool gap = false, end = false;
  std::string applyError;

  explicit LiveSession(Client c) : client(std::move(c)) {
    client.setLiveHandler([this](const Event& e) { onEvent(e); });
  }

  void onEvent(const Event& e) {
    if (const auto* le = std::get_if<LiveEvent>(&e)) {
      const uint64_t arrived = etw::traceClockNow();
      ++events;
      if (le->keyframe) ++keyframes;
      noteLatencies(le->data, arrived, le->keyframe);
      const auto* p = reinterpret_cast<const std::byte*>(le->data.data());
      if (auto r = model.apply({p, le->data.size()}, le->keyframe); !r.ok()) applyError = r.error().message;
    } else if (std::holds_alternative<LiveGapEvent>(e)) {
      gap = true;
    } else if (std::holds_alternative<LiveEndEvent>(e)) {
      end = true;
    }
  }

  void noteLatencies(const std::string& data, uint64_t arrived, bool keyframe) {
    if (keyframe || keyframes > 1) return;
    const double freq = static_cast<double>(etw::traceClockFrequency());
    size_t pos = 0;
    while (pos + sizeof(model::live_wire::RecordHeader) <= data.size()) {
      model::live_wire::RecordHeader h;
      std::memcpy(&h, data.data() + pos, sizeof(h));
      pos += sizeof(h);
      if (h.byte_len > data.size() - pos) return;
      if (h.type == static_cast<uint32_t>(model::live_wire::RecordType::Samples) && h.byte_len >= 4) {
        uint32_t count;
        std::memcpy(&count, data.data() + pos, 4);
        for (uint32_t i = 0; i < count && 4 + (i + 1) * 8ull <= h.byte_len; ++i) {
          uint64_t ts;
          std::memcpy(&ts, data.data() + pos + 4 + i * 8, 8);
          latencies.push_back(arrived >= ts ? static_cast<double>(arrived - ts) / freq : 0.0);
        }
      }
      pos += h.byte_len;
    }
  }
};

// ---- a recording sink (backend level)
// -------------------------------------------------------------------------

class CountingSink final : public ILiveSink {
 public:
  void onSessionInfo(const LiveSessionInfo& i) override {
    std::lock_guard lock(m);
    info = i;
    haveInfo = true;
  }
  void onProcess(const LiveProcess& p) override {
    std::lock_guard lock(m);
    processes.push_back(p);
  }
  void onThread(const LiveThread& t) override {
    std::lock_guard lock(m);
    threads.push_back(t);
  }
  void onModule(const LiveModule& mod) override {
    std::lock_guard lock(m);
    modules.push_back(mod);
  }
  void onSample(uint64_t ts, uint32_t pid, uint32_t tid, uint32_t, uint16_t,
                std::span<const uint64_t> ips) override {
    std::lock_guard lock(m);
    ++samples;
    sampleTs.push_back(ts);
    if (!ips.empty()) leafPcs.emplace_back(pid, ips);
    latencies.push_back(ageSeconds(ts));
    pids.insert(pid);
    tids.insert(tid);
  }
  void onContextSwitch(const LiveContextSwitch&) override {
    std::lock_guard lock(m);
    ++switches;
  }
  void onReady(const LiveReady&) override {
    std::lock_guard lock(m);
    ++readies;
  }
  void onTimeAdvance(uint64_t) override {
    std::lock_guard lock(m);
    ++heartbeats;
  }
  void onDropped(uint64_t n) override {
    std::lock_guard lock(m);
    dropped += n;
  }

  struct Stack {
    uint32_t pid;
    std::vector<uint64_t> ips;
    Stack(uint32_t p, std::span<const uint64_t> s) : pid(p), ips(s.begin(), s.end()) {}
  };

  mutable std::mutex m;
  LiveSessionInfo info;
  bool haveInfo = false;
  std::vector<LiveProcess> processes;
  std::vector<LiveThread> threads;
  std::vector<LiveModule> modules;
  uint64_t samples = 0, switches = 0, readies = 0, heartbeats = 0, dropped = 0;
  std::vector<uint64_t> sampleTs;
  std::vector<Stack> leafPcs;
  std::vector<double> latencies;
  std::set<uint32_t> pids, tids;
};

struct TraceCounts {
  uint64_t targetSamples = 0;
  uint64_t switches = 0;
  uint64_t readies = 0;
  uint64_t firstSampleTs = ~uint64_t{0}, lastSampleTs = 0;
};

TraceCounts countTrace(const fs::path& path) {
  TraceCounts c;
  auto m = model::TraceModel::load(path);
  if (!m.ok()) {
    ADD_FAILURE() << "cannot load " << path << ": " << m.error().toString();
    return c;
  }
  for (size_t t = 0; t < m->threads.size(); ++t) {
    if (!m->threads[t].is_target) continue;
    const auto& ts = m->samplesByThread[t].ts;
    c.targetSamples += ts.size();
    if (!ts.empty()) {
      c.firstSampleTs = std::min<uint64_t>(c.firstSampleTs, ts.front());
      c.lastSampleTs = std::max<uint64_t>(c.lastSampleTs, ts.back());
    }
  }
  c.switches = m->contextSwitches.size();
  c.readies = m->readyEvents.size();
  return c;
}

CaptureOptions spinOptions(const fs::path& out, int durationS, bool live) {
  CaptureOptions o;
  o.mode = CaptureMode::Launch;
  o.exe = TACHYON_TESTBED_EXE;
  o.args = {"--scenario", "spin", "--duration", std::to_string(durationS)};
  o.samplingHz = 8000;
  o.outPath = out;
  o.live = live;
  return o;
}

bool waitForTargetExit(WindowsEtwBackend& backend, double timeoutS) {
  const auto begin = Clock::now();
  while (seconds(begin) < timeoutS) {
    if (backend.targetExited()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// ---- the acceptance through the agent
// -------------------------------------------------------------------------

constexpr int kSpinSeconds = 20;

TEST_F(EtwLiveTest, AgentSpinLive) {
  startAgent();
  ASSERT_FALSE(HasFatalFailure());
  auto c = connect();
  ASSERT_TRUE(c.ok()) << "agent did not start:\n" << agentLog();
  LiveSession session(std::move(*c));

  auto started = session.client.call(spinStart(kSpinSeconds, "spin.tachyon"), 60000);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  ASSERT_TRUE(started->ok) << started->error << "\n" << agentLog();
  auto status = session.client.call(StatusRequest{}, 10000);
  ASSERT_TRUE(status.ok());
  EXPECT_TRUE(std::get<StatusBody>(status->body).live);
  auto sub = session.client.call(LiveSubscribeRequest{}, 30000);
  ASSERT_TRUE(sub.ok());
  ASSERT_TRUE(sub->ok) << sub->error;

  const auto began = Clock::now();
  std::vector<double> snapshotMs;
  double maxWindow = 0;
  std::optional<CaptureDoneEvent> done;
  auto nextSnapshot = began + std::chrono::seconds(1);
  while (!done) {
    auto ev = session.client.nextEvent(100);
    if (ev.ok()) {
      if (const auto* d = std::get_if<CaptureDoneEvent>(&*ev)) done = *d;
      if (const auto* e = std::get_if<ErrorEvent>(&*ev)) FAIL() << "capture error: " << e->message;
    } else if (ev.error().code != ErrorCode::Timeout) {
      FAIL() << "connection lost: " << ev.error().toString() << "\n" << agentLog();
    }
    if (Clock::now() >= nextSnapshot) {
      nextSnapshot += std::chrono::seconds(1);
      const auto t0 = Clock::now();
      auto snap = session.model.snapshot();
      snapshotMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
      const double hz = static_cast<double>(snap->header.clock_frequency_hz);
      if (hz > 0)
        maxWindow =
            std::max(maxWindow, static_cast<double>(snap->header.end_ts - snap->header.start_ts) / hz);
    }
    if (seconds(began) > kSpinSeconds + 120) FAIL() << "the capture did not end\n" << agentLog();
  }
  for (int i = 0; i < 20 && !session.end; ++i) (void)session.client.nextEvent(100);
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->summary.events_lost, 0u);

  const double p50 = percentile(session.latencies, 0.5), p95 = percentile(session.latencies, 0.95),
               pmax = percentile(session.latencies, 1.0);
  const double snapMax = snapshotMs.empty() ? 0 : *std::max_element(snapshotMs.begin(), snapshotMs.end());
  report("seconds=" + std::to_string(kSpinSeconds) + " samples=" + std::to_string(session.latencies.size()) +
         " latency_p50=" + std::to_string(p50) + " latency_p95=" + std::to_string(p95) +
         " latency_max=" + std::to_string(pmax) + " snapshot_ms_max=" + std::to_string(snapMax) +
         " max_window_s=" + std::to_string(maxWindow) + " events=" + std::to_string(session.events) +
         " keyframes=" + std::to_string(session.keyframes));

  EXPECT_TRUE(session.applyError.empty()) << session.applyError;
  EXPECT_FALSE(session.gap) << "live_gap";
  EXPECT_EQ(session.keyframes, 1u) << "only the first event is a keyframe when there is no gap";
  ASSERT_GT(session.latencies.size(), static_cast<size_t>(kSpinSeconds) * 2000u) << "samples seen live";
  EXPECT_LT(p95, 3.0) << "p95 latency (ETW real-time buffers flush about once a second)";
  EXPECT_LE(maxWindow, 30.0 + 0.6) << "the window never exceeds 30 s";
  EXPECT_EQ(session.model.stats().droppedLive, 0u) << "the live path dropped events";

  // The shares of the last live snapshot (after live_end: the whole window) against the final trace over the
  // same time range.
  auto finalSnap = session.model.snapshot();
  auto finalModel = model::TraceModel::load(done->summary.path);
  ASSERT_TRUE(finalModel.ok()) << finalModel.error().toString();
  uint64_t liveSamples = 0, finalSamples = 0;
  const auto live =
      leafShares(*finalSnap, finalSnap->header.start_ts, finalSnap->header.end_ts, &liveSamples);
  const auto fin =
      leafShares(*finalModel, finalSnap->header.start_ts, finalSnap->header.end_ts, &finalSamples);
  report("live samples=" + std::to_string(liveSamples) + " final samples=" + std::to_string(finalSamples));
  EXPECT_GT(liveSamples, 10000u);
  EXPECT_NEAR(static_cast<double>(liveSamples), static_cast<double>(finalSamples),
              0.03 * static_cast<double>(finalSamples) + 100)
      << "the live window holds the same samples as the final trace's range";
  for (const char* fn : {"hot_a", "hot_b", "hot_c", "spin_rest"}) {
    const double l = live.count(fn) ? live.at(fn) : 0.0, f = fin.count(fn) ? fin.at(fn) : 0.0;
    report(std::string("share ") + fn + " live=" + std::to_string(l) + " final=" + std::to_string(f));
    EXPECT_NEAR(l, f, 5.0) << fn;
  }
  EXPECT_GT(fin.count("hot_a") ? fin.at("hot_a") : 0.0, 20.0) << "the workload is what the shares say it is";
}

// ---- the final trace does not depend on live
// -------------------------------------------------------------------

TEST_F(EtwLiveTest, LiveDoesNotChangeTheFinalTrace) {
  constexpr int kSeconds = 8;
  struct Run {
    TraceCounts counts;
    WindowsEtwDiagnostics diag;
    double wallS = 0;
    std::shared_ptr<CountingSink> sink = std::make_shared<CountingSink>();
  };
  auto run = [&](bool live, const std::string& name) -> std::optional<Run> {
    Run r;
    WindowsEtwBackend backend;
    backend.setLiveSink(r.sink);
    const fs::path out = dir() / (name + ".tachyon");
    if (auto p = backend.checkPrivileges(); !p.ok()) {
      ADD_FAILURE() << p.error().toString();
      return std::nullopt;
    }
    const auto begin = Clock::now();
    auto s = backend.start(spinOptions(out, kSeconds, live));
    if (!s.ok()) {
      ADD_FAILURE() << "start: " << s.error().toString();
      return std::nullopt;
    }
    EXPECT_TRUE(waitForTargetExit(backend, kSeconds + 60));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const WindowsEtwDiagnostics during = backend.diagnostics();
    auto stopped = backend.stop();
    if (!stopped.ok()) {
      ADD_FAILURE() << "stop: " << stopped.error().toString();
      return std::nullopt;
    }
    r.wallS = seconds(begin);
    r.diag = backend.diagnostics();
    r.counts = countTrace(stopped->tracePath);
    EXPECT_EQ(stopped->eventsLost, 0u);
    EXPECT_EQ(r.diag.lastCounters.eventsLost, 0u);
    EXPECT_EQ(r.diag.lastCounters.logBuffersLost, 0u);
    EXPECT_EQ(r.diag.lastCounters.realTimeBuffersLost, 0u);
    EXPECT_EQ(during.liveActive, live);
    return r;
  };

  auto plain = run(false, "plain");
  ASSERT_TRUE(plain.has_value());
  auto live = run(true, "live");
  ASSERT_TRUE(live.has_value());

  report("plain: target samples=" + std::to_string(plain->counts.targetSamples) + " switches=" +
         std::to_string(plain->counts.switches) + " readies=" + std::to_string(plain->counts.readies));
  report("live:  target samples=" + std::to_string(live->counts.targetSamples) + " switches=" +
         std::to_string(live->counts.switches) + " readies=" + std::to_string(live->counts.readies));
  ASSERT_GT(plain->counts.targetSamples, 20000u);
  ASSERT_GT(live->counts.targetSamples, 20000u);
  const double sampleRatio =
      static_cast<double>(live->counts.targetSamples) / static_cast<double>(plain->counts.targetSamples);
  EXPECT_NEAR(sampleRatio, 1.0, 0.10) << "a live capture writes the same samples as a plain one";
  const double switchRatio =
      static_cast<double>(live->counts.switches + 50) / static_cast<double>(plain->counts.switches + 50);
  EXPECT_GT(switchRatio, 0.5) << "context switches";
  EXPECT_LT(switchRatio, 2.0) << "context switches";

  // What the live path saw is what the final trace of the same run holds.
  const WindowsEtwDiagnostics& d = live->diag;
  CountingSink& sink = *live->sink;
  std::lock_guard lock(sink.m);
  EXPECT_TRUE(sink.haveInfo);
  EXPECT_EQ(sink.info.platform, trace::kPlatformWindows);
  EXPECT_EQ(sink.info.clockFrequencyHz, etw::traceClockFrequency());
  EXPECT_NEAR(static_cast<double>(sink.info.samplingHz), 8190.0, 1.0);
  EXPECT_EQ(sink.dropped, 0u);
  EXPECT_EQ(d.live.droppedEvents, 0u);
  EXPECT_EQ(d.live.metaLost, 0u);
  EXPECT_NEAR(static_cast<double>(sink.samples), static_cast<double>(live->counts.targetSamples),
              0.05 * static_cast<double>(live->counts.targetSamples) + 100)
      << "samples decoded live vs samples in the final trace";
  EXPECT_NEAR(static_cast<double>(sink.switches), static_cast<double>(live->counts.switches),
              0.25 * static_cast<double>(live->counts.switches) + 100);
  EXPECT_GT(sink.heartbeats, 20u);

  const double p50 = percentile(sink.latencies, 0.5), p95 = percentile(sink.latencies, 0.95);
  const double decodePct = 100.0 * d.live.decodeCpuSeconds / live->wallS;
  const double consumerPct = 100.0 * d.liveConsumer.cpuSeconds / live->wallS;
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "live cost: decoder cpu %.2f s (%.1f%% of one core), consumer cpu %.2f s (%.1f%%), events "
                "seen %llu, buffers %llu, decoded samples/switches/readies %llu/%llu/%llu, backend-level "
                "sample age p50 %.3f p95 %.3f s, max event age in queue %.3f s",
                d.live.decodeCpuSeconds, decodePct, d.liveConsumer.cpuSeconds, consumerPct,
                static_cast<unsigned long long>(d.liveConsumer.events),
                static_cast<unsigned long long>(d.liveConsumer.buffers),
                static_cast<unsigned long long>(d.live.decoder.samples),
                static_cast<unsigned long long>(d.live.decoder.switches),
                static_cast<unsigned long long>(d.live.decoder.readies), p50, p95, d.live.maxEventAgeSeconds);
  report(buf);
  EXPECT_LT(p95, 3.0);
  EXPECT_LT(decodePct, 50.0);
}

// ---- attach: the rundown
// ------------------------------------------------------------------------------------------

TEST_F(EtwLiveTest, AttachLiveKnowsTheModulesThatWereAlreadyLoaded) {
  etw::LaunchSpec spec;
  spec.exe = TACHYON_TESTBED_EXE;
  spec.args = {"--scenario", "spin", "--duration", "25"};
  auto target = etw::TargetProcess::launchSuspended(spec);
  ASSERT_TRUE(target.ok()) << target.error().toString();
  const uint32_t pid = target->pid();
  ASSERT_TRUE(target->resume().ok());
  std::this_thread::sleep_for(std::chrono::seconds(2));  // it has loaded its DLLs and spins by now

  auto sink = std::make_shared<CountingSink>();
  WindowsEtwBackend backend;
  backend.setLiveSink(sink);
  ASSERT_TRUE(backend.checkPrivileges().ok());
  CaptureOptions o;
  o.mode = CaptureMode::Attach;
  o.pid = pid;
  o.samplingHz = 8000;
  o.outPath = dir() / "attach.tachyon";
  o.live = true;
  auto s = backend.start(o);
  ASSERT_TRUE(s.ok()) << s.error().toString();
  std::this_thread::sleep_for(std::chrono::seconds(6));
  auto stopped = backend.stop();
  target->terminate();
  ASSERT_TRUE(stopped.ok()) << stopped.error().toString();

  std::lock_guard lock(sink->m);
  const std::string exeName = lower(fs::path(TACHYON_TESTBED_EXE).filename().string());
  const LiveModule* exe = nullptr;
  for (const LiveModule& m : sink->modules)
    if (m.pid == pid && lower(fs::path(m.path).filename().string()) == exeName) exe = &m;
  ASSERT_NE(exe, nullptr) << "the rundown reports the module the target had loaded before the capture";
  EXPECT_EQ(exe->idKind, trace::kModuleIdPdb) << "identified by the PDB guid of its PE";
  EXPECT_EQ(exe->unloadTs, 0u);
  bool haveNtdll = false;
  for (const LiveModule& m : sink->modules)
    if (m.pid == pid && lower(fs::path(m.path).filename().string()) == "ntdll.dll") haveNtdll = true;
  EXPECT_TRUE(haveNtdll);

  bool haveProcess = false;
  for (const LiveProcess& p : sink->processes)
    if (p.pid == pid) haveProcess = true;
  EXPECT_TRUE(haveProcess);
  size_t targetThreads = 0;
  for (const LiveThread& t : sink->threads)
    if (t.pid == pid && t.isTarget) ++targetThreads;
  EXPECT_GE(targetThreads, 1u);

  ASSERT_GT(sink->samples, 10000u);
  EXPECT_EQ(sink->pids, std::set<uint32_t>{pid}) << "only the attached process is sampled";
  size_t inExe = 0;
  for (const auto& st : sink->leafPcs)
    for (uint64_t ip : st.ips)
      if (ip >= exe->base && ip < exe->base + exe->size) {
        ++inExe;
        break;
      }
  report("attach: samples=" + std::to_string(sink->samples) + " with a frame in the testbed=" +
         std::to_string(inExe) + " modules=" + std::to_string(sink->modules.size()));
  EXPECT_GT(static_cast<double>(inExe), 0.9 * static_cast<double>(sink->samples))
      << "samples of the spinning thread resolve against the rundown module";
}

// ---- circular flush
// -----------------------------------------------------------------------------------------------

TEST_F(EtwLiveTest, CircularFlushKeepsTheLiveViewRunning) {
  constexpr int kSeconds = 10;
  auto sink = std::make_shared<CountingSink>();
  WindowsEtwBackend backend;
  backend.setLiveSink(sink);
  ASSERT_TRUE(backend.checkPrivileges().ok());
  CaptureOptions o = spinOptions(dir() / "circ.tachyon", kSeconds, true);
  o.circularMb = 64;
  auto s = backend.start(o);
  ASSERT_TRUE(s.ok()) << s.error().toString();
  std::this_thread::sleep_for(std::chrono::seconds(4));
  const uint64_t beforeFlush = [&] {
    std::lock_guard lock(sink->m);
    return sink->samples;
  }();
  auto flushed = backend.flush("flush-a", 3);
  ASSERT_TRUE(flushed.ok()) << flushed.error().toString();
  const uint64_t flushTs = etw::traceClockNow();
  EXPECT_TRUE(fs::exists(flushed->tracePath));
  EXPECT_TRUE(waitForTargetExit(backend, kSeconds + 60));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  auto stopped = backend.stop();
  ASSERT_TRUE(stopped.ok()) << stopped.error().toString();

  std::lock_guard lock(sink->m);
  const uint64_t afterFlushStart = flushTs + etw::traceClockFrequency();  // one second later
  uint64_t after = 0;
  for (uint64_t ts : sink->sampleTs)
    if (ts > afterFlushStart) ++after;
  report("circular+live: samples before flush=" + std::to_string(beforeFlush) +
         " total=" + std::to_string(sink->samples) + " later than flush+1s=" + std::to_string(after) +
         " dropped=" + std::to_string(sink->dropped));
  EXPECT_GT(beforeFlush, 10000u);
  EXPECT_GT(after, 10000u) << "the consumer followed the restarted session";
}

// ---- tachyon-cli live
// ------------------------------------------------------------------------------------------------

TEST_F(EtwLiveTest, CliLiveAgainstAWindowsAgent) {
  startAgent();
  ASSERT_FALSE(HasFatalFailure());
  {
    auto c = connect();
    ASSERT_TRUE(c.ok()) << "agent did not start:\n" << agentLog();
  }
  const fs::path outFile = dir() / "cli.txt";
  const int code = runToCompletion(TACHYON_CLI_EXE,
                                   {"--socket", m_address, "--json", "live", "--launch", TACHYON_TESTBED_EXE,
                                    "--args", "--scenario spin --duration 10", "--duration", "8",
                                    "--interval", "1", "--out", (dir() / "cli-live.tachyon").string()},
                                   outFile, 180000);
  const std::string text = readTextFile(outFile);
  ASSERT_EQ(code, 0) << text << "\nagent log:\n" << agentLog();
  std::istringstream lines(text);
  std::string line;
  size_t rows = 0, withLatency = 0, gapRows = 0;
  uint64_t maxSamples = 0;
  double maxLatency = 0;
  std::vector<double> lateLatencies;
  while (std::getline(lines, line)) {
    if (line.empty() || line.front() != '{') continue;
    const auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.contains("samples")) continue;
    ++rows;
    maxSamples = std::max<uint64_t>(maxSamples, j.value("samples", 0ull));
    if (j.value("gaps", 0ull) != 0) ++gapRows;
    if (j.contains("latency_s") && j["latency_s"].is_number()) {
      ++withLatency;
      const double l = j["latency_s"].get<double>();
      maxLatency = std::max(maxLatency, l);
      if (rows > 4) lateLatencies.push_back(l);
    }
  }
  report("cli live: rows=" + std::to_string(rows) + " rows with latency=" + std::to_string(withLatency) +
         " max samples in window=" + std::to_string(maxSamples) +
         " max latency=" + std::to_string(maxLatency));
  EXPECT_GE(rows, 5u);
  EXPECT_GT(maxSamples, 10000u);
  EXPECT_EQ(gapRows, 0u);
  EXPECT_GE(withLatency, 3u) << "latency is printed for a local Windows agent";
  for (double l : lateLatencies) EXPECT_LT(l, 4.0) << "latency of the newest sample in the window";
}

// TEMPORARY (removed before the final push): ctest hides the output of passing tests, so this prints the
// numbers the tests above collected.
TEST(EtwLiveSummaryDump, PrintsTheNumbersOfThisRun) {
  std::ifstream f(fs::path(TACHYON_BINARY_DIR) / "etw_live_summary.txt");
  std::stringstream ss;
  ss << f.rdbuf();
  ADD_FAILURE() << "SUMMARY-BEGIN\n" << ss.str() << "SUMMARY-END";
}

}  // namespace
}  // namespace tachyon::capture
