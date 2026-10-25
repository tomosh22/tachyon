// Windows-only tests of the ETW session controller (WP-11). They start REAL kernel sessions, so they need an
// elevated process (the CI runner is); every test skips with a message when SeSystemProfilePrivilege is not
// available.
//
// ctest hides the output of passing tests, so every test appends its key numbers to a summary (stderr, the
// gtest XML via RecordProperty and <build>/etw_capture_summary.txt).
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#include "agent/agent.h"
#include "capture/windows_etw/etw_session.h"
#include "capture/windows_etw/windows_etw_backend.h"
#include "control/client.h"
#include "control/server.h"
#include "trace/trace_reader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// Order matters: wmistr.h before evntrace.h.
#include <evntrace.h>
#include <wmistr.h>

#include <gtest/gtest.h>

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
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- helpers ----------------------------------------------------------------------------------------------

std::mutex g_summaryMutex;

// Thread-safe. Prints to stderr and appends to the summary file.
void note(const std::string& line) {
  std::lock_guard lock(g_summaryMutex);
  std::fprintf(stderr, "[etw-capture] %s\n", line.c_str());
  std::ofstream f(fs::path(TACHYON_BINARY_DIR) / "etw_capture_summary.txt", std::ios::app);
  f << line << '\n';
}

std::string currentTestName() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string(info->test_suite_name()) + "." + info->name();
}

uint64_t fileSize(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? 0 : static_cast<uint64_t>(n);
}

class EtwCaptureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto priv = etw::EtwSession::enableProfilePrivilege();
    if (!priv.ok()) {
      GTEST_SKIP() << "no SeSystemProfilePrivilege (not elevated): " << priv.error().message;
    }
    m_dir = fs::path(TACHYON_BINARY_DIR) / "etw_capture_tmp" / currentTestName();
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    fs::create_directories(m_dir);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(m_dir, ec);  // ETL files are large; the numbers live in the summary
  }

  fs::path dir() const { return m_dir; }
  void report(const std::string& line) {
    note(currentTestName() + ": " + line);
    m_summary += line + "\n";
    RecordProperty("summary", m_summary);
  }

  fs::path m_dir;
  std::string m_summary;
};

// Busy threads and sleepers so the machine produces samples and context switches.
class Load {
 public:
  explicit Load(unsigned spinners = 2, unsigned sleepers = 2) {
    for (unsigned i = 0; i < spinners; ++i)
      m_threads.emplace_back([this] {
        volatile uint64_t x = 0;
        while (!m_stop.load(std::memory_order_relaxed)) {
          for (int k = 0; k < 20000; ++k) x = x + static_cast<uint64_t>(k);
        }
      });
    for (unsigned i = 0; i < sleepers; ++i)
      m_threads.emplace_back([this] {
        while (!m_stop.load(std::memory_order_relaxed))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
      });
  }
  ~Load() { stop(); }
  void stop() {
    m_stop = true;
    for (auto& t : m_threads)
      if (t.joinable()) t.join();
    m_threads.clear();
  }

 private:
  std::atomic<bool> m_stop{false};
  std::vector<std::thread> m_threads;
};

void sleepFor(double seconds) {
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

// A process that has been started and has exited, so its pid is dead.
uint32_t deadPid() {
  wchar_t cmd[] = L"cmd.exe /c exit 0";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return 0;
  const uint32_t pid = pi.dwProcessId;
  WaitForSingleObject(pi.hProcess, 10000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return pid;
}

class Testbed {
 public:
  Testbed(const std::string& scenario, int durationSeconds) {
    std::wstring cmd = L"\"" + fs::path(TACHYON_TESTBED_EXE).wstring() + L"\" --scenario " +
                       fs::path(scenario).wstring() + L" --duration " + std::to_wstring(durationSeconds);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                       &pi)) {
      m_process = pi.hProcess;
      m_pid = pi.dwProcessId;
      CloseHandle(pi.hThread);
    }
  }
  ~Testbed() {
    if (m_process != nullptr) {
      TerminateProcess(m_process, 1);
      CloseHandle(m_process);
    }
  }
  Testbed(const Testbed&) = delete;
  Testbed& operator=(const Testbed&) = delete;
  bool started() const { return m_process != nullptr; }
  uint32_t pid() const { return m_pid; }
  // True when the process exited within the timeout.
  bool wait(int timeoutMs) {
    return WaitForSingleObject(m_process, static_cast<DWORD>(timeoutMs)) == WAIT_OBJECT_0;
  }

 private:
  HANDLE m_process = nullptr;
  uint32_t m_pid = 0;
};

