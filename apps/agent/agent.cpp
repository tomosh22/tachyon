#include "agent/agent.h"

#include "capture/windows_etw/launch.h"
#include "control/process_list.h"
#include "control/sha256.h"
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
#include <vector>

#if !TACHYON_PLATFORM_WINDOWS
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// Removes a reserved but unused output file, when it is still the file that was reserved.
void releaseReserved(const std::filesystem::path& path, const control::FileIdentity& identity) {
  auto now = inspectPath(path);
  if (!now.ok() || !now->regular || now->identity != identity) return;  // not ours any more: leave it
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Ok when `path` is still the file reserved as `id` (or nothing was reserved); an error when something else
// has taken its place since (a rename or link by another local user): the backend may have written through
// it, so nothing is merged into it or served from it.
Result<void> checkReserved(const std::filesystem::path& path,
                           const std::optional<control::FileIdentity>& id) {
  if (!id) return ok();
  auto now = inspectPath(path);
  if (now.ok() && now->regular && now->identity == *id) return ok();
  TACHYON_LOG_ERROR("agent: {} is not the file the agent reserved for it (replaced while it was written)",
                    path.string());
  return makeError(ErrorCode::IoError, "the output file " + path.filename().string() +
                                           " was replaced by another file while it was written; refusing to "
                                           "use it");
}

// A file reserved with createExclusive that is deleted again when the operation does not go through.
struct OutputReservation {
  std::filesystem::path path;
  control::FileIdentity identity;
  bool active = false;
  OutputReservation() = default;
  OutputReservation(const OutputReservation&) = delete;
  OutputReservation& operator=(const OutputReservation&) = delete;
  ~OutputReservation() {
    if (active) releaseReserved(path, identity);
  }
};

}  // namespace

std::string remoteOutDirWarning(const std::filesystem::path& dir) {
#if TACHYON_PLATFORM_WINDOWS
  (void)dir;
  return {};
#else
  struct stat st {};
  if (::stat(dir.c_str(), &st) != 0) return {};
  const bool othersCanWrite = (st.st_mode & (S_IWGRP | S_IWOTH)) != 0;
  const bool sticky = (st.st_mode & S_ISVTX) != 0;
  const bool foreignOwner = st.st_uid != ::geteuid() && st.st_uid != 0;
  if (othersCanWrite && !sticky)
    return dir.string() +
           " is writable by group or others: a local user could replace the traces the agent writes there; "
           "run: chmod go-w " +
           dir.string();
  if (foreignOwner)
    return dir.string() + " is owned by uid " + std::to_string(st.st_uid) +
           ", who could replace the traces the agent writes there; use a directory you own";
  return {};
#endif
}

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

void Agent::onClientAccepted(ClientId client, const ClientInfo& info) {
  if (!info.remote) return;
  std::lock_guard lock(m_remoteMutex);
  m_remoteClients.insert(client);
}

void Agent::onDisconnect(ClientId client) {
  {
    std::lock_guard lock(m_remoteMutex);
    m_remoteClients.erase(client);
  }
  stopLiveSubscriber(client);
}

bool Agent::isRemoteClient(ClientId client) const {
  std::lock_guard lock(m_remoteMutex);
  return m_remoteClients.count(client) != 0;
}

Response Agent::handle(ClientId client, const Request& request) {
  const std::optional<int64_t> id = request.id;
  const bool remote = isRemoteClient(client);
  Response resp;
  if (std::holds_alternative<StatusRequest>(request.body)) {
    resp = handleStatus(id);
  } else if (const auto* s = std::get_if<StartRequest>(&request.body)) {
    resp = handleStart(*s, remote);
  } else if (std::holds_alternative<StopRequest>(request.body)) {
    resp = handleStop();
  } else if (const auto* f = std::get_if<FlushRequest>(&request.body)) {
    resp = handleFlush(*f);
  } else if (std::holds_alternative<CancelRequest>(request.body)) {
    resp = handleCancel();
  } else if (const auto* h = std::get_if<InstHelloRequest>(&request.body)) {
    // The SDK's sockets are local to this machine; a remote client has no business creating them.
    resp = remote
               ? makeErrorResponse(std::nullopt, makeError(ErrorCode::NoPrivilege,
                                                           "inst_hello is only accepted from local clients"))
               : handleInstHello(*h);
  } else if (const auto* fe = std::get_if<FetchRequest>(&request.body)) {
    resp = handleFetch(*fe);
  } else if (const auto* pr = std::get_if<ProcessesRequest>(&request.body)) {
    resp = handleProcesses(*pr);
  } else if (const auto* ls = std::get_if<LiveSubscribeRequest>(&request.body)) {
    resp = handleLiveSubscribe(client, *ls);
  } else if (std::holds_alternative<LiveUnsubscribeRequest>(request.body)) {
    stopLiveSubscriber(client);
    resp = makeOkResponse(id);
  }  // AuthRequest is answered by the server
  resp.id = id;
  return resp;
}

