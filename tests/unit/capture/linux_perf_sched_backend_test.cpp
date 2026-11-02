// WP-71: LinuxPerfBackend and the scheduler events over the fake perf layer: which events are opened next to
// the sampling events (tracepoints with their ids, SET_OUTPUT into the cpu's ring, the RAW sample type), the
// context_switch fallback when tracefs is missing or the tracepoint open is refused, the start options that
// turn the events off, and an end-to-end run from fake rings to ContextSwitchRec / ReadyRec.
#include "capture/linux_perf/linux_perf_backend.h"
#include "linux_perf_test_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;

using namespace perf;
using perf::ptest::FakePerfSystem;
using perf::ptest::TempDir;

uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

std::string readFixture(const std::string& name) {
  std::ifstream in(fs::path(TACHYON_FIXTURE_DIR) / "perf" / "sched" / name);
  EXPECT_TRUE(in.good()) << name;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A fake perf layer whose tracepoint events can be refused.
class RefusingSystem : public FakePerfSystem {
 public:
  int refuseTracepointsWith = 0;
  OpenOutcome openEvent(const EventConfig& config, int pid, int cpu) override {
    if (config.kind == EventConfig::Kind::Tracepoint && refuseTracepointsWith != 0) {
      OpenOutcome out;
      out.err = refuseTracepointsWith;
      return out;
    }
    return FakePerfSystem::openEvent(config, pid, cpu);
  }
};

std::vector<uint8_t> switchPayload(uint32_t prevPid, int64_t prevState, uint32_t nextPid) {
  std::vector<uint8_t> b(64, 0);
  const uint16_t type = 316;
  std::memcpy(b.data(), &type, 2);
  std::memcpy(b.data() + 24, &prevPid, 4);
  const int32_t prio = 120;
  std::memcpy(b.data() + 28, &prio, 4);
  std::memcpy(b.data() + 32, &prevState, 8);
  std::memcpy(b.data() + 56, &nextPid, 4);
  std::memcpy(b.data() + 60, &prio, 4);
  return b;
}

std::vector<uint8_t> wakePayload(uint32_t pid) {
  std::vector<uint8_t> b(36, 0);
  const uint16_t type = 317;
  std::memcpy(b.data(), &type, 2);
  std::memcpy(b.data() + 24, &pid, 4);
  return b;
}

class SchedBackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_fake = std::make_shared<RefusingSystem>();
    m_fake->proc = m_dir.path() / "proc";  // an empty /proc: no rundown records
    fs::create_directories(m_fake->proc);
    m_config.system = m_fake;
    m_config.drainInterval = std::chrono::milliseconds(2);
  }
  void TearDown() override {
    for (uint32_t pid : m_toKill) {
      ::kill(static_cast<pid_t>(pid), SIGKILL);
      int st = 0;
      ::waitpid(static_cast<pid_t>(pid), &st, 0);
    }
  }

  // A tracefs tree with the sched events the tests need.
  void installTracefs(bool waking = true, bool narrow = false) {
    m_tracefs = m_dir.path() / "tracefs";
    auto put = [&](const std::string& event, const std::string& fixtureName) {
      fs::create_directories(m_tracefs / "events" / "sched" / event);
      std::ofstream(m_tracefs / "events" / "sched" / event / "format") << readFixture(fixtureName);
    };
    put("sched_switch", narrow ? "sched_switch_narrow_state.format" : "sched_switch_wide_state.format");
    if (waking) put("sched_waking", "sched_waking.format");
    m_config.tracefsRoot = m_tracefs;
  }

  CaptureOptions systemOptions() {
    CaptureOptions o;
    o.mode = CaptureMode::System;
    o.outPath = m_dir.path() / "capture.tachyon";
    o.samplingHz = 8000;
    return o;
  }

  TempDir m_dir;
  std::shared_ptr<RefusingSystem> m_fake;
  LinuxPerfBackendConfig m_config;
  fs::path m_tracefs;
  std::vector<uint32_t> m_toKill;
};