struct TraceFacts {
  uint64_t samples = 0;
  uint64_t minTs = 0;
  uint64_t maxTs = 0;
  uint64_t freq = 0;
  uint32_t platform = 0;
  bool partial = false;
  std::map<uint32_t, uint32_t> tidToPid;  // all threads in the file
  std::map<uint32_t, uint64_t> samplesPerTid;
  double spanSeconds() const {
    return freq == 0 || maxTs < minTs ? 0.0 : static_cast<double>(maxTs - minTs) / static_cast<double>(freq);
  }
  uint64_t samplesOfPid(uint32_t pid) const {
    uint64_t n = 0;
    for (const auto& [tid, count] : samplesPerTid) {
      auto it = tidToPid.find(tid);
      if (it != tidToPid.end() && it->second == pid) n += count;
    }
    return n;
  }
};

Result<TraceFacts> readFacts(const fs::path& path) {
  TACHYON_TRY_ASSIGN(trace::TraceReader reader, trace::TraceReader::open(path));
  TraceFacts f;
  f.freq = reader.header().clock_frequency_hz;
  f.platform = reader.header().platform;
  f.partial = reader.isPartial();
  TACHYON_TRY_ASSIGN(auto threads, reader.readThreads());
  for (const trace::ThreadRec& t : threads) f.tidToPid[t.tid] = t.pid;
  TACHYON_TRY_ASSIGN(trace::SampleBatch samples, reader.readSamples());
  f.samples = samples.size();
  if (!samples.ts.empty()) {
    f.minTs = *std::min_element(samples.ts.begin(), samples.ts.end());
    f.maxTs = *std::max_element(samples.ts.begin(), samples.ts.end());
  }
  for (uint32_t tid : samples.tid) ++f.samplesPerTid[tid];
  return f;
}

CaptureOptions systemOptions(const fs::path& out, uint32_t circularMb = 0) {
  CaptureOptions o;
  o.mode = CaptureMode::System;
  o.samplingHz = 8000;
  o.outPath = out;
  o.circularMb = circularMb;
  return o;
}

std::string describe(const etw::SessionCounters& c) {
  std::ostringstream s;
  s << "eventsLost=" << c.eventsLost << " logBuffersLost=" << c.logBuffersLost
    << " realTimeBuffersLost=" << c.realTimeBuffersLost << " buffersWritten=" << c.buffersWritten
    << " numberOfBuffers=" << c.numberOfBuffers << " bufferSizeKb=" << c.bufferSizeKb;
  return s.str();
}

// ---- start / stop -----------------------------------------------------------------------------------------