Response Agent::handleStatus(std::optional<int64_t> id) {
  StatusBody body;
  body.state = m_state.load();
  body.progress = m_progress.load();
  body.backend = m_config.backendName;
  body.elevated = m_config.elevated;
  {
    std::lock_guard lock(m_mutex);
    const CaptureState st = m_state.load();
    if (m_live && !m_live->ended() && (st == CaptureState::Starting || st == CaptureState::Capturing)) {
      body.live = true;
      body.live_seq = m_live->lastSeq();
    }
  }
  return makeOkResponse(id, body);
}

bool Agent::armedLocked() const {
  return m_state.load() == CaptureState::Starting && m_watch != nullptr && m_watch->launchPending();
}

Response Agent::handleStart(const StartRequest& req, bool remote) {
  if (req.live && !m_backend->supportsLive())
    return makeErrorResponse(
        std::nullopt,
        makeError(ErrorCode::Unsupported, "the '" + m_config.backendName + "' backend has no live source"));
  if (remote && req.mode == capture::CaptureMode::Launch && !m_config.allowRemoteLaunch)
    return makeErrorResponse(std::nullopt,
                             makeError(ErrorCode::NoPrivilege,
                                       "launching programs over a remote connection is disabled; attach to a "
                                       "running process, or start the agent with --allow-remote-launch"));
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
  opts.live = req.live;
  if (opts.mode == capture::CaptureMode::Launch && !m_config.agentAddress.empty())
    opts.env[std::string(capture::etw::kAgentPipeEnvVar)] = m_config.agentAddress;  // the SDK connects here
  OutputReservation reservation;  // removes a reserved file again unless the capture starts
  std::optional<control::FileIdentity> reservedIdentity;
  if (remote) {
    // A remote client chooses a file name, not a location.
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::string stem;
    if (!req.out_path.empty()) {
      try {
        stem = sanitizeFileStem(std::filesystem::path(req.out_path).stem().string());
      } catch (const std::exception&) {
      }
    }
    if (stem.empty()) stem = "capture-" + std::to_string(now);
    std::filesystem::path dir =
        m_config.remoteOutDir.empty() ? std::filesystem::path(".") : m_config.remoteOutDir;
    // The name is reserved with O_CREAT|O_EXCL|O_NOFOLLOW instead of being tested for existence: a link a
    // local user planted in the directory (dangling or not) is "taken", never written through.
    std::filesystem::path candidate = dir / (stem + ".tachyon");
    Result<control::FileIdentity> reserved = createExclusive(candidate);
    for (int n = 1; n < 10000 && !reserved.ok() && reserved.error().code == ErrorCode::Busy; ++n) {
      candidate = dir / (stem + "-" + std::to_string(n) + ".tachyon");
      reserved = createExclusive(candidate);
    }
    if (!reserved.ok())
      return makeErrorResponse(
          std::nullopt,
          makeError(reserved.error().code == ErrorCode::Busy ? ErrorCode::IoError : reserved.error().code,
                    "cannot create the output file in " + dir.string() + ": " + reserved.error().message));
    opts.outPath = candidate;
    reservation.path = candidate;
    reservation.identity = *reserved;
    reservation.active = true;
    reservedIdentity = *reserved;
  } else if (req.out_path.empty()) {
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
      m_outIdentity = reservedIdentity;
      m_outReservedPath = opts.outPath;
      m_sessionFollow = opts.followChildren;
      if (req.live) {
        control::LiveEncoderConfig liveConfig;
        liveConfig.windowSeconds = m_config.liveWindowSeconds;
        m_live = std::make_shared<control::LiveEncoder>(liveConfig);
        ingestConfig.liveSink = m_live;
      } else {
        m_live.reset();
      }
      m_ingest = std::make_shared<inst::Ingest>(std::move(ingestConfig));
      m_circular = opts.circularMb > 0;
    }
  }
  if (req.live && !resuming) {
    std::shared_ptr<control::LiveEncoder> encoder;
    {
      std::lock_guard lock(m_mutex);
      encoder = m_live;
    }
    m_backend->setLiveSink(encoder);
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
      m_live.reset();
    }
    return makeErrorResponse(std::nullopt, started.error());
  }
  if (!resuming && opts.mode == capture::CaptureMode::Launch && !opts.startImmediately &&
      m_watch != nullptr && m_watch->launchPending()) {
    reservation.active = false;  // parked: the capture still writes there once it is triggered
    TACHYON_LOG_INFO("agent: launched {} (pid {}) parked; waiting for start", opts.exe, m_watch->rootPid());
    return makeOkResponse(std::nullopt);  // state stays `starting`
  }
  TACHYON_LOG_INFO("agent: capture started ({}) -> {}", capture::captureModeName(opts.mode),
                   opts.outPath.string());
  reservation.active = false;  // the backend owns the file now
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
    if (info.stats.prefixOverflows > 0) {
      // Strings and thread names past the per-segment prefix cap are not repeated in later segments: the
      // scopes that use them show up unnamed in the older part of the flight recorder.
      TACHYON_LOG_WARN("agent: the instrumentation ring of pid {} hit the string prefix cap ({} entries)",
                       info.pid, info.stats.prefixOverflows);
      warnings.push_back("instrumentation ring of pid " + std::to_string(info.pid) +
                         " hit the string table cap (8 MiB per segment); " +
                         std::to_string(info.stats.prefixOverflows) +
                         " string or thread-name records are not repeated in later segments, so some "
                         "scope names may be missing");
    }
    if (info.stats.segmentOpenFailures > 0) {
      TACHYON_LOG_WARN("agent: the instrumentation ring of pid {} could not create {} segment files: {}",
                       info.pid, info.stats.segmentOpenFailures, info.stats.lastOpenError);
      warnings.push_back("instrumentation ring of pid " + std::to_string(info.pid) + ": " +
                         std::to_string(info.stats.segmentOpenFailures) +
                         " segment files could not be created (" + info.stats.lastOpenError +
                         "); the ring kept writing to the current segment and may exceed its size");
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
  endLive();  // everything the backend decoded is in the encoder now
  if (result.ok()) {
    std::optional<control::FileIdentity> reserved;
    std::filesystem::path reservedPath;
    {
      std::lock_guard lock(m_mutex);
      reserved = m_outIdentity;
      reservedPath = m_outReservedPath;
    }
    // The reserved file itself is checked: a backend may report another path (or none it wrote).
    if (auto checked = checkReserved(reservedPath, reserved); !checked.ok()) result = checked.error();
  }
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
  if (result.ok()) noteProduced(result->tracePath);
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

Result<std::string> Agent::reserveFlushName(const std::string& requested, control::FileIdentity& identity) {
  std::string stem = sanitizeFileStem(requested);
  if (stem.empty()) stem = "flush";
  const std::filesystem::path& dir = m_sessionOutDir;  // caller holds m_mutex
  std::string candidate = stem;
  Result<control::FileIdentity> reserved = createExclusive(dir / (candidate + ".tachyon"));
  for (int n = 1; n < 10000 && !reserved.ok() && reserved.error().code == ErrorCode::Busy; ++n) {
    candidate = stem + "-" + std::to_string(n);
    reserved = createExclusive(dir / (candidate + ".tachyon"));
  }
  if (!reserved.ok())
    return makeError(ErrorCode::IoError,
                     "cannot create the flushed trace in " + dir.string() + ": " + reserved.error().message);
  identity = *reserved;
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
  if (!result.ok()) {
    if (job.reserved) releaseReserved(job.dir / (job.name + ".tachyon"), *job.reserved);
    return result.error();
  }
  if (auto checked = checkReserved(job.dir / (job.name + ".tachyon"), job.reserved); !checked.ok())
    return checked.error();
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
  noteProduced(result->tracePath);  // after the merge: it replaces the file, the identity is recorded now
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
      control::FileIdentity identity;
      auto name = reserveFlushName(processNameFor(req.pid, m_sessionExeStem) + "-" +
                                       sanitizeFileStem(job.reason, 40) + "-" + timestampText(),
                                   identity);
      if (!name.ok()) {
        m_haveTrigger = false;  // nothing started: the next trigger may try again
        return makeErrorResponse(std::nullopt, name.error());
      }
      job.name = *name;
      job.reserved = identity;
    } else {
      if (m_flushing)
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy, "a flush is already in progress"));
      job.reason = req.reason.empty() ? req.name : req.reason;
      job.seconds = req.seconds;
      control::FileIdentity identity;
      auto name = reserveFlushName(req.name, identity);
      if (!name.ok()) return makeErrorResponse(std::nullopt, name.error());
      job.name = *name;
      job.reserved = identity;
    }
    job.dir = m_sessionOutDir;
    job.coalesced = m_coalesced;
    m_flushing = true;  // stop / cancel / shutdown wait for it instead of interleaving
    // A trigger that is coalesced while this flush runs is answered with this path, whoever asked for it.
    m_lastFlushPath = (m_sessionOutDir / (job.name + ".tachyon")).string();
    if (req.trigger) {
      // Answer now, flush in the background: the requester is the engine's main thread.
      CaptureSummary planned;
      planned.path = m_lastFlushPath;
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
          endLive();
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
  endLive();
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

void Agent::noteProduced(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::canonical(path, ec);
  if (ec) return;  // not on disk (a backend that reports a path it did not write): nothing to serve
  auto info = inspectPath(canonical);
  if (!info.ok() || !info->regular) return;
  std::lock_guard lock(m_producedMutex);
  m_produced[canonical] = info->identity;
  m_hashCache.erase(canonical);
}

Response Agent::handleFetch(const FetchRequest& req) {
  // One answer for "does not exist" and "not yours to read", so the reply does not map the file system.
  const auto notFound = [] {
    return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound,
                                                     "no such trace: the agent only serves files it produced "
                                                     "since it started"));
  };
  if (req.path.empty() || req.path.find('\0') != std::string::npos)
    return makeErrorResponse(std::nullopt, makeError(ErrorCode::InvalidArgument, "bad path"));
  std::error_code ec;
  // Resolves "..", "." and every symlink: whatever the request spells, only the real file is compared.
  const std::filesystem::path canonical = std::filesystem::canonical(std::filesystem::path(req.path), ec);
  if (ec) return notFound();
  control::FileIdentity produced;
  {
    std::lock_guard lock(m_producedMutex);
    const auto it = m_produced.find(canonical);
    if (it == m_produced.end()) return notFound();
    produced = it->second;
  }
  // Open first (never through a link), then compare what was opened with the file that was produced: a
  // local user swapping the file between the lookup above and the open is caught here, not trusted.
  auto file = SafeFile::openRead(canonical);
  if (!file.ok()) return notFound();
  if (file->identity() != produced) {
    TACHYON_LOG_WARN("agent: fetch of {} refused: it is not the file the agent produced", canonical.string());
    return notFound();
  }
  const uint64_t size = file->size();

  FetchBody body;
  body.offset = req.offset;
  body.size = size;
  if (req.offset < size) {
    const uint64_t want = std::min<uint64_t>({req.length, kMaxFetchChunkBytes, size - req.offset});
    body.data.resize(static_cast<size_t>(want));
    auto got = file->readAt(req.offset, body.data.data(), static_cast<size_t>(want));
    if (!got.ok())
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::IoError, "cannot read the trace"));
    if (*got != want)
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::IoError, "short read from the trace"));
  }
  // offset beyond the end: an empty answer that tells the client the size (a stale partial download).
  body.eof = req.offset <= size && req.offset + body.data.size() == size;
  if (body.eof || req.want_sha256) {
    // The whole-file hash is cached per (path, size, mtime): a client asking for it with every chunk of a
    // multi-gigabyte trace does not make the agent read the file again each time.
    {
      std::lock_guard lock(m_producedMutex);
      const auto it = m_hashCache.find(canonical);
      if (it != m_hashCache.end() && it->second.size == size && it->second.mtimeNs == file->mtimeNs())
        body.sha256 = it->second.sha256;
    }
    if (body.sha256.empty()) {
      auto hash = file->sha256();
      if (!hash.ok()) return makeErrorResponse(std::nullopt, hash.error());
      body.sha256 = std::move(*hash);
      ++m_hashComputations;
      std::lock_guard lock(m_producedMutex);
      m_hashCache[canonical] = CachedHash{size, file->mtimeNs(), body.sha256};
    }
  }
  return makeOkResponse(std::nullopt, std::move(body));
}

