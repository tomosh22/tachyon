#include "agent/agent.h"

#include "capture/windows_etw/launch.h"
#include "control/transport.h"
#include "core/log.h"
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

namespace tachyon::agent {

using capture::CaptureState;
using namespace control;

namespace {

// The exe of the start request that triggers a parked launch (see agent.h).
constexpr const char* kResumeExe = "-";

CaptureSummary toSummary(const capture::CaptureResult& r, std::vector<std::string> warnings = {}) {
  CaptureSummary s;
  s.path = r.tracePath.string();
  s.events_lost = r.eventsLost;
  s.duration_s = r.durationS;
  // The backend's own warnings come first, then the instrumentation merge's; the cap applies to the total.
  std::vector<std::string> all = r.warnings;
  all.insert(all.end(), std::make_move_iterator(warnings.begin()), std::make_move_iterator(warnings.end()));
  s.warnings = capSummaryWarnings(std::move(all));
  return s;
}

// A safe file stem: [A-Za-z0-9._-] stay, anything else becomes '_', no leading dots, at most `maxChars`.
std::string sanitizeFileStem(const std::string& in, size_t maxChars = 120) {
  std::string out;
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '_' || c == '-';
    out.push_back(plain ? ch : '_');
    if (out.size() >= maxChars) break;
  }
  size_t lead = 0;
  while (lead < out.size() && out[lead] == '.') ++lead;
  out.erase(0, lead);
  while (!out.empty() && out.back() == '.') out.pop_back();
  // Windows device names stay devices with any extension ("CON.tachyon"): prefix them.
  std::string base = out.substr(0, out.find('.'));
  for (char& c : base) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const bool numbered = base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) &&
                        base[3] >= '1' && base[3] <= '9';
  if (numbered || base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") out.insert(0, "_");
  return out;
}

// A JSON string literal for `in`.
std::string jsonQuote(const std::string& in) {
  std::string out = "\"";
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out += "\"";
  return out;
}

std::string timestampText() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if TACHYON_PLATFORM_WINDOWS
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
  return buf;
}

// The file name of `exe` without directory and extension, made safe for a file name ("" when it has none).
std::string exeStem(const std::string& exe) {
  try {
    return sanitizeFileStem(std::filesystem::path(exe).stem().string(), 40);
  } catch (const std::exception&) {  // a name the narrow code page cannot represent
    return {};
  }
}

// Name of the process that asked for a flush: the launched exe, else the OS's name for the pid.
std::string processNameFor(uint32_t pid, const std::string& launchedExeStem) {
  if (!launchedExeStem.empty()) return launchedExeStem;
#if TACHYON_PLATFORM_LINUX
  if (pid != 0) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    std::string comm;
    if (in && std::getline(in, comm) && !comm.empty()) return comm;
  }
#elif TACHYON_PLATFORM_WINDOWS
  if (pid != 0) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
      wchar_t buffer[MAX_PATH * 4];
      DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
      std::string stem;
      if (QueryFullProcessImageNameW(process, 0, buffer, &size) != 0 && size > 0) {
        try {
          stem = sanitizeFileStem(std::filesystem::path(std::wstring(buffer, size)).stem().string(), 40);
        } catch (const std::exception&) {  // a name the narrow code page cannot represent
        }
      }
      CloseHandle(process);
      if (!stem.empty()) return stem;
    }
  }
#endif
  return pid != 0 ? "pid" + std::to_string(pid) : "process";
}

}  // namespace

Agent::Agent(std::unique_ptr<capture::ICaptureBackend> backend, AgentConfig config)
    : m_backend(std::move(backend)), m_config(std::move(config)) {
  m_backend->setProgressCallback([this](CaptureState s, float progress) {
    if (s != CaptureState::Converting || m_state.load() != CaptureState::Converting) return;
    std::lock_guard lock(m_mutex);
    if (m_state.load() == CaptureState::Converting) setStateLocked(CaptureState::Converting, progress);
  });
  m_watch = dynamic_cast<const capture::ITargetWatch*>(m_backend.get());
  if (m_watch != nullptr) m_watcher = std::thread([this] { watcherLoop(); });
}

Agent::~Agent() {
  shutdown();
}

