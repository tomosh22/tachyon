// WP-70: LinuxPerfBackend over a fake perf layer (no privileges needed): plans, the launch handshake
// ordering, parked launches, failure cleanup, attach, target exit detection, lost records, circular flush.
// Real processes are launched (/bin/sleep, /bin/sh); the kernel side is simulated by pushing perf records
// into the fake rings.
#include "capture/linux_perf/linux_perf_backend.h"
#include "capture/linux_perf/proc_snapshot.h"
#include "linux_perf_test_util.h"
#include "model/model.h"
#include "symbols/dwarf/elf_reader.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <set>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;

using namespace perf;
using perf::ptest::FakePerfSystem;
using perf::ptest::TempDir;
using perf::ptest::waitFor;

std::string linkOf(const std::string& path) {
  char buf[4096];
  const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

std::string selfExe() {
  return linkOf("/proc/self/exe");
}

// Pids of the direct children of this process that are still a copy of this program (parked, pre-exec).
std::vector<uint32_t> parkedChildren() {
  std::vector<uint32_t> out;
  for (const auto& e : fs::directory_iterator("/proc")) {
    const std::string name = e.path().filename().string();
    if (name.empty() || name[0] < '0' || name[0] > '9') continue;
    const uint32_t pid = static_cast<uint32_t>(std::stoul(name));
    std::ifstream stat(e.path() / "stat");
    std::string text((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    ProcStat st;
    if (!parseProcStat(text, st) || st.ppid != static_cast<uint32_t>(::getpid())) continue;
    if (linkOf("/proc/" + name + "/exe") == selfExe()) out.push_back(pid);
  }
  return out;
}

void killAndReap(uint32_t pid) {
  ::kill(static_cast<pid_t>(pid), SIGKILL);
  int st = 0;
  ::waitpid(static_cast<pid_t>(pid), &st, 0);
}

uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

class BackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_fake = std::make_shared<FakePerfSystem>();
    m_config.system = m_fake;
    m_config.drainInterval = std::chrono::milliseconds(2);
  }
  void TearDown() override {
    for (uint32_t pid : m_toKill) killAndReap(pid);
  }

  std::unique_ptr<LinuxPerfBackend> make() { return std::make_unique<LinuxPerfBackend>(m_config); }

  CaptureOptions launchOptions(std::vector<std::string> args = {"30"},
                               const std::string& exe = "/bin/sleep") {
    CaptureOptions o;
    o.mode = CaptureMode::Launch;
    o.exe = exe;
    o.args = std::move(args);
    o.outPath = m_dir.path() / "capture.tachyon";
    o.samplingHz = 8000;
    return o;
  }

  // Pushes records to ring `n` of the fake.
  void push(size_t n, const RecordBuilder& b) {
    ASSERT_NE(m_fake->ring(n), nullptr);
    m_fake->ring(n)->push(b.bytes());
  }

  // A sample of `pid` at `ts` in the user mode with the given chain.
  static void addSample(RecordBuilder& b, uint32_t pid, uint64_t ts, std::vector<uint64_t> chain,
                        uint32_t cpu = 0) {
    RecordBuilder::Sample s;
    s.id = {pid, pid, ts, cpu, 0, 0};
    s.chain = std::move(chain);
    for (uint64_t v : s.chain)
      if (v < kContextMax) {
        s.ip = v;
        break;
      }
    b.sample(s);
  }

  TempDir m_dir;
  std::shared_ptr<FakePerfSystem> m_fake;
  LinuxPerfBackendConfig m_config;
  std::vector<uint32_t> m_toKill;
};

TEST_F(BackendTest, CheckPrivilegesProbesTheKernel) {
  auto backend = make();
  EXPECT_TRUE(backend->checkPrivileges().ok());
  ASSERT_EQ(m_fake->events().size(), 1u);
  EXPECT_TRUE(m_fake->events()[0].closed) << "the probe event is closed again";
  EXPECT_EQ(m_fake->events()[0].pid, -1);

  m_fake->failSystemWideWith = EACCES;
  m_fake->sys.paranoid = 4;
  auto denied = backend->checkPrivileges();
  ASSERT_FALSE(denied.ok());
  EXPECT_EQ(denied.error().code, ErrorCode::NoPrivilege);
  EXPECT_NE(denied.error().message.find("kernel.perf_event_paranoid is 4"), std::string::npos);
  EXPECT_NE(denied.error().message.find("CAP_PERFMON"), std::string::npos);
}

TEST_F(BackendTest, SamplingRateAboveTheKernelLimitFailsBeforeAnythingIsOpened) {
  m_fake->sys.maxSampleRate = 1000;
  auto backend = make();
  CaptureOptions o = launchOptions();
  o.samplingHz = 8000;
  auto r = backend->start(o);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  EXPECT_NE(r.error().message.find("kernel.perf_event_max_sample_rate"), std::string::npos);
  EXPECT_TRUE(m_fake->events().empty());
  EXPECT_EQ(backend->state(), CaptureState::Idle);
  EXPECT_TRUE(parkedChildren().empty()) << "the parked child of the failed start was killed";
  EXPECT_FALSE(fs::exists(fs::path(o.outPath.string() + ".perfspool"))) << "no half-made spool left behind";
}

TEST_F(BackendTest, LaunchSystemWideOrdering) {
  // The events must be open and enabled while the child is still parked; the exec happens afterwards.
  std::mutex mu;
  std::vector<std::string> observations;
  m_fake->hook = [&](const std::string& what) {
    const bool parked = !parkedChildren().empty();
    std::lock_guard lock(mu);
    observations.push_back(what + (parked ? " [child parked]" : " [no parked child]"));
  };
  auto backend = make();
  ASSERT_TRUE(backend->start(launchOptions()).ok());
  const uint32_t pid = backend->rootPid();
  ASSERT_GT(pid, 0u);
  m_toKill.push_back(pid);

  {
    std::lock_guard lock(mu);
    ASSERT_GE(observations.size(), 4u);
    for (const std::string& o : observations)
      if (o.rfind("open", 0) == 0 || o.rfind("enable", 0) == 0)
        EXPECT_NE(o.find("[child parked]"), std::string::npos) << "events set up before the exec: " << o;
  }
  EXPECT_NE(linkOf("/proc/" + std::to_string(pid) + "/exe"), selfExe())
      << "start() returns once the target runs";
  EXPECT_EQ(backend->state(), CaptureState::Capturing);
  EXPECT_FALSE(backend->launchPending());
  EXPECT_FALSE(backend->targetExited());

  // One event per cpu, system wide, enabled after opening.
  const auto events = m_fake->events();
  ASSERT_EQ(events.size(), m_fake->cpus.size());
  for (size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i].pid, -1);
    EXPECT_EQ(events[i].cpu, static_cast<int>(m_fake->cpus[i]));
    EXPECT_TRUE(events[i].enabled);
    EXPECT_FALSE(events[i].config.inherit);
    EXPECT_FALSE(events[i].config.excludeKernel);
    EXPECT_FALSE(events[i].config.excludeCallchainKernel) << "kernel_stacks is on by default";
    EXPECT_EQ(events[i].config.samplingHz, 8000u);
    EXPECT_EQ(events[i].config.sampleType, kBackendSampleType);
    EXPECT_GT(events[i].config.wakeupBytes, 0u);
  }
  EXPECT_EQ(backend->diagnostics().plan, "system-wide");
  EXPECT_TRUE(fs::exists(fs::path(launchOptions().outPath.string() + ".perfspool") / "session.json"));
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, KernelStacksOffExcludesTheKernelCallchain) {
  auto backend = make();
  CaptureOptions o = launchOptions();
  o.kernelStacks = false;
  ASSERT_TRUE(backend->start(o).ok());
  m_toKill.push_back(backend->rootPid());
  for (const auto& e : m_fake->events()) EXPECT_TRUE(e.config.excludeCallchainKernel);
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, PerThreadLaunchEnablesOnExecAndNeverEnablesItself) {
  m_config.forcePerThread = true;
  m_fake->sys = {2, 100000, 25, 516, false};  // unprivileged
  auto backend = make();
  ASSERT_TRUE(backend->start(launchOptions()).ok());
  const uint32_t pid = backend->rootPid();
  m_toKill.push_back(pid);
  // An inherited per-task event cannot be mmapped (kernel: EINVAL), so the child gets one event per cpu, each
  // with its own ring.
  const auto events = m_fake->events();
  ASSERT_EQ(events.size(), m_fake->cpus.size());
  for (size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i].pid, static_cast<int>(pid));
    EXPECT_EQ(events[i].cpu, static_cast<int>(m_fake->cpus[i]));
    EXPECT_TRUE(events[i].config.inherit);
    EXPECT_TRUE(events[i].config.enableOnExec) << "the kernel starts the event at the child's exec";
    EXPECT_TRUE(events[i].config.disabled);
    EXPECT_FALSE(events[i].enabled) << "no explicit enable: the kernel does it at the exec";
    EXPECT_TRUE(events[i].config.excludeKernel) << "paranoid 2 without CAP_PERFMON: user samples only";
    EXPECT_TRUE(events[i].config.excludeCallchainKernel);
    EXPECT_EQ(events[i].outputTarget, -1);
  }
  const auto diag = backend->diagnostics();
  EXPECT_EQ(diag.plan, "per-thread");
  EXPECT_TRUE(diag.excludeKernel);
  ASSERT_FALSE(diag.warnings.empty());
  EXPECT_NE(diag.warnings[0].find("kernel.perf_event_paranoid is 2"), std::string::npos) << diag.warnings[0];
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, SystemWideRefusalFallsBackToPerThreadForLaunchButNotForSystemMode) {
  m_fake->failSystemWideWith = EACCES;
  m_fake->sys = {2, 100000, 25, 516, false};
  {
    auto backend = make();
    ASSERT_TRUE(backend->start(launchOptions()).ok());
    m_toKill.push_back(backend->rootPid());
    EXPECT_EQ(backend->diagnostics().plan, "per-thread");
    bool warned = false;
    for (const auto& w : backend->diagnostics().warnings)
      warned |= w.find("system-wide sampling is not permitted") != std::string::npos;
    EXPECT_TRUE(warned);
    ASSERT_TRUE(backend->stop().ok());
  }
  {
    auto backend = make();
    CaptureOptions o = launchOptions();
    o.mode = CaptureMode::System;
    auto r = backend->start(o);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::NoPrivilege);
    EXPECT_NE(r.error().message.find("kernel.perf_event_paranoid"), std::string::npos);
    EXPECT_EQ(backend->state(), CaptureState::Idle);
    EXPECT_FALSE(fs::exists(fs::path(o.outPath.string() + ".perfspool")));
    EXPECT_TRUE(parkedChildren().empty());
  }
}