TEST_F(SchedBackendTest, TracepointEventsSitNextToEveryCpuEventAndWriteIntoItsRing) {
  installTracefs();
  LinuxPerfBackend backend(m_config);
  ASSERT_TRUE(backend.start(systemOptions()).ok());
  EXPECT_EQ(backend.diagnostics().schedPath, "tracepoint");
  EXPECT_EQ(backend.diagnostics().rings, 2u) << "one ring per cpu: the tracepoint events share it";
  const auto events = m_fake->events();
  ASSERT_EQ(events.size(), 6u);  // per cpu: cpu-clock, sched_switch, sched_waking
  for (size_t cpuIndex = 0; cpuIndex < 2; ++cpuIndex) {
    const auto& clock = events[cpuIndex * 3];
    const auto& sw = events[cpuIndex * 3 + 1];
    const auto& wake = events[cpuIndex * 3 + 2];
    EXPECT_EQ(clock.config.kind, EventConfig::Kind::CpuClock);
    EXPECT_EQ(sw.config.kind, EventConfig::Kind::Tracepoint);
    EXPECT_EQ(sw.config.tracepointId, 316u);
    EXPECT_EQ(wake.config.kind, EventConfig::Kind::Tracepoint);
    EXPECT_EQ(wake.config.tracepointId, 317u);
    for (const auto* e : {&clock, &sw, &wake}) {
      EXPECT_EQ(e->pid, -1);
      EXPECT_EQ(e->cpu, static_cast<int>(m_fake->cpus[cpuIndex]));
      EXPECT_EQ(e->config.sampleType, kSchedSampleType) << "one layout for the whole ring";
      EXPECT_TRUE(e->enabled);
      EXPECT_FALSE(e->config.contextSwitch);
    }
    EXPECT_EQ(clock.outputTarget, -1);
    EXPECT_EQ(sw.outputTarget, clock.fd) << "redirected into the cpu's ring (PERF_EVENT_IOC_SET_OUTPUT)";
    EXPECT_EQ(wake.outputTarget, clock.fd);
  }
  ASSERT_TRUE(backend.stop().ok());
}

TEST_F(SchedBackendTest, StartOptionsSelectTheEvents) {
  installTracefs();
  {
    LinuxPerfBackend backend(m_config);
    CaptureOptions o = systemOptions();
    o.stackReady = false;
    ASSERT_TRUE(backend.start(o).ok());
    EXPECT_EQ(m_fake->events().size(), 4u) << "cpu-clock + sched_switch per cpu";
    ASSERT_TRUE(backend.stop().ok());
  }
  m_fake = std::make_shared<RefusingSystem>();
  m_fake->proc = m_dir.path() / "proc";
  m_config.system = m_fake;
  {
    LinuxPerfBackend backend(m_config);
    CaptureOptions o = systemOptions();
    o.stackCSwitch = false;
    ASSERT_TRUE(backend.start(o).ok());
    const auto events = m_fake->events();
    ASSERT_EQ(events.size(), 4u) << "cpu-clock + sched_waking per cpu";
    EXPECT_EQ(events[1].config.tracepointId, 317u);
    ASSERT_TRUE(backend.stop().ok());
  }
  m_fake = std::make_shared<RefusingSystem>();
  m_fake->proc = m_dir.path() / "proc";
  m_config.system = m_fake;
  {
    LinuxPerfBackend backend(m_config);
    CaptureOptions o = systemOptions();
    o.stackCSwitch = false;
    o.stackReady = false;
    ASSERT_TRUE(backend.start(o).ok());
    const auto events = m_fake->events();
    EXPECT_EQ(events.size(), 2u) << "sampling only";
    for (const auto& e : events) {
      EXPECT_FALSE(e.config.contextSwitch);
      EXPECT_EQ(e.config.sampleType, kBackendSampleType) << "no RAW without tracepoints";
    }
    EXPECT_EQ(backend.diagnostics().schedPath, "none");
    ASSERT_TRUE(backend.stop().ok());
  }
}

TEST_F(SchedBackendTest, PerThreadPlanUsesTaskBoundSwitchRecordsInsteadOfTracepoints) {
  // A task-bound sched_switch event sees only the switches out of its task, so the per-thread plan records
  // PERF_RECORD_SWITCH (out and in) and says why.
  installTracefs();
  m_config.forcePerThread = true;
  m_fake->sys = {2, 100000, 25, 516, false};  // unprivileged: per-thread events
  LinuxPerfBackend backend(m_config);
  CaptureOptions o;
  o.mode = CaptureMode::Launch;
  o.exe = "/bin/sleep";
  o.args = {"30"};
  o.outPath = m_dir.path() / "capture.tachyon";
  ASSERT_TRUE(backend.start(o).ok());
  m_toKill.push_back(backend.rootPid());
  EXPECT_EQ(backend.diagnostics().plan, "per-thread");
  EXPECT_EQ(backend.diagnostics().schedPath, "context_switch");
  const auto events = m_fake->events();
  ASSERT_EQ(events.size(), 2u) << "one event per cpu: the switch records come from the sampling event";
  for (const auto& e : events) {
    EXPECT_EQ(e.config.kind, EventConfig::Kind::CpuClock);
    EXPECT_TRUE(e.config.contextSwitch);
    EXPECT_TRUE(e.config.inherit);
    EXPECT_TRUE(e.config.enableOnExec);
    EXPECT_EQ(e.pid, static_cast<int>(backend.rootPid()));
    EXPECT_EQ(e.config.sampleType, kBackendSampleType);
  }
  bool explained = false;
  for (const std::string& w : backend.diagnostics().warnings)
    explained |= w.find("per-thread plan cannot record scheduler tracepoints") != std::string::npos;
  EXPECT_TRUE(explained);
  ASSERT_TRUE(backend.stop().ok());
}