void Agent::setEventSink(EventSink sink) {
  m_sink = std::move(sink);
}

void Agent::emit(const Event& event) {
  if (m_sink) m_sink(event);
}

void Agent::setStateLocked(CaptureState state, double progress) {
  m_state.store(state);
  m_progress.store(progress);
  emit(StateEvent{state, progress});
}

Response Agent::handle(ClientId client, const Request& request) {
  (void)client;
  const std::optional<int64_t> id = request.id;
  Response resp;
  if (std::holds_alternative<StatusRequest>(request.body)) {
    resp = handleStatus(id);
  } else if (const auto* s = std::get_if<StartRequest>(&request.body)) {
    resp = handleStart(*s);
  } else if (std::holds_alternative<StopRequest>(request.body)) {
    resp = handleStop();
  } else if (const auto* f = std::get_if<FlushRequest>(&request.body)) {
    resp = handleFlush(*f);
  } else if (std::holds_alternative<CancelRequest>(request.body)) {
    resp = handleCancel();
  } else if (const auto* h = std::get_if<InstHelloRequest>(&request.body)) {
    resp = handleInstHello(*h);
  }
  resp.id = id;
  return resp;
}

Response Agent::handleStatus(std::optional<int64_t> id) {
  StatusBody body;
  body.state = m_state.load();
  body.progress = m_progress.load();
  body.backend = m_config.backendName;
  body.elevated = m_config.elevated;
  return makeOkResponse(id, body);
}

bool Agent::armedLocked() const {
  return m_state.load() == CaptureState::Starting && m_watch != nullptr && m_watch->launchPending();
}