TEST_F(EtwCaptureTest, SystemModeStartStopProducesEtlAndOpenableTrace) {
  WindowsEtwBackend backend;
  ASSERT_TRUE(backend.checkPrivileges().ok());
  std::vector<std::pair<CaptureState, float>> progress;
  backend.setProgressCallback([&](CaptureState s, float p) { progress.emplace_back(s, p); });

  const fs::path out = dir() / "sys.tachyon";
  ASSERT_EQ(backend.state(), CaptureState::Idle);
  auto started = backend.start(systemOptions(out));
  ASSERT_TRUE(started.ok()) << started.error().toString();
  EXPECT_EQ(backend.state(), CaptureState::Capturing);
  const WindowsEtwDiagnostics live = backend.diagnostics();
  EXPECT_EQ(live.sessionName.rfind("Tachyon ", 0), 0u) << live.sessionName;

  Load load;
  sleepFor(2.0);
  load.stop();
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(backend.state(), CaptureState::Done);

  const WindowsEtwDiagnostics diag = backend.diagnostics();
  report("start/stop 2 s: session '" + live.sessionName +
         "', kernelGuidFallback=" + (live.startInfo.kernelGuidFallback ? "yes" : "no") +
         ", intervalSetWithSessionHandle=" + (live.startInfo.intervalSetWithSessionHandle ? "yes" : "no") +
         ", stackTracing=" + (live.startInfo.stackTracingEnabled ? "yes" : "no"));
  report("start/stop 2 s: " + describe(diag.lastCounters) + ", etl " +
         std::to_string(fileSize(result->rawPath)) + " bytes, tachyon " +
         std::to_string(fileSize(result->tracePath)) + " bytes, " +
         std::to_string(diag.lastConvertStats.eventsTotal) + " events, " +
         std::to_string(diag.lastConvertStats.samplesWritten) + " samples, convert " +
         std::to_string(diag.lastConvertStats.seconds) + " s");

  EXPECT_EQ(result->tracePath, out);
  EXPECT_EQ(result->rawPath, etw::etlSegmentPath(out, 0));
  EXPECT_TRUE(fs::exists(result->rawPath));
  EXPECT_GT(fileSize(result->rawPath), 0u);
  EXPECT_GT(result->durationS, 1.5);
  EXPECT_LT(result->durationS, 30.0);

  auto facts = readFacts(out);
  ASSERT_TRUE(facts.ok()) << facts.error().toString();
  EXPECT_EQ(facts->platform, trace::kPlatformWindows);
  EXPECT_FALSE(facts->partial);
  EXPECT_GT(facts->samples, 1000u) << "2 s at ~8 kHz on every core";
  EXPECT_GT(facts->freq, 0u);

  // Progress: Converting events, monotonic, then Done at 1.0.
  ASSERT_FALSE(progress.empty());
  float last = 0;
  bool sawConverting = false;
  for (const auto& [state, p] : progress) {
    if (state == CaptureState::Converting) {
      sawConverting = true;
      EXPECT_GE(p, last);
      last = p;
    }
  }
  EXPECT_TRUE(sawConverting);
  EXPECT_EQ(progress.back().first, CaptureState::Done);
  EXPECT_FLOAT_EQ(progress.back().second, 1.0f);

  // A second capture works on the same backend object.
  ASSERT_TRUE(backend.start(systemOptions(dir() / "sys2.tachyon")).ok());
  sleepFor(0.5);
  auto second = backend.stop();
  ASSERT_TRUE(second.ok()) << second.error().toString();
  EXPECT_TRUE(trace::TraceReader::open(second->tracePath).ok());
}

TEST_F(EtwCaptureTest, SecondStartWhileCapturingIsBusyAndStopWithoutStartIsNotFound) {
  WindowsEtwBackend backend;
  EXPECT_EQ(backend.stop().error().code, ErrorCode::NotFound);
  ASSERT_TRUE(backend.start(systemOptions(dir() / "busy.tachyon")).ok());
  auto again = backend.start(systemOptions(dir() / "busy2.tachyon"));
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().code, ErrorCode::Busy);
  auto flush = backend.flush("x", 1);  // not circular
  ASSERT_FALSE(flush.ok());
  EXPECT_EQ(flush.error().code, ErrorCode::InvalidArgument);
  EXPECT_TRUE(backend.stop().ok());
}

TEST_F(EtwCaptureTest, LaunchModeIsUnsupported) {
  WindowsEtwBackend backend;
  CaptureOptions o = systemOptions(dir() / "launch.tachyon");
  o.mode = CaptureMode::Launch;
  o.exe = "x.exe";
  auto r = backend.start(o);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
  EXPECT_NE(r.error().message.find("WP-13"), std::string::npos);
}

// ---- no events lost ---------------------------------------------------------------------------------------