Response Agent::handleProcesses(const ProcessesRequest& req) {
  auto list = control::listProcesses();
  if (!list.ok()) return makeErrorResponse(std::nullopt, list.error());
  constexpr size_t kMaxCmdlineChars = 256;
  constexpr size_t kBudgetBytes =
      600 * 1024;  // well under kMaxLineBytes after JSON escaping of the usual text
  ProcessesBody body;
  body.offset = req.offset;
  body.total = static_cast<uint32_t>(list->size());
  size_t used = 0;
  for (size_t i = req.offset; i < list->size(); ++i) {
    ProcessEntry p = std::move((*list)[i]);
    if (p.cmdline.size() > kMaxCmdlineChars) p.cmdline.resize(kMaxCmdlineChars);
    if (p.name.size() > 128) p.name.resize(128);
    if (p.user.size() > 128) p.user.resize(128);
    // Control characters and non-ASCII bytes can grow six-fold when escaped: charge for it.
    const size_t cost = 48 + 3 * (p.name.size() + p.user.size() + p.cmdline.size());
    if (!body.processes.empty() && used + cost > kBudgetBytes) break;
    used += cost;
    body.processes.push_back(std::move(p));
  }
  return makeOkResponse(std::nullopt, std::move(body));
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
    endLive();
    std::lock_guard lock(m_mutex);
    setStateLocked(CaptureState::Idle, 0.0);
  }
  if (needStop) {
    std::vector<std::string> warnings;
    auto result = stopAndMerge(warnings);
    if (result.ok()) noteProduced(result->tracePath);
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
  endLive();
  stopAllLiveSubscribers();
}