Response Agent::handleStart(const StartRequest& req) {
  capture::CaptureOptions opts;
  opts.mode = req.mode;
  opts.exe = req.exe;
  opts.cwd = req.cwd;
  opts.args = req.args;
  opts.env = req.env;
  opts.pid = req.pid;
  opts.samplingHz = req.sampling_hz;
  opts.followChildren = req.follow_children;
  opts.childFilter = req.child_filter;
  opts.circularMb = req.circular_mb;
  opts.startImmediately = req.start_immediately;
  opts.stackProfile = req.stackwalk.profile;
  opts.stackCSwitch = req.stackwalk.cswitch;
  opts.stackReady = req.stackwalk.readythread;
  opts.kernelStacks = req.kernel_stacks;
  if (opts.mode == capture::CaptureMode::Launch && !m_config.agentAddress.empty())
    opts.env[std::string(capture::etw::kAgentPipeEnvVar)] = m_config.agentAddress;  // the SDK connects here
  if (req.out_path.empty()) {
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    opts.outPath = "capture-" + std::to_string(now) + ".tachyon";
  } else {
    opts.outPath = req.out_path;
  }

  bool resuming = false;
  {
    std::lock_guard lock(m_mutex);
    // exe "-" is the CLI's `resume`: a trigger for a parked launch, never a program to start.
    if (opts.mode == capture::CaptureMode::Launch && opts.exe == kResumeExe &&
        !(armedLocked() && !m_resuming))
      return makeErrorResponse(
          std::nullopt, makeError(ErrorCode::NotFound,
                                  "no parked launch to resume (start one with --launch ... --no-start)"));
    if (armedLocked() && !m_resuming) {
      // The trigger of a deferred launch (see the class comment).
      if (!req.start_immediately)
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy,
                                                         "a launched process is already waiting to be "
                                                         "started; send start with start_immediately"));
      m_resuming = true;
      resuming = true;
    } else {
      if (m_state.load() != CaptureState::Idle)
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "a capture is already in progress"));
      setStateLocked(CaptureState::Starting, 0.0);
      inst::IngestConfig ingestConfig;
      ingestConfig.spoolDir = m_config.instSpoolRoot.empty()
                                  ? std::filesystem::path(opts.outPath.string() + ".inst")
                                  : m_config.instSpoolRoot / (opts.outPath.filename().string() + ".inst");
      ingestConfig.maxSpoolBytes = m_config.maxInstSpoolBytes;
      ingestConfig.drainIdleMs = m_config.instDrainIdleMs;
      ingestConfig.drainMaxMs = m_config.instDrainMaxMs;
      ingestConfig.maxProcesses = m_config.maxInstProcesses;
      ingestConfig.maxConnectionsPerProcess = m_config.maxInstConnectionsPerProcess;
      ingestConfig.maxTotalSpoolBytes = m_config.maxInstTotalSpoolBytes;
      ingestConfig.verifyPeerPid = m_config.verifyInstPeerPid;
      if (opts.circularMb > 0) {
        ingestConfig.ringBytes =
            std::max<uint64_t>(static_cast<uint64_t>(opts.circularMb) << 20, m_config.minRingBytes);
        ingestConfig.ringSegmentBytes = m_config.ringSegmentBytes;
      }
      m_sessionOutDir =
          opts.outPath.has_parent_path() ? opts.outPath.parent_path() : std::filesystem::path(".");
      m_sessionExeStem = opts.mode == capture::CaptureMode::Launch ? exeStem(opts.exe) : std::string();
      m_loggedNotCircular = false;
      m_haveTrigger = false;
      m_lastFlushPath.clear();
      m_coalesced = 0;
      m_sessionMode = opts.mode;
      m_sessionPid = opts.pid;
      m_sessionFollow = opts.followChildren;
      m_ingest = std::make_shared<inst::Ingest>(std::move(ingestConfig));
      m_circular = opts.circularMb > 0;
    }
  }

  Result<void> started = resuming ? ok() : m_backend->checkPrivileges();
  if (started.ok()) started = m_backend->start(opts);

  std::shared_ptr<inst::Ingest> abandoned;  // destroyed after the lock below is released
  std::lock_guard lock(m_mutex);
  if (resuming) m_resuming = false;
  if (!started.ok()) {
    TACHYON_LOG_WARN("agent: start failed: {}", started.error().toString());
    // A failed trigger can leave the launch parked (the backend decides); anything else is idle again.
    if (!(resuming && m_watch->launchPending())) {
      setStateLocked(CaptureState::Idle, 0.0);
      abandoned = std::move(m_ingest);
    }
    return makeErrorResponse(std::nullopt, started.error());
  }
  if (!resuming && opts.mode == capture::CaptureMode::Launch && !opts.startImmediately &&
      m_watch != nullptr && m_watch->launchPending()) {
    TACHYON_LOG_INFO("agent: launched {} (pid {}) parked; waiting for start", opts.exe, m_watch->rootPid());
    return makeOkResponse(std::nullopt);  // state stays `starting`
  }
  TACHYON_LOG_INFO("agent: capture started ({}) -> {}", capture::captureModeName(opts.mode),
                   opts.outPath.string());
  setStateLocked(CaptureState::Capturing, 0.0);
  return makeOkResponse(std::nullopt);
}

void Agent::beginConversionLocked() {
  setStateLocked(CaptureState::Converting, 0.0);
  if (m_worker.joinable()) m_worker.join();  // previous worker has already finished all locked work
  m_worker = std::thread([this] { conversionWorker(); });
}

void Agent::watcherLoop() {
  std::unique_lock lock(m_mutex);
  while (!m_watchStop) {
    m_watchCv.wait_for(lock, std::chrono::milliseconds(50), [this] { return m_watchStop; });
    if (m_watchStop) break;
    if (m_state.load() != CaptureState::Capturing || m_flushing) continue;
    lock.unlock();
    const bool exited = m_watch->targetExited();  // never under m_mutex: it takes the backend's lock
    lock.lock();
    if (!exited || m_watchStop || m_state.load() != CaptureState::Capturing || m_flushing) continue;
    TACHYON_LOG_INFO("agent: capture target exited, finishing the capture");
    beginConversionLocked();
  }
}

Response Agent::handleStop() {
  std::unique_lock lock(m_mutex);
  m_flushDone.wait(lock, [this] { return !m_flushing; });  // a flush in flight finishes first
  switch (m_state.load()) {
    case CaptureState::Idle:
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no capture is running"));
    case CaptureState::Capturing: break;
    case CaptureState::Starting:
      if (armedLocked())
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy,
                                                         "the launched process is not recording yet; send "
                                                         "start to begin or cancel to terminate it"));
      return makeErrorResponse(std::nullopt,
                               makeError(ErrorCode::Busy, "capture is not in a stoppable state"));
    default:
      return makeErrorResponse(std::nullopt,
                               makeError(ErrorCode::Busy, "capture is not in a stoppable state"));
  }
  beginConversionLocked();
  return makeOkResponse(std::nullopt);
}