TEST_F(EtwCaptureTest, NoEventsLostDuringTestbedSpin) {
  // 5 s here (the work package asks for 30 s on a dev box; see the report for the command).
  constexpr int kSeconds = 5;
  WindowsEtwBackend backend;
  ASSERT_TRUE(backend.start(systemOptions(dir() / "spin.tachyon")).ok());
  Testbed testbed("spin", kSeconds);
  ASSERT_TRUE(testbed.started());
  ASSERT_TRUE(testbed.wait((kSeconds + 20) * 1000)) << "testbed did not finish";
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();

  const WindowsEtwDiagnostics diag = backend.diagnostics();
  report("testbed spin " + std::to_string(kSeconds) + " s, system mode, 8 kHz: " +
         describe(diag.lastCounters) + "; result.eventsLost=" + std::to_string(result->eventsLost) +
         "; convert eventsTotal=" + std::to_string(diag.lastConvertStats.eventsTotal) +
         " samples=" + std::to_string(diag.lastConvertStats.samplesWritten) +
         " cswitch=" + std::to_string(diag.lastConvertStats.cswitchesWritten));
  EXPECT_EQ(diag.lastCounters.eventsLost, 0u);
  EXPECT_EQ(diag.lastCounters.logBuffersLost, 0u);
  EXPECT_EQ(diag.lastCounters.realTimeBuffersLost, 0u);
  EXPECT_EQ(result->eventsLost, 0u);

  auto facts = readFacts(result->tracePath);
  ASSERT_TRUE(facts.ok()) << facts.error().toString();
  const uint64_t testbedSamples = facts->samplesOfPid(testbed.pid());
  report("testbed pid " + std::to_string(testbed.pid()) + " has " + std::to_string(testbedSamples) + " of " +
         std::to_string(facts->samples) + " samples");
  EXPECT_GT(testbedSamples, 5000u) << "a spinning thread for 5 s at 8 kHz";
}

TEST_F(EtwCaptureTest, AttachModeKeepsOnlyTheTargetProcess) {
  Testbed testbed("spin", 3);
  ASSERT_TRUE(testbed.started());
  WindowsEtwBackend backend;
  CaptureOptions o = systemOptions(dir() / "attach.tachyon");
  o.mode = CaptureMode::Attach;
  o.pid = testbed.pid();
  auto started = backend.start(o);
  ASSERT_TRUE(started.ok()) << started.error().toString();
  testbed.wait(20000);
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();

  auto facts = readFacts(result->tracePath);
  ASSERT_TRUE(facts.ok()) << facts.error().toString();
  uint64_t foreign = 0;
  for (const auto& [tid, count] : facts->samplesPerTid) {
    auto it = facts->tidToPid.find(tid);
    if (it == facts->tidToPid.end() || it->second != testbed.pid()) foreign += count;
  }
  report("attach pid " + std::to_string(testbed.pid()) + ": " + std::to_string(facts->samples) +
         " samples, " + std::to_string(foreign) + " outside the target");
  EXPECT_GT(facts->samples, 1000u);
  EXPECT_EQ(foreign, 0u);

  CaptureOptions dead = o;
  dead.pid = deadPid();
  auto bad = backend.start(dead);
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().code, ErrorCode::NotFound);
}

// ---- circular mode ----------------------------------------------------------------------------------------

