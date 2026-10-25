#include "capture/windows_etw/windows_etw_backend.h"

#include "capture/windows_etw/process_scan.h"
#include "capture/windows_etw/time_window.h"
#include "core/log.h"
#include "core/platform.h"

#include <algorithm>
#include <cmath>
#include <system_error>

namespace tachyon::capture {

namespace fs = std::filesystem;
using etw::EtwSession;
using etw::SessionConfig;
using etw::SessionCounters;
using etw::StopKind;

namespace {

double secondsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

constexpr int kNameRetries = 8;

}  // namespace

WindowsEtwBackend::WindowsEtwBackend(WindowsEtwBackendConfig config) : m_config(std::move(config)) {}

WindowsEtwBackend::~WindowsEtwBackend() {
  // A launched process still parked in its suspended state would never run again.
  std::unique_ptr<etw::TargetProcess> pending;
  {
    std::lock_guard lock(m_mutex);
    if (m_pending) pending = std::move(m_target);
  }
  if (pending) pending->terminate();
}

Result<void> WindowsEtwBackend::checkPrivileges() {
#if TACHYON_PLATFORM_WINDOWS
  return EtwSession::enableProfilePrivilege();
#else
  return makeError(ErrorCode::Unsupported, "the ETW backend requires Windows");
#endif
}

CaptureState WindowsEtwBackend::state() const {
  std::lock_guard lock(m_mutex);
  return m_state;
}

void WindowsEtwBackend::setProgressCallback(std::function<void(CaptureState, float)> callback) {
  std::lock_guard lock(m_mutex);
  m_callback = std::move(callback);
}

WindowsEtwDiagnostics WindowsEtwBackend::diagnostics() const {
  std::lock_guard lock(m_mutex);
  return m_diag;
}

void WindowsEtwBackend::notify(CaptureState state, float progress) {
  std::function<void(CaptureState, float)> cb;
  {
    std::lock_guard lock(m_mutex);
    cb = m_callback;
  }
  if (cb) cb(state, progress);
}

Result<SessionConfig> WindowsEtwBackend::configureSegment(const CaptureOptions& options, uint32_t segment) {
  const uint32_t counter = m_nameCounter.fetch_add(1);
  const uint32_t pid = EtwSession::currentPid();
  const auto stamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  return etw::makeSessionConfig(
      options, etw::makeSessionName(pid, counter), etw::makeSessionGuid(pid, counter, stamp),
      etw::etlSegmentPath(options.outPath, segment), EtwSession::logicalCoreCount(), m_config.buffers);
}

// Names must be unique per machine; another EtwSession in this process (or a stale one) may own ours, so a
// "session already exists" (Busy) result moves on to the next counter value.
Result<std::unique_ptr<EtwSession>> WindowsEtwBackend::startSegment(const CaptureOptions& options,
                                                                    uint32_t segment) {
  Error last = makeError(ErrorCode::Internal, "no attempt made");
  for (int attempt = 0; attempt < kNameRetries; ++attempt) {
    TACHYON_TRY_ASSIGN(SessionConfig cfg, configureSegment(options, segment));
    auto session = EtwSession::start(cfg);
    if (session.ok()) {
      std::lock_guard lock(m_mutex);
      m_sessionConfig = cfg;
      return session;
    }
    last = session.error();
    if (last.code != ErrorCode::Busy) break;
  }
  return last;
}

Result<void> WindowsEtwBackend::start(const CaptureOptions& options) {
#if !TACHYON_PLATFORM_WINDOWS
  TACHYON_TRY(etw::validateCaptureOptions(options));
  return makeError(ErrorCode::Unsupported, "the ETW backend requires Windows");
#else
  // Second start of a deferred launch: begin recording; everything but the fact of the call is ignored.
  {
    std::unique_lock lock(m_mutex);
    if (m_pending) {
      if (!options.startImmediately)
        return makeError(ErrorCode::Busy, "a launched process is already waiting for the recording to start");
      const CaptureOptions launchedOptions = m_options;
      std::unique_ptr<etw::TargetProcess> target = std::move(m_target);
      m_pending = false;
      lock.unlock();
      TACHYON_LOG_INFO("etw: recording starts for deferred launch of pid {}", launchedOptions.pid);
      return startRecording(launchedOptions, std::move(target), false);
    }
  }

  TACHYON_TRY(etw::validateCaptureOptions(options));
  if (options.mode == CaptureMode::Attach && !EtwSession::processAlive(options.pid))
    return makeError(ErrorCode::NotFound,
                     "attach target pid " + std::to_string(options.pid) + " is not running");
  {
    std::lock_guard lock(m_mutex);
    TACHYON_TRY(m_lifecycle.beginStart());
    m_state = CaptureState::Starting;
  }
  auto fail = [this](Error e) -> Result<void> {
    std::lock_guard lock(m_mutex);
    m_lifecycle.startFailed();
    m_state = CaptureState::Idle;
    return e;
  };

  if (m_config.cleanupOrphans) {
    auto report = EtwSession::cleanupOrphans();
    if (report.ok()) {
      std::lock_guard lock(m_mutex);
      m_diag.lastOrphanReport = *report;
    } else {
      TACHYON_LOG_WARN("etw: orphan cleanup failed: {}", report.error().toString());
    }
  }

  CaptureOptions effective = options;
  std::unique_ptr<etw::TargetProcess> target;
  bool launchNow = false;
  if (options.mode == CaptureMode::Launch) {
    if (options.startImmediately) {
      // Session first, process second (see the class comment): the exe and ntdll images and the process
      // start are then logged live, which a rundown at stop time cannot reproduce once the process exited.
      launchNow = true;
    } else {
      // Deferred: the process is created now and parked; the session starts at the second start().
      auto proc = etw::TargetProcess::launchSuspended(launchSpecFor(options));
      if (!proc.ok()) return fail(proc.error());
      target = std::make_unique<etw::TargetProcess>(std::move(*proc));
      effective.pid = target->pid();
      TACHYON_LOG_INFO("etw: launched '{}' parked as pid {}{}", options.exe, effective.pid,
                       target->tracksDescendants() ? " (job object)" : "");
      std::lock_guard lock(m_mutex);
      m_options = effective;
      m_target = std::move(target);
      m_pending = true;
      m_diag.rootPid = effective.pid;
      return ok();  // lifecycle and state stay Starting until the second start()
    }
  } else if (options.mode == CaptureMode::Attach) {
    auto proc = etw::TargetProcess::open(options.pid);
    if (proc.ok()) {
      target = std::make_unique<etw::TargetProcess>(std::move(*proc));
    } else {
      TACHYON_LOG_WARN("etw: attach target {} cannot be watched for exit: {}", options.pid,
                       proc.error().toString());
    }
  }
  return startRecording(effective, std::move(target), launchNow);
#endif
}

etw::LaunchSpec WindowsEtwBackend::launchSpecFor(const CaptureOptions& options) {
  etw::LaunchSpec spec;
  spec.exe = options.exe;
  spec.args = options.args;
  spec.cwd = options.cwd;
  spec.env = options.env;
  return spec;
}

Result<void> WindowsEtwBackend::startRecording(const CaptureOptions& requested,
                                               std::unique_ptr<etw::TargetProcess> target, bool launchNow) {
  CaptureOptions options = requested;
  const bool resumeNeeded = options.mode == CaptureMode::Launch;  // parked or freshly launched: suspended
  auto fail = [this, &target, resumeNeeded](Error e) -> Result<void> {
    if (target && resumeNeeded)
      target->terminate();  // never leave a suspended process behind (not an attach)
    std::lock_guard lock(m_mutex);
    m_lifecycle.startFailed();
    m_state = CaptureState::Idle;
    m_pending = false;
    return e;
  };

  auto session = startSegment(options, 0);
  if (!session.ok()) return fail(session.error());
  if (launchNow) {
    auto proc = etw::TargetProcess::launchSuspended(launchSpecFor(options));
    if (!proc.ok()) {
      const fs::path etl = (*session)->config().etlPath;
      (void)(*session)->stop();
      std::error_code ec;
      fs::remove(etl, ec);
      return fail(proc.error());
    }
    target = std::make_unique<etw::TargetProcess>(std::move(*proc));
    options.pid = target->pid();
    TACHYON_LOG_INFO("etw: launched '{}' suspended as pid {}{}", options.exe, options.pid,
                     target->tracksDescendants() ? " (job object)" : "");
  }
  // The session is live: now let the target run, so its first instruction is inside the capture.
  if (resumeNeeded && target) {
    auto resumed = target->resume();
    if (!resumed.ok()) {
      (void)(*session)->stop();
      return fail(resumed.error());
    }
  }

  std::lock_guard lock(m_mutex);
  m_options = options;
  m_session = std::move(*session);
  m_target = std::move(target);
  m_diag.startInfo = m_session->startInfo();
  m_diag.sessionName = m_session->config().name;
  m_diag.lastFlushGapSeconds = -1.0;
  m_diag.flushCount = 0;
  m_diag.rootPid = options.mode == CaptureMode::System ? 0 : options.pid;
  m_diag.followedChildren.clear();
  m_diag.targetPids.clear();
  m_startTime = std::chrono::steady_clock::now();
  m_lifecycle.startSucceeded(m_session->config().circular);
  m_state = CaptureState::Capturing;
  TACHYON_LOG_INFO("etw: session '{}' started ({} mode, {} Hz, {} buffers x {} KB{})", m_diag.sessionName,
                   captureModeName(options.mode),
                   etw::intervalUnitsToHz(m_session->config().profileIntervalUnits),
                   m_session->config().buffers.maximumBuffers, m_session->config().buffers.bufferSizeKb,
                   m_session->config().circular ? ", circular" : "");
  return ok();
}

bool WindowsEtwBackend::launchPending() const {
  std::lock_guard lock(m_mutex);
  return m_pending;
}

bool WindowsEtwBackend::targetExited() const {
  std::lock_guard lock(m_mutex);
  if (m_state != CaptureState::Capturing || !m_target) return false;
  return m_options.followChildren ? m_target->allExited() : m_target->exited();
}

uint32_t WindowsEtwBackend::rootPid() const {
  std::lock_guard lock(m_mutex);
  if (!m_pending && m_state != CaptureState::Capturing) return 0;
  return m_options.mode == CaptureMode::System ? 0 : m_options.pid;
}

etlconvert::ConvertOptions WindowsEtwBackend::convertOptionsFor(const CaptureOptions& options,
                                                                const SessionConfig& cfg,
                                                                const SessionCounters& counters) {
  etlconvert::ConvertOptions co;
  if (options.mode != CaptureMode::System) {
    co.targetPids = {options.pid};
    co.mode = etlconvert::CaptureMode::Process;
    if (options.followChildren) {
      // The converter filters by a fixed pid list: complete it from the ETL's process events.
      etw::ProcessTreeTracker tracker({options.pid}, true, options.childFilter);
      auto scanned = etw::scanEtlProcessEvents(cfg.etlPath, tracker);
      if (scanned.ok()) {
        co.targetPids = tracker.targets();
        TACHYON_LOG_INFO("etw: following {} child process(es) of pid {} ({} rejected by filter '{}')",
                         tracker.children().size(), options.pid, tracker.rejectedChildren(),
                         options.childFilter);
        std::lock_guard lock(m_mutex);
        m_diag.followedChildren = tracker.children();
      } else {
        TACHYON_LOG_WARN("etw: child scan failed, converting the root only: {}", scanned.error().toString());
      }
    }
    std::lock_guard lock(m_mutex);
    m_diag.targetPids = co.targetPids;
  } else {
    co.mode = etlconvert::CaptureMode::System;
  }
  co.samplingHz = static_cast<uint32_t>(std::lround(etw::intervalUnitsToHz(cfg.profileIntervalUnits)));
  co.eventsLost = counters.eventsLost;
  co.captureOptionsJson = etw::captureOptionsJson(options, cfg, co.targetPids);
  co.agentVersion = m_config.agentVersion;
  return co;
}

Result<CaptureResult> WindowsEtwBackend::stop() {
  std::unique_ptr<EtwSession> session;
  std::unique_ptr<etw::TargetProcess> target;
  CaptureOptions options;
  SessionConfig cfg;
  double durationS = 0.0;
  {
    std::lock_guard lock(m_mutex);
    if (m_pending) {  // launched but never recorded: nothing to convert, the parked process is useless
      std::unique_ptr<etw::TargetProcess> parked = std::move(m_target);
      m_pending = false;
      m_lifecycle.startFailed();
      m_state = CaptureState::Idle;
      if (parked) parked->terminate();
      return makeError(ErrorCode::NotFound,
                       "the launched process never started recording; it was terminated");
    }
    auto kind = m_lifecycle.beginStop();
    if (!kind.ok()) return kind.error();
    if (*kind == StopKind::NoSession) {
      m_lifecycle.stopFinished();
      m_state = CaptureState::Idle;
      return makeError(ErrorCode::Internal,
                       "the ETW session was lost: restarting it after a flush failed; nothing to stop");
    }
    session = std::move(m_session);
    target = std::move(m_target);  // released below; the process itself keeps running
    options = m_options;
    cfg = m_sessionConfig;
    durationS = secondsSince(m_startTime);
    m_state = CaptureState::Converting;
  }

  auto finishFailed = [this](Error e) -> Result<CaptureResult> {
    std::lock_guard lock(m_mutex);
    m_lifecycle.stopFinished();
    m_state = CaptureState::Idle;
    return e;
  };

  auto counters = session->stop();
  if (!counters.ok()) return finishFailed(counters.error());
  const fs::path etl = session->config().etlPath;
  session.reset();
  target.reset();
  if (counters->eventsLost != 0 || counters->logBuffersLost != 0 || counters->realTimeBuffersLost != 0)
    TACHYON_LOG_WARN("etw: data loss: {} events, {} log buffers, {} real-time buffers", counters->eventsLost,
                     counters->logBuffersLost, counters->realTimeBuffersLost);

  notify(CaptureState::Converting, 0.0f);
  etlconvert::ConvertOptions co = convertOptionsFor(options, cfg, *counters);
  co.progress = [this](float p) { notify(CaptureState::Converting, p); };

  std::error_code ec;
  if (options.outPath.has_parent_path()) fs::create_directories(options.outPath.parent_path(), ec);
  auto stats = etlconvert::EtlConverter::convert(etl, options.outPath, co);
  if (!stats.ok())
    return finishFailed(
        makeError(stats.error().code, "converting " + etl.string() + ": " + stats.error().message));

  CaptureResult result;
  result.tracePath = options.outPath;
  result.eventsLost = std::max<uint64_t>(counters->eventsLost, stats->eventsLost);
  result.durationS = durationS;
  if (m_config.keepRawEtl) {
    result.rawPath = etl;
  } else {
    fs::remove(etl, ec);
  }
  {
    std::lock_guard lock(m_mutex);
    m_diag.lastCounters = *counters;
    m_diag.lastConvertStats = *stats;
    m_lifecycle.stopFinished();
    m_state = CaptureState::Done;
  }
  notify(CaptureState::Done, 1.0f);
  return result;
}

Result<CaptureResult> WindowsEtwBackend::flush(std::string name, uint32_t seconds) {
  if (!etw::isValidFlushName(name)) return makeError(ErrorCode::InvalidArgument, "invalid flush name");

  std::unique_ptr<EtwSession> old;
  CaptureOptions options;
  SessionConfig oldCfg;
  uint32_t segment = 0;
  double sinceStartS = 0.0;
  {
    std::lock_guard lock(m_mutex);
    if (m_state != CaptureState::Capturing)
      return makeError(ErrorCode::NotFound, "no ETW capture is running");
    TACHYON_TRY(m_lifecycle.beginFlush());
    old = std::move(m_session);
    options = m_options;
    oldCfg = m_sessionConfig;
    segment = m_lifecycle.segment();
    sinceStartS = secondsSince(m_startTime);
  }

  // 1. Write everything buffered, then stop. The stopped file is a complete ETL (end rundown included).
  auto flushed = old->flushBuffers();
  if (!flushed.ok()) TACHYON_LOG_WARN("etw: flush before stop failed: {}", flushed.error().toString());
  auto counters = old->stop();
  const auto stoppedAt = std::chrono::steady_clock::now();
  const fs::path oldEtl = oldCfg.etlPath;
  if (!counters.ok()) {
    std::lock_guard lock(m_mutex);
    m_lifecycle.flushRestartFailed();
    return counters.error();
  }
  old.reset();

  // 2. Restart on the next segment. The time between the stop above and the new session being configured is
  //    the capture gap.
  auto next = startSegment(options, segment + 1);
  const double gapS = secondsSince(stoppedAt);
  {
    std::lock_guard lock(m_mutex);
    if (next.ok()) {
      m_session = std::move(*next);
      m_lifecycle.flushRestarted();
      m_diag.lastFlushGapSeconds = gapS;
      ++m_diag.flushCount;
      m_diag.sessionName = m_session->config().name;
      TACHYON_LOG_INFO("etw: flush '{}' restarted session '{}' after a {:.1f} ms gap", name,
                       m_diag.sessionName, gapS * 1000.0);
    } else {
      m_lifecycle.flushRestartFailed();
      TACHYON_LOG_ERROR("etw: could not restart the session after flush: {}", next.error().toString());
    }
  }

  // 3. Convert the stopped segment, keeping the last `seconds`. The new session is already recording.
  const fs::path dir = options.outPath.has_parent_path() ? options.outPath.parent_path() : fs::path(".");
  const fs::path outTrace = dir / (name + ".tachyon");
  etlconvert::ConvertOptions co = convertOptionsFor(options, oldCfg, *counters);
  auto stats = etw::convertEtlLastSeconds(oldEtl, outTrace, co, seconds);
  if (!stats.ok())
    return makeError(stats.error().code, "converting " + oldEtl.string() + ": " + stats.error().message);

  CaptureResult result;
  result.tracePath = outTrace;
  result.eventsLost = std::max<uint64_t>(counters->eventsLost, stats->eventsLost);
  const double freq = stats->clockFrequencyHz != 0 ? static_cast<double>(stats->clockFrequencyHz) : 1.0;
  result.durationS =
      stats->endTs > stats->startTs ? static_cast<double>(stats->endTs - stats->startTs) / freq : sinceStartS;
  std::error_code ec;
  if (m_config.keepRawEtl) {
    const fs::path raw = dir / (name + ".etl");
    fs::remove(raw, ec);
    fs::rename(oldEtl, raw, ec);
    result.rawPath = ec ? oldEtl : raw;
  } else {
    fs::remove(oldEtl, ec);
  }
  {
    std::lock_guard lock(m_mutex);
    m_diag.lastCounters = *counters;
  }
  return result;
}

}  // namespace tachyon::capture