TEST_F(BackendTest, FallbackAfterAPartialSystemWideOpenStartsFromACleanSlate) {
  m_fake->cpus = {0, 1, 2};
  m_fake->sys = {2, 100000, 25, 516, false};
  int opens = 0;
  m_fake->hook = [&](const std::string& what) {
    // The second cpu is refused: the first event was already opened and mapped.
    if (what.rfind("open pid=-1", 0) == 0 && ++opens == 2) m_fake->failSystemWideWith = EACCES;
  };
  auto backend = make();
  ASSERT_TRUE(backend->start(launchOptions()).ok());
  m_toKill.push_back(backend->rootPid());
  EXPECT_EQ(backend->diagnostics().plan, "per-thread");
  EXPECT_EQ(backend->diagnostics().rings, 3u);
  size_t live = 0;
  for (const auto& e : m_fake->events()) {
    if (e.closed) continue;
    ++live;
    EXPECT_GE(e.pid, 0) << "only the per-thread events are alive";
    EXPECT_EQ(e.outputTarget, -1) << "a launch has one task: every event owns its cpu's ring";
  }
  EXPECT_EQ(live, 3u);
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, FailureWhileOpeningClosesWhatWasOpened) {
  m_fake->cpus = {0, 1, 2};
  auto backend = make();
  // Fail the third cpu's event: count opens in the hook and turn the failure on late.
  int opens = 0;
  m_fake->hook = [&](const std::string& what) {
    if (what.rfind("open", 0) == 0 && ++opens == 3) m_fake->failSystemWideWith = EMFILE;
  };
  auto r = backend->start(launchOptions());
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().message.find("ulimit -n"), std::string::npos);
  size_t closed = 0;
  for (const auto& e : m_fake->events()) closed += e.closed ? 1 : 0;
  EXPECT_EQ(closed, m_fake->events().size()) << "every opened event was closed";
  EXPECT_EQ(backend->state(), CaptureState::Idle);
  EXPECT_TRUE(parkedChildren().empty());
  // The backend is usable again.
  m_fake->failSystemWideWith = 0;
  m_fake->hook = nullptr;
  ASSERT_TRUE(backend->start(launchOptions()).ok());
  m_toKill.push_back(backend->rootPid());
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, RingMappingFallsBackToSmallerRingsOnMemlockErrors) {
  // Fail the first two attempts of each ring with EPERM, then succeed: the backend halves the size.
  struct Sys : FakePerfSystem {
    RingOutcome mapRing(int fd, uint32_t pages) override {
      requested.push_back(pages);
      if (pages > 64) {
        RingOutcome out;
        out.err = EPERM;
        return out;
      }
      return FakePerfSystem::mapRing(fd, pages);
    }
    std::vector<uint32_t> requested;
  };
  auto sys = std::make_shared<Sys>();
  m_config.system = sys;
  auto backend = make();
  ASSERT_TRUE(backend->start(launchOptions()).ok());
  m_toKill.push_back(backend->rootPid());
  EXPECT_EQ(backend->diagnostics().ringPages, 64u);
  EXPECT_EQ(sys->requested.front(), 256u);
  ASSERT_TRUE(backend->stop().ok());

  sys->sys.mlockKb = 516;
  struct Always : FakePerfSystem {
    RingOutcome mapRing(int, uint32_t) override {
      RingOutcome out;
      out.err = EPERM;
      return out;
    }
  };
  auto always = std::make_shared<Always>();
  m_config.system = always;
  auto b2 = make();
  auto r = b2->start(launchOptions());
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().message.find("perf_event_mlock_kb"), std::string::npos) << r.error().message;
}