TEST_F(EtwCaptureTest, CircularFileStaysWithinTheCap) {
  constexpr uint32_t kCapMb = 8;
  CaptureOptions o = systemOptions(dir() / "circ.tachyon", kCapMb);
  auto cfg = etw::makeSessionConfig(o, etw::makeSessionName(etw::EtwSession::currentPid(), 900),
                                    etw::makeSessionGuid(etw::EtwSession::currentPid(), 900, 1),
                                    etw::etlSegmentPath(o.outPath, 0), etw::EtwSession::logicalCoreCount());
  ASSERT_TRUE(cfg.ok()) << cfg.error().toString();
  ASSERT_TRUE(cfg->circular);
  auto session = etw::EtwSession::start(*cfg);
  ASSERT_TRUE(session.ok()) << session.error().toString();

  Load load(2, 2);
  // Run until the session has written several times the cap (so it must have wrapped), at most 25 s.
  const uint64_t targetMb = 4ull * kCapMb;
  uint64_t writtenMb = 0;
  const auto begin = Clock::now();
  while (std::chrono::duration<double>(Clock::now() - begin).count() < 25.0) {
    sleepFor(0.5);
    auto q = (*session)->query();
    ASSERT_TRUE(q.ok()) << q.error().toString();
    writtenMb = q->buffersWritten * q->bufferSizeKb / 1024;
    if (writtenMb >= targetMb) break;
  }
  load.stop();
  auto counters = (*session)->stop();
  ASSERT_TRUE(counters.ok()) << counters.error().toString();
  const uint64_t bytes = fileSize(cfg->etlPath);
  const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
  report("circular cap " + std::to_string(kCapMb) + " MB: wrote ~" + std::to_string(writtenMb) +
         " MB of buffers in " + std::to_string(seconds) + " s, final ETL " + std::to_string(bytes) +
         " bytes; " + describe(*counters));

  EXPECT_GE(writtenMb, 2ull * kCapMb) << "the run was too short to wrap the circular file";
  const uint64_t slack = 2ull * counters->bufferSizeKb * 1024;  // header + one buffer in flight
  EXPECT_LE(bytes, static_cast<uint64_t>(kCapMb) * 1024 * 1024 + slack);
  EXPECT_GT(bytes, 1024u * 1024u) << "the circular file should be nearly full";
}

TEST_F(EtwCaptureTest, FlushKeepsTheLastSecondsAndRestartsTheSession) {
  constexpr uint32_t kWindowS = 2;
  WindowsEtwBackend backend;
  const fs::path out = dir() / "flush.tachyon";
  ASSERT_TRUE(backend.start(systemOptions(out, /*circularMb=*/128)).ok());
  Load load;
  sleepFor(6.0);

  const auto t0 = Clock::now();
  auto flushed = backend.flush("hitch-one", kWindowS);
  const double flushSeconds = std::chrono::duration<double>(Clock::now() - t0).count();
  ASSERT_TRUE(flushed.ok()) << flushed.error().toString();
  EXPECT_EQ(backend.state(), CaptureState::Capturing) << "a flush must not end the capture";
  EXPECT_EQ(flushed->tracePath, dir() / "hitch-one.tachyon");
  EXPECT_TRUE(fs::exists(flushed->tracePath));

  auto facts = readFacts(flushed->tracePath);
  ASSERT_TRUE(facts.ok()) << facts.error().toString();
  const WindowsEtwDiagnostics afterFirst = backend.diagnostics();
  report("flush #1 (window " + std::to_string(kWindowS) + " s after 6 s of capture): sample span " +
         std::to_string(facts->spanSeconds()) + " s, " + std::to_string(facts->samples) +
         " samples, result.durationS " + std::to_string(flushed->durationS) + ", flush() took " +
         std::to_string(flushSeconds) + " s, GAP " + std::to_string(afterFirst.lastFlushGapSeconds * 1000.0) +
         " ms, " + describe(afterFirst.lastCounters));
  EXPECT_GT(facts->samples, 500u);
  EXPECT_GE(facts->spanSeconds(), 1.2);
  EXPECT_LE(facts->spanSeconds(), kWindowS + 0.6)
      << "the flush should keep only the last " << kWindowS << " s of 6 s";
  EXPECT_GE(afterFirst.lastFlushGapSeconds, 0.0);
  EXPECT_LT(afterFirst.lastFlushGapSeconds, 3.0);
  EXPECT_EQ(afterFirst.flushCount, 1u);

  // The restarted session records on: a second flush works and again honours its own window.
  sleepFor(2.5);
  auto second = backend.flush("hitch-two", 1);
  ASSERT_TRUE(second.ok()) << second.error().toString();
  auto facts2 = readFacts(second->tracePath);
  ASSERT_TRUE(facts2.ok()) << facts2.error().toString();
  const WindowsEtwDiagnostics afterSecond = backend.diagnostics();
  report("flush #2 (window 1 s): sample span " + std::to_string(facts2->spanSeconds()) + " s, " +
         std::to_string(facts2->samples) + " samples, GAP " +
         std::to_string(afterSecond.lastFlushGapSeconds * 1000.0) + " ms");
  EXPECT_GE(facts2->spanSeconds(), 0.5);
  EXPECT_LE(facts2->spanSeconds(), 1.6);
  EXPECT_EQ(afterSecond.flushCount, 2u);

  // Samples of the second flush are newer than those of the first.
  EXPECT_GT(facts2->maxTs, facts->maxTs);

  sleepFor(0.5);
  load.stop();
  auto finalResult = backend.stop();
  ASSERT_TRUE(finalResult.ok()) << finalResult.error().toString();
  EXPECT_EQ(finalResult->rawPath, etw::etlSegmentPath(out, 2));
  auto last = readFacts(out);
  ASSERT_TRUE(last.ok()) << last.error().toString();
  EXPECT_GT(last->samples, 100u);
  report("final stop after two flushes: " + std::to_string(last->samples) + " samples over " +
         std::to_string(last->spanSeconds()) + " s; raw " + finalResult->rawPath.filename().string());
  EXPECT_TRUE(fs::exists(dir() / "hitch-one.etl"));
  EXPECT_TRUE(fs::exists(dir() / "hitch-two.etl"));

  // Bad names never reach the session.
  ASSERT_TRUE(backend.start(systemOptions(dir() / "again.tachyon", 64)).ok());
  EXPECT_EQ(backend.flush("../escape", 1).error().code, ErrorCode::InvalidArgument);
  EXPECT_TRUE(backend.stop().ok());
}

