#include "capture/linux_perf/linux_perf_backend.h"

#include "capture/linux_perf/proc_snapshot.h"
#include "capture/windows_etw/session_config.h"
#include "core/log.h"
#include "trace/trace_format.h"

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <nlohmann/json.hpp>
#include <system_error>

namespace tachyon::capture {

namespace fs = std::filesystem;
using perf::EventConfig;
using perf::IPerfSystem;

namespace {

double secondsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

fs::path spoolDirFor(const fs::path& outPath) {
  return fs::path(outPath.string() + ".perfspool");
}

bool isPowerOfTwo(uint32_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

std::string joinCommandLine(const std::string& exe, const std::vector<std::string>& args) {
  std::string out = exe;
  for (const std::string& a : args) {
    out.push_back(' ');
    out += a;
  }
  return out;
}

}  // namespace

struct LinuxPerfBackend::Session {
  std::vector<Slot> slots;
  std::unique_ptr<perf::Drain> drain;
  perf::DescendantTracker tracker;
  perf::SessionInfo info;
  fs::path spoolDir;
  bool systemWide = false;
  uint32_t ringPages = 0;
  bool kernelBuildId = true;
  bool excludeKernel = false;
  bool enabled = false;
  std::map<int, int> ownerByCpu;  // per-thread plan: cpu -> fd that owns the cpu's ring
  std::vector<std::string> warnings;
  std::vector<uint32_t> roots;
  perf::SchedPlan sched;
  std::unique_ptr<perf::LiveSource> live;  // WP-65a: decodes what the drain reads, for the live sink
};

LinuxPerfBackend::LinuxPerfBackend(LinuxPerfBackendConfig config)
    : m_config(std::move(config)),
      m_system(m_config.system ? m_config.system : perf::makeLinuxPerfSystem()) {}

LinuxPerfBackend::~LinuxPerfBackend() {
  std::unique_ptr<Session> session;
  std::unique_ptr<perf::LaunchedProcess> child;
  {
    std::lock_guard lock(m_mutex);
    session = std::move(m_session);
    if (m_pending) child = std::move(m_child);  // a parked child would never run again
  }
  if (session) teardown(*session);
  if (child) child->terminate();
}

CaptureState LinuxPerfBackend::state() const {
  std::lock_guard lock(m_mutex);
  return m_state;
}

void LinuxPerfBackend::setProgressCallback(std::function<void(CaptureState, float)> callback) {
  std::lock_guard lock(m_mutex);
  m_callback = std::move(callback);
}

LinuxPerfDiagnostics LinuxPerfBackend::diagnostics() const {
  std::lock_guard lock(m_mutex);
  LinuxPerfDiagnostics d = m_diag;
  if (m_session && m_session->drain) d.drain = m_session->drain->stats();
  if (m_session && m_session->live) d.live = m_session->live->stats();
  return d;
}

void LinuxPerfBackend::setLiveSink(std::shared_ptr<ILiveSink> sink) {
  std::lock_guard lock(m_mutex);
  m_liveSink = std::move(sink);
}

void LinuxPerfBackend::notify(CaptureState state, float progress) {
  std::function<void(CaptureState, float)> cb;
  {
    std::lock_guard lock(m_mutex);
    cb = m_callback;
  }
  if (cb) cb(state, progress);
}

void LinuxPerfBackend::reapFinished() {
  std::lock_guard lock(m_mutex);
  m_reap.erase(std::remove_if(m_reap.begin(), m_reap.end(), [](const auto& p) { return p->exited(); }),
               m_reap.end());
}

Result<void> LinuxPerfBackend::checkPrivileges() {
  const std::vector<uint32_t> cpus = m_system->onlineCpus();
  EventConfig cfg;
  cfg.samplingHz = 1;
  cfg.disabled = true;
  const perf::SysctlInfo sys = m_system->sysctls();
  const perf::OpenOutcome o = m_system->openEvent(cfg, -1, cpus.empty() ? 0 : static_cast<int>(cpus.front()));
  if (o.fd < 0)
    return perf::describeOpenError(o.err, cfg, sys, -1, cpus.empty() ? 0 : static_cast<int>(cpus.front()));
  m_system->closeEvent(o.fd);
  return ok();
}

Result<void> LinuxPerfBackend::start(const CaptureOptions& options) {
  reapFinished();
  CaptureOptions effective = options;
  std::unique_ptr<perf::LaunchedProcess> child;
  bool trigger = false;
  {
    std::lock_guard lock(m_mutex);
    if (m_pending) {  // the second start() of a deferred launch: record now, other fields are ignored
      effective = m_options;
      effective.startImmediately = true;
      child = std::move(m_child);
      m_pending = false;
      trigger = true;
    } else {
      TACHYON_TRY(m_lifecycle.beginStart());
      m_state = CaptureState::Starting;
    }
  }
  auto fail = [this, &child](Error e) -> Result<void> {
    if (child) child->terminate();
    std::lock_guard lock(m_mutex);
    m_lifecycle.startFailed();
    m_state = CaptureState::Idle;
    m_pending = false;
    return e;
  };

  if (!trigger) {
    if (options.samplingHz == 0)
      return fail(makeError(ErrorCode::InvalidArgument, "sampling_hz must be positive"));
    if (!isPowerOfTwo(m_config.ringPages))
      return fail(makeError(ErrorCode::InvalidArgument, "ring size must be a power-of-two number of pages"));
    if (options.outPath.empty()) return fail(makeError(ErrorCode::InvalidArgument, "no output path given"));
    if (options.mode == CaptureMode::Launch) {
      if (options.exe.empty()) return fail(makeError(ErrorCode::InvalidArgument, "launch mode needs an exe"));
      perf::LaunchSpec spec;
      spec.exe = options.exe;
      spec.args = options.args;
      spec.cwd = options.cwd;
      spec.env = options.env;
      auto spawned = perf::LaunchedProcess::spawnParked(spec);
      if (!spawned.ok()) return fail(spawned.error());
      child = std::make_unique<perf::LaunchedProcess>(std::move(*spawned));
      effective.pid = child->pid();
      if (!options.startImmediately) {
        TACHYON_LOG_INFO("perf: launched '{}' parked as pid {}", options.exe, effective.pid);
        std::lock_guard lock(m_mutex);
        m_options = effective;
        m_child = std::move(child);
        m_pending = true;
        m_diag = LinuxPerfDiagnostics{};
        m_horizon = perf::RetentionHorizon(m_config.horizonMemoryNs);
        m_diag.rootPid = effective.pid;
        return ok();  // lifecycle and state stay Starting until the second start()
      }
    } else if (options.mode == CaptureMode::Attach) {
      if (options.pid == 0) return fail(makeError(ErrorCode::InvalidArgument, "attach mode needs a pid"));
      if (!perf::processAlive(m_system->procRoot(), options.pid))
        return fail(makeError(ErrorCode::NotFound,
                              "attach target pid " + std::to_string(options.pid) + " does not exist"));
    }
  }
  auto started = beginRecording(effective, std::move(child));
  if (!started.ok()) {
    std::lock_guard lock(m_mutex);
    m_lifecycle.startFailed();
    m_state = CaptureState::Idle;
    m_pending = false;
    return started.error();
  }
  return ok();
}

Result<void> LinuxPerfBackend::openSlot(Session& session, const EventConfig& cfg, int pid, int cpu) {
  const perf::OpenOutcome o = m_system->openEvent(cfg, pid, cpu);
  if (o.fd < 0) return perf::describeOpenError(o.err, cfg, m_system->sysctls(), pid, cpu);
  session.kernelBuildId = session.kernelBuildId && o.kernelBuildId;
  uint32_t pages = session.ringPages;
  for (;;) {
    perf::RingOutcome ring = m_system->mapRing(o.fd, pages);
    if (ring.ring) {
      session.ringPages = std::min(session.ringPages, pages);
      Slot slot;
      slot.fd = o.fd;
      slot.cpu = cpu;
      slot.ring = std::move(ring.ring);
      session.slots.push_back(std::move(slot));
      return ok();
    }
    if (pages > 16 && (ring.err == EPERM || ring.err == ENOMEM || ring.err == EAGAIN)) {
      pages /= 2;  // the locked-memory budget: try a smaller ring
      continue;
    }
    m_system->closeEvent(o.fd);
    return perf::describeOpenError(ring.err, cfg, m_system->sysctls(), pid, cpu, true);
  }
}

Result<void> LinuxPerfBackend::ensureDescriptors(size_t threads, size_t cpus) {
  constexpr size_t kSlack = 128;  // the spool, pipes, sockets, the agent's own connections
  const size_t needed = threads * cpus;
  rlimit lim{};
  if (::getrlimit(RLIMIT_NOFILE, &lim) != 0) return ok();  // cannot tell: the open itself will report EMFILE
  if (lim.rlim_cur == RLIM_INFINITY || needed + kSlack <= lim.rlim_cur) return ok();
  const rlim_t want = static_cast<rlim_t>(needed + kSlack);
  if (lim.rlim_max == RLIM_INFINITY || want <= lim.rlim_max) {
    rlimit raised = lim;
    raised.rlim_cur = want;
    if (::setrlimit(RLIMIT_NOFILE, &raised) == 0) return ok();
  } else if (lim.rlim_cur < lim.rlim_max) {
    rlimit raised = lim;
    raised.rlim_cur = lim.rlim_max;
    (void)::setrlimit(RLIMIT_NOFILE, &raised);
  }
  ::getrlimit(RLIMIT_NOFILE, &lim);
  return makeError(ErrorCode::Internal,
                   std::to_string(needed) + " descriptors needed (" + std::to_string(threads) +
                       " threads x " + std::to_string(cpus) + " cpus, plus " + std::to_string(kSlack) +
                       " spare) but the limit is " + std::to_string(lim.rlim_cur) + " (hard limit " +
                       std::to_string(lim.rlim_max) +
                       "): raise it with `ulimit -n`, sample fewer cpus' worth of threads, or run with "
                       "CAP_PERFMON for the system-wide plan, which needs one descriptor per cpu");
}

Result<void> LinuxPerfBackend::openTask(Session& session, const EventConfig& cfg, int tid) {
  for (uint32_t cpu : m_system->onlineCpus()) {
    auto owner = session.ownerByCpu.find(static_cast<int>(cpu));
    if (owner == session.ownerByCpu.end()) {
      TACHYON_TRY(openSlot(session, cfg, tid, static_cast<int>(cpu)));
      session.ownerByCpu[static_cast<int>(cpu)] = session.slots.back().fd;
      continue;
    }
    // An inherited per-task event cannot be mmapped (the kernel refuses: every child would write into one
    // ring), so each task gets one event per cpu and all of a cpu's events share that cpu's ring.
    const perf::OpenOutcome o = m_system->openEvent(cfg, tid, static_cast<int>(cpu));
    if (o.fd < 0) return perf::describeOpenError(o.err, cfg, m_system->sysctls(), tid, static_cast<int>(cpu));
    Slot slot;
    slot.fd = o.fd;
    slot.cpu = static_cast<int>(cpu);
    session.slots.push_back(std::move(slot));
    if (!m_system->setOutput(o.fd, owner->second))
      return makeError(ErrorCode::Internal,
                       std::string("PERF_EVENT_IOC_SET_OUTPUT failed: ") + std::strerror(errno));
  }
  return ok();
}

Result<void> LinuxPerfBackend::openSchedEvents(Session& session, const EventConfig& base, int pid, int cpu,
                                               int ownerFd) {
  if (!session.sched.tracepoints()) return ok();
  for (uint32_t id : {session.sched.switchId, session.sched.wakeId}) {
    if (id == 0) continue;
    EventConfig cfg = base;
    cfg.kind = EventConfig::Kind::Tracepoint;
    cfg.tracepointId = id;
    cfg.contextSwitch = false;
    const perf::OpenOutcome o = m_system->openEvent(cfg, pid, cpu);
    if (o.fd < 0) return perf::describeOpenError(o.err, cfg, m_system->sysctls(), pid, cpu);
    Slot slot;
    slot.fd = o.fd;
    slot.cpu = cpu;
    session.slots.push_back(std::move(slot));  // no ring of its own: it writes into the owner's
    if (!m_system->setOutput(o.fd, ownerFd))
      return makeError(
          ErrorCode::Internal,
          std::string("PERF_EVENT_IOC_SET_OUTPUT failed for a sched tracepoint: ") + std::strerror(errno));
  }
  return ok();
}

Result<void> LinuxPerfBackend::openPlan(Session& session, const CaptureOptions& options, bool systemWide) {
  const perf::SysctlInfo sys = m_system->sysctls();
  const uint32_t pageBytes = 4096;
  EventConfig cfg;
  cfg.samplingHz = options.samplingHz;
  cfg.sampleType = session.info.layout.sampleType;
  cfg.contextSwitch = session.sched.contextSwitch();
  cfg.wakeupBytes = std::max<uint32_t>(session.ringPages * pageBytes / 4, pageBytes);
  cfg.disabled = true;
  if (systemWide) {
    cfg.excludeKernel = false;
    cfg.excludeCallchainKernel = !options.kernelStacks;
    for (uint32_t cpu : m_system->onlineCpus()) {
      TACHYON_TRY(openSlot(session, cfg, -1, static_cast<int>(cpu)));
      TACHYON_TRY(openSchedEvents(session, cfg, -1, static_cast<int>(cpu), session.slots.back().fd));
    }
    session.excludeKernel = false;
    return ok();
  }
  // Per-thread: unprivileged use. Kernel-mode samples need paranoid <= 1 (or CAP_PERFMON).
  cfg.inherit = true;
  cfg.enableOnExec = options.mode == CaptureMode::Launch;
  cfg.excludeKernel = sys.paranoid >= 2 && !sys.hasPerfCapability;
  cfg.excludeCallchainKernel = !options.kernelStacks || cfg.excludeKernel;
  session.excludeKernel = cfg.excludeKernel;
  if (cfg.excludeKernel)
    session.warnings.push_back(
        "kernel.perf_event_paranoid is " + std::to_string(sys.paranoid) +
        " and the process lacks CAP_PERFMON: kernel-mode samples and kernel frames are "
        "excluded (lower it to 1 or grant CAP_PERFMON to include them)");
  std::vector<uint32_t> opened;
  {
    const size_t threads =
        options.mode == CaptureMode::Launch ? 1 : perf::listThreads(m_system->procRoot(), options.pid).size();
    TACHYON_TRY(ensureDescriptors(threads, m_system->onlineCpus().size()));
  }
  if (options.mode == CaptureMode::Launch) {
    TACHYON_TRY(openTask(session, cfg, static_cast<int>(options.pid)));
    opened.push_back(options.pid);
  } else {
    // Attach: every thread that exists now; threads created later are covered by inherit, and threads created
    // while the events are being opened by the re-scans.
    for (int round = 0; round < 4; ++round) {
      bool added = false;
      for (uint32_t tid : perf::listThreads(m_system->procRoot(), options.pid)) {
        if (std::find(opened.begin(), opened.end(), tid) != opened.end()) continue;
        auto r = openTask(session, cfg, static_cast<int>(tid));
        if (!r.ok()) {
          if (r.error().code == ErrorCode::NotFound) continue;  // the thread just exited
          return r;
        }
        opened.push_back(tid);
        added = true;
      }
      if (!added) break;
    }
    if (opened.empty())
      return makeError(ErrorCode::NotFound,
                       "attach target pid " + std::to_string(options.pid) + " has no threads left");
  }
  return ok();
}

void LinuxPerfBackend::teardown(Session& session) {
  if (session.enabled || !session.slots.empty()) {
    for (Slot& s : session.slots)
      if (s.fd >= 0) (void)m_system->setEnabled(s.fd, false);
    session.enabled = false;
  }
  if (session.drain) (void)session.drain->finish();
  if (session.live) session.live->finish();
  for (Slot& s : session.slots) s.ring.reset();
  for (Slot& s : session.slots) {
    if (s.fd >= 0) m_system->closeEvent(s.fd);
    s.fd = -1;
  }
  session.slots.clear();
  session.ownerByCpu.clear();
}

Result<void> LinuxPerfBackend::openEvents(Session& session, const CaptureOptions& options) {
  // The record layout of the events: tracepoint captures add RAW to every event.
  session.info.layout = perf::RecordLayout{
      session.sched.tracepoints() ? perf::kSchedSampleType : perf::kBackendSampleType, true};
  session.info.startTs = m_system->nowNs();
  // System-wide when allowed; Launch / Attach fall back to per-thread events.
  const bool trySystem = options.mode == CaptureMode::System || !m_config.forcePerThread;
  // A task-bound sched_switch event sees only the switches out of its task and a task-bound sched_waking
  // only the wakeups it issues, so the tracepoints are recorded in the system-wide plan only; the per-thread
  // plan gets the task-bound PERF_RECORD_SWITCH out / in records instead.
  const std::string perThreadReason =
      "the per-thread plan cannot record scheduler tracepoints: a task-bound sched_switch event misses the "
      "switches into the task and a task-bound sched_waking event the wakeups by other tasks";
  auto demoteSched = [&](const std::string& why) {
    session.sched = perf::fallbackSchedPlan(options.stackCSwitch, options.stackReady, why);
    session.info.layout = perf::RecordLayout{perf::kBackendSampleType, true};
  };
  if (!trySystem && session.sched.tracepoints()) demoteSched(perThreadReason);
  bool systemWide = false;
  if (trySystem) {
    auto r = openPlan(session, options, true);
    if (r.ok()) {
      systemWide = true;
    } else if (options.mode == CaptureMode::System || r.error().code != ErrorCode::NoPrivilege) {
      return r.error();
    } else {
      session.warnings.push_back("system-wide sampling is not permitted (" + r.error().message +
                                 "); sampling the target's threads with inherit instead");
      TACHYON_LOG_WARN("perf: {}", session.warnings.back());
      teardown(session);
      session.ringPages = m_config.ringPages;
      session.kernelBuildId = true;
      if (session.sched.tracepoints()) demoteSched(perThreadReason);
    }
  }
  if (!systemWide) {
    if (options.mode == CaptureMode::System)
      return makeError(ErrorCode::NoPrivilege,
                       "system mode needs system-wide sampling, which is disabled here");
    TACHYON_TRY(openPlan(session, options, false));
  }
  session.systemWide = systemWide;
  return ok();
}

Result<void> LinuxPerfBackend::beginRecording(const CaptureOptions& options,
                                              std::unique_ptr<perf::LaunchedProcess> child) {
  auto session = std::make_unique<Session>();
  auto fail = [&](Error e) -> Result<void> {
    teardown(*session);
    std::error_code ec;
    if (!session->spoolDir.empty()) fs::remove_all(session->spoolDir, ec);
    if (child) child->terminate();
    return e;
  };

  const perf::SysctlInfo sys = m_system->sysctls();
  if (sys.maxSampleRate > 0 && options.samplingHz > sys.maxSampleRate) {
    EventConfig cfg;
    cfg.samplingHz = options.samplingHz;
    return fail(perf::describeOpenError(EINVAL, cfg, sys, -1, -1));
  }
  session->ringPages = m_config.ringPages;
  session->spoolDir = spoolDirFor(options.outPath);
  std::error_code ec;
  fs::remove_all(session->spoolDir, ec);
  if (options.outPath.has_parent_path()) fs::create_directories(options.outPath.parent_path(), ec);
  fs::create_directories(session->spoolDir, ec);
  if (ec)
    return fail(
        makeError(ErrorCode::IoError, "cannot create " + session->spoolDir.string() + ": " + ec.message()));

  if (options.mode != CaptureMode::System) session->roots = {options.pid};

  perf::SessionInfo& info = session->info;
  info.samplingHz = options.samplingHz;
  info.mode = std::string(captureModeName(options.mode));
  info.rootPid = options.mode == CaptureMode::System ? 0 : options.pid;
  info.followChildren = options.followChildren;
  info.childFilter = options.childFilter;
  info.exe = options.exe;
  info.args = options.args;
  info.kernelStacks = options.kernelStacks;
  info.paranoid = sys.paranoid;
  info.maxSampleRate = sys.maxSampleRate;
  info.cpuTimeMaxPercent = sys.cpuTimeMaxPercent;
  info.circularMb = options.circularMb;
  info.agentVersion = m_config.agentVersion;
  utsname un{};
  if (::uname(&un) == 0) {
    info.kernelRelease = un.release;
    info.hostname = un.nodename;
  }
  info.cpuCount = static_cast<uint32_t>(m_system->onlineCpus().size());

  // Open the events: the chosen plan (see openEvents) with the scheduler events of the sched plan. A refused
  // tracepoint event is not fatal: the capture falls back to PERF_RECORD_SWITCH records and says so.
  session->sched =
      perf::planSched(m_config.forceContextSwitchFallback
                          ? fs::path{}
                          : (m_config.tracefsRoot.empty() ? m_system->tracefsRoot() : m_config.tracefsRoot),
                      options.stackCSwitch, options.stackReady);
  const size_t warningsBefore = session->warnings.size();
  auto opened = openEvents(*session, options);
  if (!opened.ok() && session->sched.tracepoints()) {
    TACHYON_LOG_WARN("perf: sched tracepoints could not be opened ({}); using PERF_RECORD_SWITCH",
                     opened.error().message);
    teardown(*session);
    session->warnings.resize(warningsBefore);
    session->ringPages = m_config.ringPages;
    session->kernelBuildId = true;
    session->sched =
        perf::fallbackSchedPlan(options.stackCSwitch, options.stackReady,
                                "opening the tracepoint events failed: " + opened.error().message);
    opened = openEvents(*session, options);
  }
  if (!opened.ok()) return fail(opened.error());
  for (const std::string& w : session->sched.warnings) {
    session->warnings.push_back(w);
    TACHYON_LOG_WARN("perf: {}", w);
  }
  info.sched = session->sched.info;
  info.layout = perf::RecordLayout{
      session->sched.tracepoints() ? perf::kSchedSampleType : perf::kBackendSampleType, true};
  const bool systemWide = session->systemWide;
  info.plan = systemWide ? "system-wide" : "per-thread";
  info.kernelBuildId = session->kernelBuildId;
  info.excludeKernel = session->excludeKernel;

  // The spool and the drain.
  perf::DrainConfig dc;
  dc.layout = info.layout;
  dc.spoolDir = session->spoolDir;
  dc.circularMb = options.circularMb;
  dc.pollInterval = m_config.drainInterval;
  std::vector<perf::IRing*> rings;
  for (Slot& s : session->slots)
    if (s.ring) rings.push_back(s.ring.get());
  info.ringCount = static_cast<uint32_t>(rings.size());
  session->drain = std::make_unique<perf::Drain>(dc, rings, &session->tracker);
  if (auto r = session->drain->open(); !r.ok()) return fail(r.error());
  session->tracker.setRoots(session->roots, options.followChildren);

  // Live view: a second consumer of what the drain reads. It starts before the events are enabled so that no
  // chunk is missed; the sink hears the session facts first.
  if (options.live) {
    std::shared_ptr<ILiveSink> sink;
    {
      std::lock_guard lock(m_mutex);
      sink = m_liveSink;
    }
    if (!sink)
      return fail(makeError(ErrorCode::InvalidArgument, "a live capture needs a live sink (setLiveSink)"));
    LiveSessionInfo facts;
    facts.clockFrequencyHz = 1'000'000'000ull;  // CLOCK_MONOTONIC_RAW nanoseconds
    facts.platform = trace::kPlatformLinux;
    facts.samplingHz = options.samplingHz;
    facts.startTs = info.startTs;
    sink->onSessionInfo(facts);
    perf::LiveSourceConfig lc;
    lc.decoder.sink = sink;
    lc.decoder.layout = info.layout;
    lc.decoder.sched = info.sched;
    lc.decoder.rootPids = session->roots;
    lc.decoder.followChildren = options.followChildren;
    lc.decoder.childFilter = options.childFilter;
    lc.decoder.startTs = info.startTs;
    if (options.mode == CaptureMode::Launch)
      lc.decoder.rootCommandLine = joinCommandLine(options.exe, options.args);
    lc.decoder.procRoot = m_system->procRoot();
    const std::shared_ptr<perf::IPerfSystem> system = m_system;
    lc.nowNs = [system] { return system->nowNs(); };
    session->live = std::make_unique<perf::LiveSource>(std::move(lc));
    session->live->start();
    perf::LiveSource* liveSource = session->live.get();
    session->drain->setTap([liveSource](std::span<const uint8_t> meta, std::span<const uint8_t> data) {
      liveSource->submit(meta, data);
    });
  }

  // Enable. The per-thread launch plan enables itself at the child's exec.
  const bool selfEnabling = !systemWide && options.mode == CaptureMode::Launch;
  if (!selfEnabling) {
    for (Slot& s : session->slots) {
      if (!m_system->setEnabled(s.fd, true))
        return fail(makeError(ErrorCode::Internal,
                              std::string("PERF_EVENT_IOC_ENABLE failed: ") + std::strerror(errno)));
    }
  }
  session->enabled = true;

  // The rundown follows the enable: processes created in between show up twice, which the converter ignores.
  perf::RundownOptions ro;
  ro.procRoot = m_system->procRoot();
  ro.rootPids = session->roots;
  ro.includeDescendants = options.followChildren;
  ro.ts = info.startTs;
  ro.layout = info.layout;
  perf::RundownResult rundown = perf::buildRundown(ro);
  size_t consumed = 0;
  (void)perf::forEachRecord(
      std::span<const uint8_t>(rundown.records), consumed, [&](std::span<const uint8_t> rec) {
        perf::ParsedRecord pr;
        if (perf::parseRecord(rec, info.layout, pr) == perf::ParseStatus::Ok &&
            pr.type() == perf::kRecordFork)
          session->tracker.onFork(pr.pid, pr.ppid,
                                  pr.tid);  // children that already existed count as descendants
      });
  if (auto r = session->drain->appendMeta(perf::kSpoolRingRundown, rundown.records); !r.ok())
    return fail(r.error());
  if (session->live) session->live->submit(rundown.records, {}, true);
  session->drain->start();
  info.warnings = session->warnings;
  if (auto r = perf::writeSessionInfo(session->spoolDir, info); !r.ok()) return fail(r.error());

  // Everything is live: let the launched target run.
  if (child) {
    if (auto r = child->release(); !r.ok()) return fail(r.error());
  }

  {
    std::lock_guard lock(m_mutex);
    m_options = options;
    m_session = std::move(session);
    m_child = std::move(child);
    m_diag = LinuxPerfDiagnostics{};
    m_horizon = perf::RetentionHorizon(m_config.horizonMemoryNs);
    m_diag.plan = m_session->info.plan;
    m_diag.schedPath = m_session->sched.info.mode;
    m_diag.rings = m_session->info.ringCount;
    m_diag.ringPages = m_session->ringPages;
    m_diag.kernelBuildId = m_session->kernelBuildId;
    m_diag.excludeKernel = m_session->excludeKernel;
    m_diag.sysctls = sys;
    m_diag.warnings = m_session->warnings;
    m_diag.rootPid = m_session->info.rootPid;
    m_diag.spoolDir = m_session->spoolDir;
    m_startTime = std::chrono::steady_clock::now();
    m_lifecycle.startSucceeded(options.circularMb > 0);
    m_state = CaptureState::Capturing;
  }
  TACHYON_LOG_INFO("perf: capture started ({} mode, {} plan, {} Hz, {} rings x {} pages, sched: {}{})",
                   captureModeName(options.mode), info.plan, options.samplingHz, info.ringCount,
                   m_diag.ringPages, m_diag.schedPath, options.circularMb > 0 ? ", circular" : "");
  return ok();
}

bool LinuxPerfBackend::launchPending() const {
  std::lock_guard lock(m_mutex);
  return m_pending;
}

bool LinuxPerfBackend::targetExited() const {
  std::lock_guard lock(m_mutex);
  if (m_state != CaptureState::Capturing || !m_session || m_options.mode == CaptureMode::System) return false;
  const fs::path procRoot = m_system->procRoot();
  const bool rootGone = m_child ? m_child->exited() : !perf::processAlive(procRoot, m_options.pid);
  if (!rootGone) return false;
  if (!m_options.followChildren) return true;
  for (uint32_t pid : m_session->tracker.pids())
    if (pid != m_options.pid && perf::processAlive(procRoot, pid)) return false;
  return true;
}

uint32_t LinuxPerfBackend::rootPid() const {
  std::lock_guard lock(m_mutex);
  if (!m_pending && m_state != CaptureState::Capturing) return 0;
  return m_options.mode == CaptureMode::System ? 0 : m_options.pid;
}

perf::ConvertOptions LinuxPerfBackend::convertOptionsFor(const CaptureOptions& options,
                                                         const Session& session) const {
  perf::ConvertOptions co;
  co.rootPids = session.roots;
  co.followChildren = options.followChildren;
  co.childFilter = options.childFilter;
  co.agentVersion = m_config.agentVersion;
  co.procRoot = m_system->procRoot();
  if (options.mode == CaptureMode::Launch) co.rootCommandLine = joinCommandLine(options.exe, options.args);
  nlohmann::json j;
  j["mode"] = std::string(captureModeName(options.mode));
  j["exe"] = options.exe;
  j["args"] = options.args;
  j["pid"] = options.pid;
  j["sampling_hz"] = options.samplingHz;
  j["follow_children"] = options.followChildren;
  j["child_filter"] = options.childFilter;
  j["circular_mb"] = options.circularMb;
  j["kernel_stacks"] = options.kernelStacks;
  j["plan"] = session.info.plan;
  j["sched_path"] = session.sched.info.mode;
  j["stack_cswitch"] = options.stackCSwitch;
  j["stack_ready"] = options.stackReady;
  co.captureOptionsJson = j.dump();
  return co;
}

Result<CaptureResult> LinuxPerfBackend::stop() {
  std::unique_ptr<Session> session;
  std::unique_ptr<perf::LaunchedProcess> child;
  CaptureOptions options;
  double durationS = 0.0;
  {
    std::lock_guard lock(m_mutex);
    if (m_pending) {  // launched but never recording: the parked child is useless
      std::unique_ptr<perf::LaunchedProcess> parked = std::move(m_child);
      m_pending = false;
      m_lifecycle.startFailed();
      m_state = CaptureState::Idle;
      if (parked) parked->terminate();
      return makeError(ErrorCode::NotFound,
                       "the launched process never started recording; it was terminated");
    }
    auto kind = m_lifecycle.beginStop();
    if (!kind.ok()) return kind.error();
    if (*kind == etw::StopKind::NoSession) {
      m_lifecycle.stopFinished();
      m_state = CaptureState::Idle;
      return makeError(ErrorCode::Internal, "no perf session is live");
    }
    session = std::move(m_session);
    child = std::move(m_child);
    options = m_options;
    durationS = secondsSince(m_startTime);
    m_state = CaptureState::Converting;
  }
  auto finishFailed = [this](Error e) -> Result<CaptureResult> {
    std::lock_guard lock(m_mutex);
    m_lifecycle.stopFinished();
    m_state = CaptureState::Idle;
    return e;
  };

  // Stop the events, take the last records out of the rings, close the spool.
  for (Slot& s : session->slots) (void)m_system->setEnabled(s.fd, false);
  session->enabled = false;
  const uint64_t endTs = m_system->nowNs();
  auto drained = session->drain->finish();
  if (session->live) {  // the last chunks are queued: decode them, then the sink has everything
    session->live->finish();
    std::lock_guard lock(m_mutex);
    m_diag.live = session->live->stats();
  }
  const perf::DrainStats drain = session->drain->stats();
  for (Slot& s : session->slots) s.ring.reset();
  for (Slot& s : session->slots) {
    m_system->closeEvent(s.fd);
    s.fd = -1;
  }
  session->slots.clear();
  if (child) {  // a released target keeps running; it is reaped when it exits
    std::lock_guard lock(m_mutex);
    m_reap.push_back(std::move(child));
  }

  perf::SessionInfo info = session->info;
  info.endTs = endTs;
  info.finished = true;
  info.drainedBytes = drain.bytesDrained;
  info.ringOverruns = drain.ringOverruns;
  info.corruptBatches = drain.malformedChunks;
  info.warnings = session->warnings;
  if (!drained.ok()) info.warnings.push_back("closing the spool failed: " + drained.error().message);
  if (drain.ioFailed)
    info.warnings.push_back("spool write failed, the capture is truncated: " + drain.ioError);
  if (drain.throttleRecords > 0)
    TACHYON_LOG_WARN("perf: {}", perf::throttleAdvice(m_diag.sysctls, info.samplingHz));
  if (drain.lostEvents > 0)
    TACHYON_LOG_WARN("perf: {} records lost: the drain thread could not keep up", drain.lostEvents);
  if (auto w = perf::writeSessionInfo(session->spoolDir, info); !w.ok())
    TACHYON_LOG_WARN("perf: cannot update session.json: {}", w.error().toString());

  notify(CaptureState::Converting, 0.0f);
  auto set = perf::SpoolSet::openDirectory(session->spoolDir);
  if (!set.ok()) return finishFailed(set.error());
  perf::ConvertOptions co = convertOptionsFor(options, *session);
  co.progress = [this](float p) { notify(CaptureState::Converting, p); };
  auto stats = perf::convertSpool(*set, options.outPath, co);
  if (!stats.ok())
    return finishFailed(makeError(stats.error().code, "converting " + session->spoolDir.string() + ": " +
                                                          stats.error().message +
                                                          " (the raw spool was kept)"));

  CaptureResult result;
  result.tracePath = options.outPath;
  result.eventsLost = std::max<uint64_t>(stats->eventsLost, drain.lostEvents);
  result.warnings = stats->warnings;
  result.durationS = durationS;
  if (m_config.keepRawSpool) {
    result.rawPath = session->spoolDir;
  } else {
    std::error_code ec;
    fs::remove_all(session->spoolDir, ec);
  }
  for (const std::string& w : stats->warnings) TACHYON_LOG_WARN("perf: {}", w);
  {
    std::lock_guard lock(m_mutex);
    m_diag.drain = drain;
    m_diag.convert = *stats;
    m_diag.warnings = stats->warnings;
    m_diag.targetPids = session->roots;
    m_lifecycle.stopFinished();
    m_state = CaptureState::Done;
  }
  notify(CaptureState::Done, 1.0f);
  return result;
}

Result<CaptureResult> LinuxPerfBackend::flush(std::string name, uint32_t seconds) {
  if (!etw::isValidFlushName(name)) return makeError(ErrorCode::InvalidArgument, "invalid flush name");
  CaptureOptions options;
  perf::Drain* drain = nullptr;
  perf::SessionInfo info;
  fs::path spoolDir;
  perf::ConvertOptions co;
  {
    std::lock_guard lock(m_mutex);
    if (m_state != CaptureState::Capturing || !m_session)
      return makeError(ErrorCode::NotFound, "no perf capture is running");
    TACHYON_TRY(m_lifecycle.beginFlush());
    options = m_options;
    drain = m_session->drain.get();
    info = m_session->info;
    co = convertOptionsFor(options, *m_session);
    spoolDir = m_session->spoolDir;
  }
  auto endFlush = [this]() {
    std::lock_guard lock(m_mutex);
    m_lifecycle.flushRestarted();  // the capture never stops during a flush
  };

  // Seal the current segment. The drain keeps running on a fresh one: there is no gap.
  auto sealed = drain->sealForFlush();
  const uint64_t flushTs = m_system->nowNs();
  if (!sealed.ok()) {
    endFlush();
    return sealed.error();
  }
  const fs::path dir = options.outPath.has_parent_path() ? options.outPath.parent_path() : fs::path(".");
  const fs::path outTrace = dir / (name + ".tachyon");

  perf::SpoolSet set;
  set.dir = spoolDir;
  set.info = info;
  set.info.endTs = flushTs;
  set.info.finished = true;
  set.meta = perf::spoolMetaPath(spoolDir);
  for (const perf::SealedSegment& s : *sealed) set.data.push_back(s.path);
  co.endTs = flushTs;
  co.windowStartTs = seconds > 0 && flushTs > static_cast<uint64_t>(seconds) * 1'000'000'000ull
                         ? flushTs - static_cast<uint64_t>(seconds) * 1'000'000'000ull
                         : 0;
  auto stats = perf::convertSpool(set, outTrace, co);
  if (!stats.ok()) {
    endFlush();
    return makeError(stats.error().code, "converting the flushed segments: " + stats.error().message);
  }

  CaptureResult result;
  result.tracePath = outTrace;
  result.eventsLost = stats->eventsLost;
  result.warnings = stats->warnings;
  result.durationS = static_cast<double>(stats->endTs - std::max(stats->startTs, co.windowStartTs)) / 1e9;
  std::error_code ec;
  if (m_config.keepRawSpool) {
    // <name>.perfspool: the consumed segments plus the metadata they need.
    const fs::path raw = dir / (name + ".perfspool");
    fs::remove_all(raw, ec);
    fs::create_directories(raw, ec);
    fs::copy_file(perf::spoolMetaPath(spoolDir), perf::spoolMetaPath(raw), ec);
    (void)perf::writeSessionInfo(raw, set.info);
    for (const perf::SealedSegment& s : *sealed) fs::copy_file(s.path, raw / s.path.filename(), ec);
    result.rawPath = raw;
  }
  // Retention (retention.h): the horizon is the largest window of the recent flushes, so consecutive flushes
  // each hold the full last `seconds`; segments without timestamps are released by sequence. The circular
  // budget (Drain, circularMb) still drops the oldest segments regardless.
  {
    uint64_t horizon = 0;
    {
      std::lock_guard lock(m_mutex);
      horizon = m_horizon.note(flushTs, seconds);
    }
    drain->release(perf::selectReleasable(*sealed, flushTs, horizon));
  }
  {
    std::lock_guard lock(m_mutex);
    ++m_diag.flushCount;
  }
  endFlush();
  return result;
}

}  // namespace tachyon::capture