TEST_F(BackendTest, EndToEndThroughTheSpoolToATraceWithRealModuleIdentity) {
  auto backend = make();
  std::vector<std::pair<CaptureState, float>> states;
  backend->setProgressCallback([&](CaptureState s, float p) { states.emplace_back(s, p); });
  CaptureOptions o = launchOptions();
  ASSERT_TRUE(backend->start(o).ok());
  const uint32_t pid = backend->rootPid();
  m_toKill.push_back(pid);

  // The first executable mapping of /bin/sleep in the live process tells us where the PIE image sits.
  std::string sleepPath;
  uint64_t lowest = ~0ull, execAddr = 0, execLen = 0, execOff = 0, inode = 0;
  {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
      MapsLine m;
      if (!parseMapsLine(line, m) || m.path.empty() || m.path[0] != '/') continue;
      if (fs::exists(m.path) && fs::equivalent(m.path, "/bin/sleep")) {
        sleepPath = m.path;
        lowest = std::min(lowest, m.start);
        if (m.exec && execAddr == 0) {
          execAddr = m.start;
          execLen = m.end - m.start;
          execOff = m.offset;
          inode = m.inode;
        }
      }
    }
  }
  ASSERT_FALSE(sleepPath.empty());
  ASSERT_NE(execAddr, 0u);

  RecordBuilder b;
  const uint64_t t0 = nowNs();
  b.comm(pid, pid, "sleep", true, {pid, pid, t0, 0, 0, 0});
  RecordBuilder::Mmap2 mm;
  mm.pid = mm.tid = pid;
  mm.addr = execAddr;
  mm.len = execLen;
  mm.pgoff = execOff;
  mm.devMajor = 0;
  mm.inode = inode;
  mm.name = sleepPath;
  mm.id = {pid, pid, t0 + 1, 0, 0, 0};
  b.mmap2(mm);
  for (int i = 0; i < 20; ++i)
    addSample(b, pid, t0 + 100 + static_cast<uint64_t>(i) * 1000,
              {kContextUser, execAddr + 0x40 + static_cast<uint64_t>(i), execAddr + 0x100});
  addSample(b, 1, t0 + 500, {kContextUser, 0x1234});  // an unrelated process
  push(0, b);
  ASSERT_TRUE(waitFor([&] { return backend->diagnostics().drain.samples == 21; }));

  auto result = backend->stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_EQ(result->tracePath, o.outPath);
  EXPECT_EQ(result->eventsLost, 0u);
  EXPECT_TRUE(fs::exists(result->rawPath));
  EXPECT_EQ(result->rawPath, fs::path(o.outPath.string() + ".perfspool"));
  EXPECT_GT(result->durationS, 0.0);
  EXPECT_EQ(backend->state(), CaptureState::Done);

  auto model = model::TraceModel::load(o.outPath);
  ASSERT_TRUE(model.ok()) << model.error().toString();
  EXPECT_EQ(model->captureInfo.sampling_hz, 8000u);
  ASSERT_EQ(model->threads.size(), 1u) << "only the target process";
  EXPECT_EQ(model->threads[0].pid, pid);
  EXPECT_TRUE(model->threads[0].is_target);
  EXPECT_EQ(model->samplesByThread[0].ts.size(), 20u);

  // The module of the executable: base = lowest mapping of the file for a PIE image (independent check from
  // /proc/pid/maps), RVA = address - base, and a GNU build-id.
  const model::Module* sleepModule = nullptr;
  for (const auto& m : model->modules)
    if (m.path == sleepPath) sleepModule = &m;
  ASSERT_NE(sleepModule, nullptr);
  EXPECT_EQ(sleepModule->base, lowest);
  auto elf = symbols::ElfFile::open(sleepPath);
  ASSERT_TRUE(elf.ok());
  if (auto buildId = elf->buildId()) {  // the module's identity is the GNU build-id of the file
    EXPECT_EQ(sleepModule->id.kind, trace::kModuleIdGnuBuildId);
    ASSERT_EQ(sleepModule->id.len, buildId->size());
    EXPECT_TRUE(std::equal(buildId->begin(), buildId->end(), sleepModule->id.bytes.begin()));
  } else {
    EXPECT_EQ(sleepModule->id.kind, 0);
  }
  EXPECT_EQ(sleepModule->pid, pid);
  bool found = false;
  for (const auto& f : model->frames)
    if (f.module.valid() && model->modules[f.module.get()].path == sleepPath &&
        f.addr_or_rva == execAddr + 0x40 - lowest)
      found = true;
  EXPECT_TRUE(found);

  // States seen by the progress callback.
  std::set<CaptureState> seen;
  for (const auto& [s, p] : states) seen.insert(s);
  EXPECT_TRUE(seen.count(CaptureState::Converting));
  EXPECT_TRUE(seen.count(CaptureState::Done));

  // The target survives the end of the capture.
  EXPECT_TRUE(processAlive("/proc", pid));
  // ...and a new capture can start right away.
  ASSERT_TRUE(backend->start(launchOptions({"30"})).ok());
  m_toKill.push_back(backend->rootPid());
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, LostRecordsAreReportedAsEventsLost) {
  auto backend = make();
  ASSERT_TRUE(backend->start(launchOptions()).ok());
  m_toKill.push_back(backend->rootPid());
  RecordBuilder b;
  b.lost(0, 123, {0, 0, nowNs(), 1, 0, 0});
  push(1, b);
  ASSERT_TRUE(waitFor([&] { return backend->diagnostics().drain.lostEvents == 123; }));
  auto result = backend->stop();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->eventsLost, 123u);
  auto reader = trace::TraceReader::open(result->tracePath);
  ASSERT_TRUE(reader.ok());
  auto info = reader->readCaptureInfo();
  ASSERT_TRUE(info.ok());
  EXPECT_NE(info->find("\"events_lost\":123"), std::string::npos) << *info;
}