// ---- orphan cleanup ---------------------------------------------------------------------------------------

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// A cheap non-kernel real-time session with an arbitrary name, standing in for somebody else's logger.
class ForeignSession {
 public:
  explicit ForeignSession(const std::wstring& name) : m_name(name) {
    const size_t nameBytes = (name.size() + 1) * sizeof(wchar_t);
    m_storage.assign((sizeof(EVENT_TRACE_PROPERTIES) + nameBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t),
                     0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_storage.data());
    p->Wnode.BufferSize = static_cast<ULONG>(m_storage.size() * sizeof(uint64_t));
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;
    p->Wnode.Guid = GUID{0x5a3b1c77u, 0x2d4e, 0x4f10, {0x8a, 0x1b, 0x6c, 0x2e, 0x9d, 0x77, 0x31, 0x42}};
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->LoggerNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES));
    std::memcpy(reinterpret_cast<char*>(p) + p->LoggerNameOffset, name.c_str(), nameBytes);
    m_status = StartTraceW(&m_handle, name.c_str(), p);
  }
  ~ForeignSession() { stop(); }
  ForeignSession(const ForeignSession&) = delete;
  ForeignSession& operator=(const ForeignSession&) = delete;
  bool started() const { return m_status == ERROR_SUCCESS; }
  unsigned long status() const { return m_status; }
  void stop() {
    if (!started() || m_stopped) return;
    m_stopped = true;
    ControlTraceW(m_handle, m_name.c_str(), reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_storage.data()),
                  EVENT_TRACE_CONTROL_STOP);
  }

 private:
  std::wstring m_name;
  std::vector<uint64_t> m_storage;
  TRACEHANDLE m_handle = 0;
  unsigned long m_status = 0xFFFFFFFF;
  bool m_stopped = false;
};

etw::SessionConfig sessionConfigNamed(const std::string& name, const fs::path& etl, uint32_t counter) {
  CaptureOptions o = systemOptions(etl);
  auto cfg = etw::makeSessionConfig(o, name, etw::makeSessionGuid(etw::EtwSession::currentPid(), counter, 3),
                                    etl, etw::EtwSession::logicalCoreCount());
  EXPECT_TRUE(cfg.ok());
  return cfg.ok() ? *cfg : etw::SessionConfig{};
}