std::shared_ptr<inst::Ingest> Agent::currentIngest() {
  std::lock_guard lock(m_mutex);
  return m_ingest;
}

std::shared_ptr<inst::Ingest> Agent::takeIngest(bool& circular) {
  std::lock_guard lock(m_mutex);
  circular = m_circular;
  return std::move(m_ingest);
}

std::vector<std::string> Agent::mergeSpools(inst::Ingest& ingest, const std::vector<inst::SpoolInfo>& spools,
                                            const capture::CaptureResult& result, bool clip,
                                            bool removeWhenDone, const std::string& captureInfoPatch) {
  std::vector<std::string> warnings;
  if (const uint32_t rejected = ingest.rejectedConnections(); rejected > 0)
    warnings.push_back(
        std::to_string(rejected) +
        " instrumentation connections were refused (connection cap, wrong peer pid or no thread)");
  if (spools.empty() && captureInfoPatch.empty()) {
    if (removeWhenDone) ingest.removeSpoolDir();
    return warnings;
  }
  std::vector<inst::MergeInput> inputs;
  for (const inst::SpoolInfo& info : spools) {
    if (info.segments.empty()) {
      inputs.push_back({info.path, info.flushedBytes});
    } else {
      for (const inst::SpoolSegment& seg : info.segments) inputs.push_back({seg.file, seg.bytes});
    }
    if (info.stats.overflowed) {
      TACHYON_LOG_WARN("agent: the instrumentation spool of pid {} reached its cap; {} records dropped",
                       info.pid, info.stats.recordsDropped);
      warnings.push_back("instrumentation spool of pid " + std::to_string(info.pid) + " hit its size cap; " +
                         std::to_string(info.stats.recordsDropped) + " records dropped");
    }
  }
  inst::MergeOptions options;
  options.clipToTraceRange = clip;
  options.balanceClippedScopes = clip;
  options.captureInfoPatch = captureInfoPatch;
  auto merged = inst::mergeInstrumentation(result.tracePath, inputs, options);
  if (!merged.ok()) {
    TACHYON_LOG_ERROR("agent: merging instrumentation into {} failed: {}; spool files kept in {}",
                      result.tracePath.string(), merged.error().toString(), ingest.spoolDir().string());
    warnings.push_back("instrumentation merge failed (" + merged.error().message + "); spool files kept in " +
                       ingest.spoolDir().string());
    return warnings;
  }
  TACHYON_LOG_INFO(
      "agent: merged instrumentation into {}: {} scope events, {} fiber events, {} frame marks, {} markers, "
      "{} plot samples, {} thread names",
      result.tracePath.string(), merged->scopeEvents, merged->fiberEvents, merged->frameMarks,
      merged->markers, merged->plots, merged->threadNames);
  for (const std::string& warning : merged->warnings) {
    TACHYON_LOG_WARN("agent: instrumentation: {}", warning);
    warnings.push_back("instrumentation: " + warning);
  }
  if (removeWhenDone && !m_config.keepInstSpool) ingest.removeSpoolDir();
  return warnings;
}

Result<capture::CaptureResult> Agent::stopAndMerge(std::vector<std::string>& warnings) {
  bool circular = false;
  std::shared_ptr<inst::Ingest> ingest = takeIngest(circular);
  // Drain the SDK connections first: what the processes had already sent belongs to the capture.
  std::vector<inst::SpoolInfo> spools;
  if (ingest) spools = ingest->finish();
  auto result = m_backend->stop();
  if (ingest) {
    if (result.ok()) {
      warnings = mergeSpools(*ingest, spools, *result, circular, true);
    } else {
      ingest->removeSpoolDir();
    }
  }
  return result;
}