TEST_F(BackendTest, DeletingTheRawSpoolLeavesOnlyTheTrace) {
  m_config.keepRawSpool = false;
  auto backend = make();
  ASSERT_TRUE(backend->start(launchOptions()).ok());
  m_toKill.push_back(backend->rootPid());
  auto result = backend->stop();
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result->rawPath.empty());
  EXPECT_FALSE(fs::exists(fs::path(launchOptions().outPath.string() + ".perfspool")));
  EXPECT_TRUE(fs::exists(result->tracePath));
}

TEST_F(BackendTest, ParkedLaunchWaitsForTheSecondStart) {
  auto backend = make();
  CaptureOptions o = launchOptions();
  o.startImmediately = false;
  ASSERT_TRUE(backend->start(o).ok());
  EXPECT_TRUE(backend->launchPending());
  EXPECT_EQ(backend->state(), CaptureState::Starting);
  EXPECT_TRUE(m_fake->events().empty()) << "no kernel session yet";
  const uint32_t pid = backend->rootPid();
  ASSERT_GT(pid, 0u);
  m_toKill.push_back(pid);
  EXPECT_EQ(linkOf("/proc/" + std::to_string(pid) + "/exe"), selfExe()) << "still parked";

  CaptureOptions trigger;  // everything but "go" is ignored
  trigger.mode = CaptureMode::Launch;
  trigger.exe = "-";
  trigger.outPath = "ignored";
  ASSERT_TRUE(backend->start(trigger).ok());
  EXPECT_FALSE(backend->launchPending());
  EXPECT_EQ(backend->state(), CaptureState::Capturing);
  EXPECT_NE(linkOf("/proc/" + std::to_string(pid) + "/exe"), selfExe());
  EXPECT_FALSE(m_fake->events().empty());
  auto result = backend->stop();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->tracePath, o.outPath) << "the first call's options win";
}