// ---- Live view (WP-65a) ----

namespace {
// A subscriber's outbound queue may hold this much before its sender waits (below the 2 MiB cap that drops a
// client, so a response or event of another kind always fits behind live data).
constexpr size_t kLiveQueueLimit = 1536 * 1024;
}  // namespace

void Agent::onServerStarted(control::Server& server) {
  std::lock_guard lock(m_serverMutex);
  m_server = &server;
}

void Agent::onServerStopping() {
  {
    std::lock_guard lock(m_serverMutex);
    m_server = nullptr;
  }
  stopAllLiveSubscribers();
}

void Agent::endLive() {
  std::shared_ptr<control::LiveEncoder> encoder;
  {
    std::lock_guard lock(m_mutex);
    encoder = m_live;
  }
  if (encoder) encoder->finish();
}

Response Agent::handleLiveSubscribe(ClientId client, const LiveSubscribeRequest& req) {
  std::shared_ptr<control::LiveEncoder> encoder;
  {
    std::lock_guard lock(m_mutex);
    const CaptureState st = m_state.load();
    if (m_live && !m_live->ended() && (st == CaptureState::Starting || st == CaptureState::Capturing))
      encoder = m_live;
  }
  if (!encoder)
    return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no live capture is running"));
  stopLiveSubscriber(client);  // a second subscribe replaces the first
  auto sub = std::make_unique<LiveSubscriber>();
  sub->client = client;
  sub->encoder = encoder;
  LiveSubscribeBody body;
  if (req.has_from_seq && encoder->retains(req.from_seq)) {
    sub->next = req.from_seq;
    body.seq = req.from_seq;
  } else {
    sub->keyframe.emplace(encoder->beginKeyframe());
    body.seq = sub->keyframe->seq();
    sub->next = body.seq + 1;
  }
  {
    std::lock_guard lock(m_subMutex);
    if (m_subscribers.size() + m_pendingSubs.size() >= m_config.maxLiveSubscribers)
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy, "too many live subscribers"));
    m_pendingSubs[client] = std::move(sub);
  }
  return makeOkResponse(std::nullopt, body);
}