TEST_F(EtwCaptureTest, OrphanCleanupStopsOnlyTachyonSessionsOfDeadProcesses) {
  const uint32_t dead = deadPid();
  ASSERT_NE(dead, 0u);
  ASSERT_FALSE(etw::EtwSession::processAlive(dead));
  ASSERT_TRUE(etw::EtwSession::processAlive(etw::EtwSession::currentPid()));

  // 1. A "crashed agent": a Tachyon session whose pid is dead. The object is leaked on purpose (released
  //    without stop) the way a crash would leave it.
  const std::string orphanName = etw::makeSessionName(dead, 0);
  auto orphan = etw::EtwSession::start(sessionConfigNamed(orphanName, dir() / "orphan.etl", 701));
  ASSERT_TRUE(orphan.ok()) << orphan.error().toString();
  // 2. A live sibling: a Tachyon session of this (running) process.
  const std::string liveName = etw::makeSessionName(etw::EtwSession::currentPid(), 4242);
  auto live = etw::EtwSession::start(sessionConfigNamed(liveName, dir() / "live.etl", 702));
  ASSERT_TRUE(live.ok()) << live.error().toString();
  // 3. Somebody else's session with a dead pid in its name.
  const std::wstring foreignName = L"Contoso " + std::to_wstring(dead);
  ForeignSession foreign(foreignName);
  ASSERT_TRUE(foreign.started()) << "StartTrace for the foreign session failed with " << foreign.status();

  auto before = etw::EtwSession::listSessions();
  ASSERT_TRUE(before.ok()) << before.error().toString();
  EXPECT_TRUE(contains(*before, orphanName));
  EXPECT_TRUE(contains(*before, liveName));
  EXPECT_TRUE(contains(*before, "Contoso " + std::to_string(dead)));

  auto report1 = etw::EtwSession::cleanupOrphans();
  ASSERT_TRUE(report1.ok()) << report1.error().toString();
  report("orphan cleanup: inspected " + std::to_string(report1->inspected) + " sessions, stopped " +
         std::to_string(report1->stopped.size()) + ", skipped alive " +
         std::to_string(report1->skippedAlive.size()) + ", failed " + std::to_string(report1->failed.size()));
  EXPECT_TRUE(contains(report1->stopped, orphanName));
  EXPECT_FALSE(contains(report1->stopped, liveName));
  EXPECT_TRUE(contains(report1->skippedAlive, liveName));
  EXPECT_TRUE(report1->failed.empty());
  for (const std::string& s : report1->stopped)
    EXPECT_EQ(s.rfind("Tachyon ", 0), 0u) << "stopped a non-Tachyon session: " << s;

  auto after = etw::EtwSession::listSessions();
  ASSERT_TRUE(after.ok());
  EXPECT_FALSE(contains(*after, orphanName)) << "the orphan is still running";
  EXPECT_TRUE(contains(*after, liveName)) << "a live sibling's session must survive";
  EXPECT_TRUE(contains(*after, "Contoso " + std::to_string(dead))) << "a non-Tachyon session must survive";

  // The orphan's EtwSession object now refers to a session that is gone: stopping reports NotFound, not a
  // crash.
  auto stale = (*orphan)->stop();
  EXPECT_FALSE(stale.ok());
  EXPECT_TRUE((*live)->stop().ok());
  foreign.stop();
}