TEST_F(BackendTest, StopOnAParkedLaunchKillsTheChildAndReportsNotFound) {
  auto backend = make();
  CaptureOptions o = launchOptions();
  o.startImmediately = false;
  ASSERT_TRUE(backend->start(o).ok());
  const uint32_t pid = backend->rootPid();
  auto r = backend->stop();
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
  EXPECT_FALSE(backend->launchPending());
  EXPECT_EQ(backend->state(), CaptureState::Idle);
  EXPECT_FALSE(processAlive("/proc", pid));
}

TEST_F(BackendTest, ExecFailureReportsTheReasonAndCleansUp) {
  TempDir dir;
  const auto notAProgram = dir.path() / "bad";
  {
    std::ofstream(notAProgram) << "text";
    fs::permissions(notAProgram, fs::perms::owner_all);
  }
  auto backend = make();
  CaptureOptions o = launchOptions({}, notAProgram.string());
  auto r = backend->start(o);
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().message.find("cannot start the target"), std::string::npos) << r.error().message;
  EXPECT_EQ(backend->state(), CaptureState::Idle);
  size_t closed = 0;
  for (const auto& e : m_fake->events()) closed += e.closed ? 1 : 0;
  EXPECT_EQ(closed, m_fake->events().size());
  EXPECT_FALSE(fs::exists(fs::path(o.outPath.string() + ".perfspool")));

  auto missing = backend->start(launchOptions({}, "/no/such/program"));
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST_F(BackendTest, LifecycleErrors) {
  auto backend = make();
  EXPECT_EQ(backend->stop().error().code, ErrorCode::NotFound);
  EXPECT_EQ(backend->flush("x", 1).error().code, ErrorCode::NotFound);
  EXPECT_EQ(backend->flush("a/b", 1).error().code, ErrorCode::InvalidArgument);

  CaptureOptions bad = launchOptions();
  bad.samplingHz = 0;
  EXPECT_EQ(backend->start(bad).error().code, ErrorCode::InvalidArgument);
  bad = launchOptions();
  bad.exe.clear();
  EXPECT_EQ(backend->start(bad).error().code, ErrorCode::InvalidArgument);
  bad = launchOptions();
  bad.mode = CaptureMode::Attach;
  bad.pid = 0;
  EXPECT_EQ(backend->start(bad).error().code, ErrorCode::InvalidArgument);
  bad.pid = 4000000;
  EXPECT_EQ(backend->start(bad).error().code, ErrorCode::NotFound);
  bad = launchOptions();
  bad.outPath.clear();
  EXPECT_EQ(backend->start(bad).error().code, ErrorCode::InvalidArgument);

  ASSERT_TRUE(backend->start(launchOptions()).ok());
  m_toKill.push_back(backend->rootPid());
  EXPECT_EQ(backend->start(launchOptions()).error().code, ErrorCode::Busy);
  EXPECT_EQ(backend->flush("x", 1).error().code, ErrorCode::InvalidArgument) << "not a circular capture";
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, AttachOpensEveryThreadAndSurvivesTheProcessGoingAway) {
  TempDir proc;
  m_fake->proc = proc.path();
  perf::ptest::writeFakeProcess(proc.path(), 500, 1, "target", {500, 501, 502},
                                {"400000-401000 r-xp 00000000 08:01 5 /gone/target"});
  m_config.forcePerThread = true;
  auto backend = make();
  CaptureOptions o;
  o.mode = CaptureMode::Attach;
  o.pid = 500;
  o.outPath = m_dir.path() / "attach.tachyon";
  ASSERT_TRUE(backend->start(o).ok());
  EXPECT_EQ(backend->rootPid(), 500u);
  std::set<int> tids;
  size_t owners = 0, redirected = 0;
  for (const auto& e : m_fake->events()) {
    tids.insert(e.pid);
    EXPECT_TRUE(e.config.inherit);
    EXPECT_FALSE(e.config.enableOnExec);
    EXPECT_TRUE(e.enabled);
    (e.outputTarget < 0 ? owners : redirected)++;
  }
  EXPECT_EQ(tids, (std::set<int>{500, 501, 502}));
  EXPECT_EQ(m_fake->events().size(), 3u * m_fake->cpus.size()) << "every thread on every cpu";
  EXPECT_EQ(owners, m_fake->cpus.size()) << "one ring per cpu";
  EXPECT_EQ(redirected, 2u * m_fake->cpus.size()) << "the other threads write into their cpu's ring";
  for (const auto& e : m_fake->events()) {
    if (e.outputTarget < 0) continue;
    bool sameCpuOwner = false;
    for (const auto& o : m_fake->events())
      sameCpuOwner |= o.fd == e.outputTarget && o.cpu == e.cpu && o.outputTarget < 0;
    EXPECT_TRUE(sameCpuOwner) << "set_output only joins events of the same cpu";
  }
  EXPECT_EQ(backend->diagnostics().rings, m_fake->cpus.size());
  EXPECT_FALSE(backend->targetExited());
  // The process goes away.
  fs::remove_all(proc.path() / "500");
  perf::ptest::writeFakeProcess(proc.path(), 500, 1, "target", {500}, {}, 'Z');
  EXPECT_TRUE(backend->targetExited());

  RecordBuilder b;
  addSample(b, 500, nowNs() + 1000, {kContextUser, 0x400100});
  push(0, b);
  ASSERT_TRUE(waitFor([&] { return backend->diagnostics().drain.samples == 1; }));
  auto result = backend->stop();
  ASSERT_TRUE(result.ok()) << result.error().toString();
  auto model = model::TraceModel::load(o.outPath);
  ASSERT_TRUE(model.ok());
  EXPECT_EQ(model->threads.size(), 3u) << "the rundown knew all three threads";
  ASSERT_EQ(model->modules.size(), 1u);
  EXPECT_EQ(model->modules[0].path, "/gone/target");
  EXPECT_EQ(model->modules[0].base, 0x400000u);
}