void Agent::afterResponse(ClientId client, const Request& request, const Response& response) {
  if (!std::holds_alternative<LiveSubscribeRequest>(request.body) || !response.ok) return;
  std::lock_guard lock(m_subMutex);
  auto it = m_pendingSubs.find(client);
  if (it == m_pendingSubs.end()) return;
  std::unique_ptr<LiveSubscriber> sub = std::move(it->second);
  m_pendingSubs.erase(it);
  LiveSubscriber* raw = sub.get();
  m_subscribers[client] = std::move(sub);
  raw->thread = std::thread([this, raw] { liveSubscriberLoop(*raw); });
}

void Agent::stopLiveSubscriber(ClientId client) {
  std::unique_ptr<LiveSubscriber> sub;
  {
    std::lock_guard lock(m_subMutex);
    m_pendingSubs.erase(client);
    auto it = m_subscribers.find(client);
    if (it == m_subscribers.end()) return;
    sub = std::move(it->second);
    m_subscribers.erase(it);
  }
  sub->stop.store(true);
  if (sub->thread.joinable()) sub->thread.join();
}

void Agent::stopAllLiveSubscribers() {
  std::map<ClientId, std::unique_ptr<LiveSubscriber>> subs;
  {
    std::lock_guard lock(m_subMutex);
    subs.swap(m_subscribers);
    m_pendingSubs.clear();
  }
  for (auto& [id, sub] : subs) sub->stop.store(true);
  for (auto& [id, sub] : subs)
    if (sub->thread.joinable()) sub->thread.join();
}