void Agent::conversionWorker() {
  std::vector<std::string> warnings;
  auto result = stopAndMerge(warnings);
  std::lock_guard lock(m_mutex);
  if (result.ok()) {
    TACHYON_LOG_INFO("agent: capture done: {} ({} events lost)", result->tracePath.string(),
                     result->eventsLost);
    setStateLocked(CaptureState::Done, 1.0);
    finishCaptureLocked(CaptureDoneEvent{toSummary(*result, std::move(warnings))});
  } else {
    TACHYON_LOG_ERROR("agent: conversion failed: {}", result.error().toString());
    finishCaptureLocked(ErrorEvent{result.error().message});
  }
}

void Agent::finishCaptureLocked(const Event& finalEvent) {
  // The state is idle before anyone can see the final event, so a client that reacts to capture_done (or
  // error) with a status request or a new start never meets `done`. The event order is unchanged:
  // state(done), capture_done, state(idle).
  m_state.store(CaptureState::Idle);
  m_progress.store(0.0);
  emit(finalEvent);
  emit(StateEvent{CaptureState::Idle, 0.0});
}

std::string Agent::uniqueFlushName(const std::string& requested) const {
  std::string stem = sanitizeFileStem(requested);
  if (stem.empty()) stem = "flush";
  const std::filesystem::path& dir = m_sessionOutDir;  // caller holds m_mutex
  std::error_code ec;
  std::string candidate = stem;
  for (int n = 1; n < 10000 && std::filesystem::exists(dir / (candidate + ".tachyon"), ec); ++n)
    candidate = stem + "-" + std::to_string(n);
  return candidate;
}

void Agent::endFlush() {
  {
    std::lock_guard lock(m_mutex);
    m_flushing = false;
  }
  m_flushDone.notify_all();
}

Result<CaptureSummary> Agent::performFlush(const FlushJob& job) {
  if (job.trigger && m_config.flushSettleMs > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(m_config.flushSettleMs));
  // No lock across the backend call (it may take seconds and may call the progress callback).
  auto result = m_backend->flush(job.name, job.seconds);
  if (!result.ok()) return result.error();
  // The flushed trace covers a window of the capture: add what the instrumented processes sent so far,
  // clipped to that window. The spools keep growing for the rest of the capture.
  const std::string patch = "{\"flush_reason\":" + jsonQuote(job.reason) +
                            ",\"flush_name\":" + jsonQuote(job.name) +
                            ",\"flush_window_s\":" + std::to_string(job.seconds) +
                            ",\"flush_trigger\":" + (job.trigger ? "true" : "false") + "}";
  std::vector<std::string> flushWarnings;
  if (std::shared_ptr<inst::Ingest> ingest = currentIngest()) {
    const std::vector<inst::SpoolInfo> spools = ingest->snapshot();
    flushWarnings = mergeSpools(*ingest, spools, *result, true, false, patch);
  }
  CaptureSummary summary = toSummary(*result, std::move(flushWarnings));
  FlushedEvent event;
  event.path = summary.path;
  event.name = job.name;
  event.reason = job.reason;
  event.duration_s = summary.duration_s;
  event.events_lost = summary.events_lost;
  {
    std::lock_guard lock(m_mutex);
    event.coalesced = m_coalesced;
    m_coalesced = 0;
    m_lastFlushPath = summary.path;
    emit(event);
  }
  TACHYON_LOG_INFO("agent: flushed '{}' ({} s window) -> {}", job.reason, job.seconds, summary.path);
  return summary;
}