TEST_F(BackendTest, AttachToAGoneProcessFailsWithNotFound) {
  TempDir proc;
  m_fake->proc = proc.path();
  auto backend = make();
  CaptureOptions o;
  o.mode = CaptureMode::Attach;
  o.pid = 777;
  o.outPath = m_dir.path() / "a.tachyon";
  EXPECT_EQ(backend->start(o).error().code, ErrorCode::NotFound);
  EXPECT_EQ(backend->state(), CaptureState::Idle);
}

TEST_F(BackendTest, TargetExitDetectionForLaunchAndFollowedChildren) {
  TempDir scratch;
  const auto pidFile = scratch.path() / "grandchild.pid";
  auto backend = make();
  CaptureOptions o =
      launchOptions({"-c", "sleep 30 & echo $! > " + pidFile.string() + "; exit 0"}, "/bin/sh");
  o.followChildren = true;
  ASSERT_TRUE(backend->start(o).ok());
  const uint32_t root = backend->rootPid();
  ASSERT_TRUE(waitFor([&] { return fs::exists(pidFile) && fs::file_size(pidFile) > 0; }));
  std::ifstream in(pidFile);
  uint32_t grandchild = 0;
  in >> grandchild;
  ASSERT_GT(grandchild, 0u);
  m_toKill.push_back(grandchild);
  // Wait for the shell to be gone.
  ASSERT_TRUE(waitFor([&] { return !processAlive("/proc", root); }));
  // The kernel reports the fork of the grandchild: the drain tells the tracker.
  RecordBuilder b;
  b.fork(grandchild, root, grandchild, root, nowNs(), {root, root, nowNs(), 0, 0, 0});
  push(0, b);
  ASSERT_TRUE(waitFor([&] { return backend->diagnostics().drain.metaRecords >= 1; }));
  EXPECT_FALSE(backend->targetExited()) << "the root exited but a followed child is still running";
  killAndReap(grandchild);
  m_toKill.clear();
  EXPECT_TRUE(waitFor([&] { return backend->targetExited(); }));
  ASSERT_TRUE(backend->stop().ok());
}