TEST_F(EtwCaptureTest, BackendRestartsAfterACrashedAgentLeftASessionBehind) {
  const uint32_t dead = deadPid();
  ASSERT_NE(dead, 0u);
  // Simulated crashed agent: a live kernel session named like ours, owned by a dead pid.
  const std::string orphanName = etw::makeSessionName(dead, 0);
  auto orphan = etw::EtwSession::start(sessionConfigNamed(orphanName, dir() / "crashed.etl", 703));
  ASSERT_TRUE(orphan.ok()) << orphan.error().toString();

  WindowsEtwBackend backend;  // cleanupOrphans defaults to true
  auto started = backend.start(systemOptions(dir() / "restart.tachyon"));
  ASSERT_TRUE(started.ok()) << started.error().toString();
  const WindowsEtwDiagnostics diag = backend.diagnostics();
  EXPECT_TRUE(contains(diag.lastOrphanReport.stopped, orphanName)) << "the stale session was not cleaned up";

  auto sessions = etw::EtwSession::listSessions();
  ASSERT_TRUE(sessions.ok());
  EXPECT_FALSE(contains(*sessions, orphanName));
  EXPECT_TRUE(contains(*sessions, diag.sessionName));

  Load load;
  sleepFor(1.0);
  load.stop();
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  auto facts = readFacts(result->tracePath);
  ASSERT_TRUE(facts.ok()) << facts.error().toString();
  report("restart after crashed agent: stopped orphans " +
         std::to_string(diag.lastOrphanReport.stopped.size()) + ", new capture produced " +
         std::to_string(facts->samples) + " samples");
  EXPECT_GT(facts->samples, 100u);
}

TEST_F(EtwCaptureTest, SessionNamesAreUniqueWhenTwoBackendsRunInOneProcess) {
  // Two backend objects in one process share the pid; the counter retry must keep their names apart. (Two
  // kernel system loggers can coexist: up to 8.)
  WindowsEtwBackend a, b;
  ASSERT_TRUE(a.start(systemOptions(dir() / "a.tachyon")).ok());
  auto bStart = b.start(systemOptions(dir() / "b.tachyon"));
  ASSERT_TRUE(bStart.ok()) << bStart.error().toString();
  EXPECT_NE(a.diagnostics().sessionName, b.diagnostics().sessionName);
  EXPECT_TRUE(a.stop().ok());
  EXPECT_TRUE(b.stop().ok());
}

// ---- the agent end to end ---------------------------------------------------------------------------------

TEST_F(EtwCaptureTest, AgentDrivesTheEtwBackendOverTheControlPipe) {
  const std::string address = R"(\\.\pipe\tachyon-etw-test-)" + std::to_string(GetCurrentProcessId());
  agent::AgentConfig ac;
  ac.backendName = "etw";
  ac.agentAddress = address;
  ac.elevated = true;
  agent::Agent core(std::make_unique<WindowsEtwBackend>(), ac);
  auto listener = control::listenOn(address);
  ASSERT_TRUE(listener.ok()) << listener.error().toString();
  control::Server server(std::move(*listener), core);
  core.setEventSink([&server](const control::Event& e) { server.broadcast(e); });
  server.start();

  auto client = control::Client::connectWithRetry(address, 5000);
  ASSERT_TRUE(client.ok()) << client.error().toString();
  control::StartRequest start;
  start.mode = CaptureMode::System;
  start.out_path = (dir() / "agent.tachyon").string();
  auto r = client->call(start, 30000);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  ASSERT_TRUE(r->ok) << r->error;

  Load load;
  sleepFor(2.0);
  load.stop();
  auto stop = client->call(control::StopRequest{}, 30000);
  ASSERT_TRUE(stop.ok() && stop->ok);
  auto done = client->waitForEvent(
      [](const control::Event& e) { return std::holds_alternative<control::CaptureDoneEvent>(e); }, 120000);
  ASSERT_TRUE(done.ok()) << done.error().toString();
  const auto& summary = std::get<control::CaptureDoneEvent>(*done).summary;
  report("agent end to end: " + summary.path + ", events_lost " + std::to_string(summary.events_lost) +
         ", duration " + std::to_string(summary.duration_s) + " s");
  EXPECT_EQ(summary.events_lost, 0u);
  auto facts = readFacts(summary.path);
  ASSERT_TRUE(facts.ok()) << facts.error().toString();
  EXPECT_GT(facts->samples, 1000u);
  server.stop();
  core.shutdown();
}

}  // namespace
}  // namespace tachyon::capture

#endif  // TACHYON_PLATFORM_WINDOWS