Response Agent::handleFlush(const FlushRequest& req) {
  FlushJob job;
  job.trigger = req.trigger;
  {
    std::lock_guard lock(m_mutex);
    switch (m_state.load()) {
      case CaptureState::Idle:
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no capture is running"));
      case CaptureState::Capturing: break;
      default:
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "capture is not in a flushable state"));
    }
    if (!m_circular) {
      if (req.trigger && !m_loggedNotCircular) {
        m_loggedNotCircular = true;
        TACHYON_LOG_WARN(
            "agent: flush requested by pid {} ('{}') but the capture is not circular; start it with "
            "--circular-mb to use the flight recorder",
            req.pid, req.reason.empty() ? req.name : req.reason);
      }
      return makeErrorResponse(
          std::nullopt,
          makeError(ErrorCode::InvalidArgument, "flush requires a circular capture (circular_mb > 0)"));
    }
    const auto now = std::chrono::steady_clock::now();
    if (req.trigger) {
      // Rate limit: one flush at a time, and a pause after the previous accepted trigger.
      const bool tooSoon =
          m_haveTrigger && now - m_lastTrigger < std::chrono::milliseconds(m_config.flushCoalesceMs);
      if (m_flushing || tooSoon) {
        ++m_coalesced;
        CaptureSummary previous;
        previous.path = m_lastFlushPath;
        TACHYON_LOG_DEBUG("agent: flush trigger '{}' coalesced ({} so far)", req.reason, m_coalesced);
        return makeOkResponse(std::nullopt, previous);
      }
      m_haveTrigger = true;
      m_lastTrigger = now;
      job.reason = req.reason.empty() ? req.name : req.reason;
      job.seconds = m_config.flushSeconds;
      job.name = uniqueFlushName(processNameFor(req.pid, m_sessionExeStem) + "-" +
                                 sanitizeFileStem(job.reason, 40) + "-" + timestampText());
    } else {
      if (m_flushing)
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy, "a flush is already in progress"));
      job.reason = req.reason.empty() ? req.name : req.reason;
      job.seconds = req.seconds;
      job.name = uniqueFlushName(req.name);
    }
    job.coalesced = m_coalesced;
    m_flushing = true;  // stop / cancel / shutdown wait for it instead of interleaving
    if (req.trigger) {
      // Answer now, flush in the background: the requester is the engine's main thread.
      CaptureSummary planned;
      planned.path = (m_sessionOutDir / (job.name + ".tachyon")).string();
      m_lastFlushPath = planned.path;
      if (m_flushWorker.joinable()) m_flushWorker.join();  // the previous one is done (m_flushing was false)
      m_flushWorker = std::thread([this, job] {
        auto flushed = performFlush(job);
        if (!flushed.ok()) {
          TACHYON_LOG_ERROR("agent: flush '{}' failed: {}", job.name, flushed.error().toString());
          std::lock_guard lock(m_mutex);
          emit(ErrorEvent{"flush '" + job.name + "' failed: " + flushed.error().message});
        }
        endFlush();
      });
      return makeOkResponse(std::nullopt, planned);
    }
  }
  struct FlushGuard {
    Agent& agent;
    ~FlushGuard() { agent.endFlush(); }
  } flushGuard{*this};
  auto summary = performFlush(job);
  if (!summary.ok()) return makeErrorResponse(std::nullopt, summary.error());
  return makeOkResponse(std::nullopt, *summary);
}

Response Agent::handleCancel() {
  {
    std::unique_lock lock(m_mutex);
    m_flushDone.wait(lock, [this] { return !m_flushing; });
    switch (m_state.load()) {
      case CaptureState::Idle:
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no capture is running"));
      case CaptureState::Capturing: break;
      case CaptureState::Starting:
        if (armedLocked() && !m_resuming) {
          // A parked launch: terminate it (the backend's stop does that) and go back to idle.
          m_resuming = true;
          lock.unlock();
          {
            bool circular = false;
            if (auto ingest = takeIngest(circular)) ingest->discard();
          }
          (void)m_backend->stop();
          lock.lock();
          m_resuming = false;
          TACHYON_LOG_INFO("agent: parked launch cancelled");
          setStateLocked(CaptureState::Idle, 0.0);
          return makeOkResponse(std::nullopt);
        }
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "capture is not in a cancellable state"));
      default:
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "capture is not in a cancellable state"));
    }
    setStateLocked(CaptureState::Converting, 0.0);
  }
  {
    bool circular = false;
    if (auto ingest = takeIngest(circular)) ingest->discard();
  }
  auto result = m_backend->stop();
  if (result.ok()) {
    std::error_code ec;
    std::filesystem::remove(result->tracePath, ec);
    if (!result->rawPath.empty()) std::filesystem::remove(result->rawPath, ec);
  }
  std::lock_guard lock(m_mutex);
  TACHYON_LOG_INFO("agent: capture cancelled");
  setStateLocked(CaptureState::Idle, 0.0);
  if (!result.ok()) return makeErrorResponse(std::nullopt, result.error());
  return makeOkResponse(std::nullopt);
}