bool Agent::sendLiveLine(LiveSubscriber& sub, std::string line, bool& stalled) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(m_config.liveStallMs);
  for (;;) {
    if (sub.stop.load()) return false;
    Server::SendStatus status;
    {
      std::lock_guard lock(m_serverMutex);
      if (m_server == nullptr) return false;
      status = m_server->trySendLine(sub.client, line, kLiveQueueLimit);
    }
    if (status == Server::SendStatus::Sent) return true;
    if (status == Server::SendStatus::Gone) return false;
    if (Clock::now() >= deadline) {
      stalled = true;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void Agent::liveSubscriberLoop(LiveSubscriber& sub) {
  control::LiveEncoder& encoder = *sub.encoder;
  auto sendSmall = [&](const Event& event) {
    std::lock_guard lock(m_serverMutex);
    if (m_server != nullptr)
      (void)m_server->trySendLine(sub.client, encodeEvent(event), kMaxQueuedBytesPerClient);
  };
  bool stalled = false;
  if (sub.keyframe) {
    std::string chunk;
    bool first = true;
    while (sub.keyframe->next(chunk)) {
      if (!sendLiveLine(sub, encodeEvent(LiveEvent{sub.keyframe->seq(), first, std::move(chunk)}), stalled)) {
        if (stalled) sendSmall(LiveGapEvent{sub.next});
        return;
      }
      first = false;
      chunk.clear();
    }
    sub.keyframe.reset();
  }
  while (!sub.stop.load()) {
    control::LiveBatch batch;
    switch (encoder.waitBatch(sub.next, batch, std::chrono::milliseconds(100))) {
      case control::LiveWait::Timeout: continue;
      case control::LiveWait::Gap: sendSmall(LiveGapEvent{sub.next}); return;
      case control::LiveWait::Ended: sendSmall(LiveEndEvent{sub.next - 1}); return;
      case control::LiveWait::Ready: break;
    }
    for (std::string_view piece : control::splitRecords(*batch.data, model::live_wire::kMaxEventBytes)) {
      if (!sendLiveLine(sub, encodeEvent(LiveEvent{batch.seq, false, std::string(piece)}), stalled)) {
        if (stalled) sendSmall(LiveGapEvent{sub.next});
        return;
      }
    }
    ++sub.next;
  }
}

}  // namespace tachyon::agent