TEST_F(SchedBackendTest, SystemWideRefusalDemotesTheSchedPlanToo) {
  installTracefs();
  m_fake->failSystemWideWith = EACCES;
  m_fake->sys = {2, 100000, 25, 516, false};
  LinuxPerfBackend backend(m_config);
  CaptureOptions o;
  o.mode = CaptureMode::Launch;
  o.exe = "/bin/sleep";
  o.args = {"30"};
  o.outPath = m_dir.path() / "capture.tachyon";
  ASSERT_TRUE(backend.start(o).ok());
  m_toKill.push_back(backend.rootPid());
  EXPECT_EQ(backend.diagnostics().plan, "per-thread");
  EXPECT_EQ(backend.diagnostics().schedPath, "context_switch");
  size_t live = 0;
  for (const auto& e : m_fake->events()) {
    if (e.closed) continue;
    ++live;
    EXPECT_EQ(e.config.kind, EventConfig::Kind::CpuClock);
    EXPECT_TRUE(e.config.contextSwitch);
  }
  EXPECT_EQ(live, 2u);
  ASSERT_TRUE(backend.stop().ok());
}

TEST_F(SchedBackendTest, EndToEndFromRingsToSwitchesAndReadyEvents) {
  installTracefs();
  LinuxPerfBackend backend(m_config);
  CaptureOptions o = systemOptions();
  ASSERT_TRUE(backend.start(o).ok());
  const uint64_t t0 = nowNs();
  // cpu 1's ring (event 3): the waiter (tid 101 of process 100) sleeps; cpu 0's ring (event 0): the holder
  // (tid 100) wakes it; cpu 1 again: the idle task hands the cpu back to the waiter.
  RecordBuilder b1(RecordLayout{kSchedSampleType, true}), b0(RecordLayout{kSchedSampleType, true});
  RecordBuilder::Sample s;
  s.misc = kMiscKernel;
  s.id = {100, 101, t0 + 1000, 1, 0, 0};
  s.chain = {kContextKernel, 0xffffffff81100000ull, kContextUser, 0x401100ull};
  s.ip = s.chain[1];
  s.raw = switchPayload(101, 1, 0);
  b1.sample(s);
  s.id = {0, 0, t0 + 2100, 1, 0, 0};
  s.chain = {kContextKernel, 0xffffffff81100000ull};
  s.raw = switchPayload(0, 0, 101);
  b1.sample(s);
  s.id = {100, 100, t0 + 2000, 0, 0, 0};
  s.chain = {kContextKernel, 0xffffffff81200000ull, kContextUser, 0x402200ull};
  s.ip = s.chain[1];
  s.raw = wakePayload(101);
  b0.sample(s);
  m_fake->ring(3)->push(b1.bytes());
  m_fake->ring(0)->push(b0.bytes());
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_TRUE(result->warnings.empty()) << (result->warnings.empty() ? "" : result->warnings[0]);

  auto reader = trace::TraceReader::open(o.outPath);
  ASSERT_TRUE(reader.ok());
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  ASSERT_EQ(sw->size(), 2u);
  EXPECT_EQ((*sw)[0].old_tid, 101u);
  EXPECT_EQ((*sw)[0].old_state, 5);
  EXPECT_EQ((*sw)[0].old_wait_reason, 1);
  EXPECT_EQ((*sw)[1].new_tid, 101u);
  auto rd = reader->readReadyEvents();
  ASSERT_TRUE(rd.ok());
  ASSERT_EQ(rd->size(), 1u);
  EXPECT_EQ((*rd)[0].readied_tid, 101u);
  EXPECT_EQ((*rd)[0].readying_tid, 100u);
  const auto doc = nlohmann::json::parse(*reader->readCaptureInfo());
  EXPECT_EQ(doc["sched"]["path"], "tracepoint");
  EXPECT_EQ(doc["capture_options"]["sched_path"], "tracepoint");

  // The spool is self-contained: its session carries the layout and the formats.
  auto set = SpoolSet::openDirectory(result->rawPath);
  ASSERT_TRUE(set.ok());
  EXPECT_EQ(set->info.layout.sampleType, kSchedSampleType);
  EXPECT_EQ(set->info.sched.mode, "tracepoint");
  EXPECT_NE(set->info.sched.switchFormat.find("sched_switch"), std::string::npos);
}