bool Agent::instPidAllowedLocked(uint32_t pid) const {
  if (pid == 0) return false;
  if (m_sessionFollow) return true;  // descendants are not known through the backend interface
  switch (m_sessionMode) {
    case capture::CaptureMode::System: return true;
    case capture::CaptureMode::Attach: return pid == m_sessionPid;
    case capture::CaptureMode::Launch: {
      if (m_watch == nullptr) return true;  // a backend that cannot tell (the mock)
      const uint32_t root = m_watch->rootPid();
      return root == 0 ||
             pid == root;  // 0: not launched yet; nothing can be running then, nothing to protect
    }
  }
  return false;
}

Response Agent::handleInstHello(const InstHelloRequest& req) {
  if (req.api_version >> 16 != kProtocolApiVersion >> 16)
    return makeErrorResponse(std::nullopt, makeError(ErrorCode::Internal, "unsupported SDK api_version"));
  std::shared_ptr<inst::Ingest> ingest;
  {
    std::lock_guard lock(m_mutex);
    const CaptureState st = m_state.load();
    if (st == CaptureState::Starting || st == CaptureState::Capturing) {
      if (!instPidAllowedLocked(req.pid))
        return makeErrorResponse(
            std::nullopt, makeError(ErrorCode::NoPrivilege, "pid " + std::to_string(req.pid) +
                                                                " is not part of the running capture"));
      ingest = m_ingest;
    }
  }
  if (!ingest)
    return makeErrorResponse(
        std::nullopt,
        makeError(ErrorCode::NotFound, "no capture is running; instrumentation is not recorded"));
  InstHelloBody body;
  body.pipe = instrumentationAddress(m_config.agentAddress, req.pid);
  // The listener must exist before the client is told where to connect.
  auto attached = ingest->attach(req.pid, body.pipe);
  // The socket is 0600 and owned by the agent's user; for an agent running on behalf of another user it is
  // handed over before the client learns its address (WP-41b).
  if (attached.ok()) attached = applySocketOwner(body.pipe, m_config.socketOwner);
  if (!attached.ok()) {
    TACHYON_LOG_WARN("agent: inst_hello from pid {}: {}", req.pid, attached.error().toString());
    return makeErrorResponse(std::nullopt, attached.error());
  }
  return makeOkResponse(std::nullopt, body);
}

void Agent::shutdown() {
  std::thread watcher;
  {
    std::lock_guard lock(m_mutex);
    m_watchStop = true;
    watcher = std::move(m_watcher);
  }
  m_watchCv.notify_all();
  if (watcher.joinable()) watcher.join();

  bool needStop = false;
  bool parked = false;
  {
    std::unique_lock lock(m_mutex);
    m_flushDone.wait(lock, [this] { return !m_flushing; });
    if (m_state.load() == CaptureState::Capturing) {
      setStateLocked(CaptureState::Converting, 0.0);
      needStop = true;
    } else if (armedLocked()) {
      parked = true;
    }
  }
  if (parked) {
    {
      bool circular = false;
      if (auto ingest = takeIngest(circular)) ingest->discard();
    }
    (void)m_backend->stop();  // terminates the parked process
    std::lock_guard lock(m_mutex);
    setStateLocked(CaptureState::Idle, 0.0);
  }
  if (needStop) {
    std::vector<std::string> warnings;
    auto result = stopAndMerge(warnings);
    std::lock_guard lock(m_mutex);
    if (result.ok()) {
      TACHYON_LOG_INFO("agent: shutdown stopped capture: {}", result->tracePath.string());
      setStateLocked(CaptureState::Done, 1.0);
      finishCaptureLocked(CaptureDoneEvent{toSummary(*result, std::move(warnings))});
    } else {
      TACHYON_LOG_ERROR("agent: shutdown stop failed: {}", result.error().toString());
      setStateLocked(CaptureState::Idle, 0.0);
    }
  }
  std::thread worker;
  std::thread flushWorker;
  {
    std::lock_guard lock(m_mutex);
    worker = std::move(m_worker);
    flushWorker = std::move(m_flushWorker);
  }
  if (worker.joinable()) worker.join();
  if (flushWorker.joinable()) flushWorker.join();
}

}  // namespace tachyon::agent