TEST_F(BackendTest, TargetExitedWithoutFollowingIsTheRootAlone) {
  auto backend = make();
  CaptureOptions o = launchOptions({"-c", "exit 0"}, "/bin/sh");
  ASSERT_TRUE(backend->start(o).ok());
  EXPECT_TRUE(waitFor([&] { return backend->targetExited(); }));
  ASSERT_TRUE(backend->stop().ok());
  EXPECT_FALSE(backend->targetExited()) << "no capture is live";
}

TEST_F(BackendTest, CircularFlushWritesTheWindowAndStartsOver) {
  auto backend = make();
  CaptureOptions o = launchOptions();
  o.circularMb = 16;
  ASSERT_TRUE(backend->start(o).ok());
  const uint32_t pid = backend->rootPid();
  m_toKill.push_back(pid);

  uint64_t t = nowNs();
  RecordBuilder first;
  first.comm(pid, pid, "sleep", true, {pid, pid, t, 0, 0, 0});
  for (int i = 0; i < 100; ++i)
    addSample(first, pid, t + 10 + static_cast<uint64_t>(i) * 1000, {kContextUser, 0x1000});
  push(0, first);
  ASSERT_TRUE(waitFor([&] { return backend->diagnostics().drain.samples == 100; }));

  auto flushed = backend->flush("hitch-1", 0);
  ASSERT_TRUE(flushed.ok()) << flushed.error().toString();
  EXPECT_EQ(flushed->tracePath, m_dir.path() / "hitch-1.tachyon");
  EXPECT_TRUE(fs::exists(flushed->rawPath));
  EXPECT_EQ(backend->state(), CaptureState::Capturing) << "a flush does not stop the capture";
  auto one = model::TraceModel::load(flushed->tracePath);
  ASSERT_TRUE(one.ok()) << one.error().toString();
  ASSERT_EQ(one->threads.size(), 1u);
  EXPECT_EQ(one->samplesByThread[0].ts.size(), 100u);
  EXPECT_EQ(one->threads[0].name, "sleep") << "the history in meta.bin names the thread";

  RecordBuilder second;
  t = nowNs();
  for (int i = 0; i < 30; ++i)
    addSample(second, pid, t + static_cast<uint64_t>(i) * 1000, {kContextUser, 0x2000});
  push(1, second);
  ASSERT_TRUE(waitFor([&] { return backend->diagnostics().drain.samples == 130; }));
  auto again = backend->flush("hitch-2", 0);
  ASSERT_TRUE(again.ok()) << again.error().toString();
  auto two = model::TraceModel::load(again->tracePath);
  ASSERT_TRUE(two.ok());
  EXPECT_EQ(two->samplesByThread[0].ts.size(), 30u) << "the second flush starts after the first";
  EXPECT_EQ(backend->diagnostics().flushCount, 2u);

  // The window: only the last 1 second of what is recorded now.
  RecordBuilder third;
  t = nowNs();
  for (int i = 0; i < 10; ++i)
    addSample(third, pid, t - 5'000'000'000ull + static_cast<uint64_t>(i),
              {kContextUser, 0x3000});  // 5 s old
  for (int i = 0; i < 7; ++i) addSample(third, pid, t + static_cast<uint64_t>(i), {kContextUser, 0x3000});
  push(0, third);
  ASSERT_TRUE(waitFor([&] { return backend->diagnostics().drain.samples == 147; }));
  auto windowed = backend->flush("hitch-3", 1);
  ASSERT_TRUE(windowed.ok());
  auto three = model::TraceModel::load(windowed->tracePath);
  ASSERT_TRUE(three.ok());
  EXPECT_EQ(three->samplesByThread[0].ts.size(), 7u);

  auto result = backend->stop();
  ASSERT_TRUE(result.ok());
}

}  // namespace
}  // namespace tachyon::capture