TEST_F(SchedBackendTest, WithoutTracefsTheSamplingEventsAskForSwitchRecordsAndTheCaptureSaysSo) {
  LinuxPerfBackend backend(m_config);  // the fake reports no tracefs
  CaptureOptions o = systemOptions();
  ASSERT_TRUE(backend.start(o).ok());
  EXPECT_EQ(backend.diagnostics().schedPath, "context_switch");
  const auto events = m_fake->events();
  ASSERT_EQ(events.size(), 2u);
  for (const auto& e : events) {
    EXPECT_EQ(e.config.kind, EventConfig::Kind::CpuClock);
    EXPECT_TRUE(e.config.contextSwitch);
    EXPECT_EQ(e.config.sampleType, kBackendSampleType) << "no RAW area without tracepoints";
  }
  // cpu 0: the waiter sleeps and the idle task runs; cpu-wide records name both tasks.
  const uint64_t t0 = nowNs();
  RecordBuilder b(RecordLayout{kBackendSampleType, true});
  b.switchCpuWide(true, 0, 0, {100, 101, t0 + 1000, 0, 0, 0});
  b.switchCpuWide(false, 100, 101, {0, 0, t0 + 1000, 0, 0, 0});
  b.switchCpuWide(true, 100, 101, {0, 0, t0 + 2000, 0, 0, 0});
  b.switchCpuWide(false, 0, 0, {100, 101, t0 + 2000, 0, 0, 0});
  m_fake->ring(0)->push(b.bytes());
  auto result = backend.stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  bool explained = false;
  for (const std::string& w : result->warnings)
    explained |=
        w.find("PERF_RECORD_SWITCH") != std::string::npos && w.find("wait reasons") != std::string::npos;
  EXPECT_TRUE(explained);
  auto reader = trace::TraceReader::open(o.outPath);
  auto sw = reader->readContextSwitches();
  ASSERT_TRUE(sw.ok());
  ASSERT_EQ(sw->size(), 2u);
  EXPECT_EQ((*sw)[0].old_tid, 101u);
  EXPECT_EQ((*sw)[0].new_tid, 0u);
  EXPECT_EQ((*sw)[0].old_state, 5);
  EXPECT_EQ((*sw)[1].new_tid, 101u);
  const auto doc = nlohmann::json::parse(*reader->readCaptureInfo());
  EXPECT_EQ(doc["sched"]["path"], "context_switch");
  EXPECT_FALSE(doc["sched"]["fallback_reason"].get<std::string>().empty());
}

TEST_F(SchedBackendTest, ARefusedTracepointOpenFallsBackAndStartsFromACleanSlate) {
  installTracefs();
  m_fake->refuseTracepointsWith = EACCES;
  LinuxPerfBackend backend(m_config);
  ASSERT_TRUE(backend.start(systemOptions()).ok());
  EXPECT_EQ(backend.diagnostics().schedPath, "context_switch");
  size_t live = 0;
  for (const auto& e : m_fake->events()) {
    if (e.closed) continue;
    ++live;
    EXPECT_EQ(e.config.kind, EventConfig::Kind::CpuClock);
    EXPECT_TRUE(e.config.contextSwitch);
    EXPECT_EQ(e.config.sampleType, kBackendSampleType);
  }
  EXPECT_EQ(live, 2u) << "the first attempt's events were closed";
  auto result = backend.stop();
  ASSERT_TRUE(result.ok());
  bool explained = false;
  for (const std::string& w : result->warnings)
    explained |= w.find("opening the tracepoint events failed") != std::string::npos;
  EXPECT_TRUE(explained);
}

TEST_F(SchedBackendTest, TheWakeupTracepointAloneMissingKeepsSwitchTracepoints) {
  installTracefs(/*waking=*/false);
  LinuxPerfBackend backend(m_config);
  ASSERT_TRUE(backend.start(systemOptions()).ok());
  EXPECT_EQ(backend.diagnostics().schedPath, "tracepoint");
  EXPECT_EQ(m_fake->events().size(), 4u);
  auto result = backend.stop();
  ASSERT_TRUE(result.ok());
  bool warned = false;
  for (const std::string& w : result->warnings) warned |= w.find("ready-thread") != std::string::npos;
  EXPECT_TRUE(warned);
}

}  // namespace
}  // namespace tachyon::capture
