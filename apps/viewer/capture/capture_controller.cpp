#include "viewer/capture/capture_controller.h"

#include "control/auth.h"
#include "control/client.h"
#include "control/fetch.h"
#include "control/tcp.h"
#include "control/transport.h"
#include "core/log.h"
#include "core/platform.h"
#include "viewer/capture/capture_paths.h"
#include "viewer/capture/capture_ui.h"
#include "viewer/job_queue.h"
#include "viewer/paths.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>

#if !TACHYON_PLATFORM_WINDOWS
#include <unistd.h>
#endif

namespace tachyon::viewer {

using capture_ui::inputString;

namespace {

constexpr int kStartAgentRetryMs = 8000;
constexpr int kElevatedRetryMs = 60000;  // time to type a password / answer the UAC prompt
constexpr ImVec4 kRed(1.0f, 0.4f, 0.4f, 1.0f);
constexpr ImVec4 kAmber(1.0f, 0.75f, 0.2f, 1.0f);
constexpr ImVec4 kGreen(0.4f, 0.85f, 0.45f, 1.0f);

const char* modeTitle(capture::CaptureMode mode, bool remote, bool live = false) {
  // The Live dialog lets the user switch the mode inside it: one window for all three.
  if (live) return remote ? "Live Capture (remote agent)##live-dialog" : "Live Capture##live-dialog";
  switch (mode) {
    case capture::CaptureMode::Launch:
      return remote ? "Run Application (remote agent)##capture-dialog" : "Run Application##capture-dialog";
    case capture::CaptureMode::Attach:
      return remote ? "Attach to Process (remote agent)##capture-dialog"
                    : "Attach to Process##capture-dialog";
    case capture::CaptureMode::System:
      return remote ? "System Capture (remote agent)##capture-dialog" : "System Capture##capture-dialog";
  }
  return "Capture##capture-dialog";
}

}  // namespace

AgentLaunchPlan planAgentLaunch(const CaptureSettings& settings, const std::filesystem::path& viewerExe,
                                const std::string& address, bool elevate) {
  AgentLaunchPlan plan;
  plan.backend =
      settings.agentBackend.empty() ? (TACHYON_PLATFORM_WINDOWS ? "etw" : "perf") : settings.agentBackend;
  plan.backendNeedsPrivileges = plan.backend != "mock";
  if (!settings.agentExe.empty()) {
    plan.spec.exe = pathFromUtf8(settings.agentExe);
  } else {
    plan.spec.exe =
        viewerExe.parent_path() / (TACHYON_PLATFORM_WINDOWS ? "tachyon-agent.exe" : "tachyon-agent");
  }
  plan.spec.args = {"--backend", plan.backend, TACHYON_PLATFORM_WINDOWS ? "--pipe" : "--socket", address};
#if !TACHYON_PLATFORM_WINDOWS
  // pkexec runs the agent as root: it must hand its sockets to the user who is starting it (WP-41b).
  if (elevate) {
    plan.spec.args.push_back("--socket-owner");
    plan.spec.args.push_back(std::to_string(static_cast<unsigned long>(::getuid())));
  }
#endif
  plan.spec.args.insert(plan.spec.args.end(), settings.agentArgs.begin(), settings.agentArgs.end());
  plan.spec.elevate = elevate;
  plan.spec.description = "the capture agent";
  return plan;
}

std::string formatElapsed(double seconds) {
  if (seconds < 0.0) seconds = 0.0;
  const int64_t whole = static_cast<int64_t>(seconds);
  char buffer[32];
  if (whole >= 3600) {
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", static_cast<long long>(whole / 3600),
                  static_cast<long long>((whole / 60) % 60), static_cast<long long>(whole % 60));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%lld:%04.1f", static_cast<long long>(whole / 60),
                  seconds - static_cast<double>((whole / 60) * 60));
  }
  return buffer;
}

CaptureController::CaptureController(CaptureHost host, std::shared_ptr<IProcessLauncher> launcher)
    : m_host(std::move(host)),
      m_launcher(launcher ? std::move(launcher)
                          : std::shared_ptr<IProcessLauncher>(makeSystemProcessLauncher())),
      m_session([&] {
        AgentSession::Options o;
        o.wake = m_host.wake;
        return o;
      }()) {}

CaptureController::~CaptureController() {
  if (m_processJob != 0 && m_host.jobs) m_host.jobs->cancel(m_processJob);
}

void CaptureController::toast(ToastLevel level, std::string text) {
  if (m_host.toast) m_host.toast(level, std::move(text));
}

// ---- agent connection ----

std::string CaptureController::agentAddress() const {
  if (m_host.settings && !m_host.settings->capture.agentAddress.empty())
    return m_host.settings->capture.agentAddress;
  return control::defaultAgentAddress();
}

void CaptureController::connectNow(int retryMs, bool userInitiated) {
  if (m_connectedRemote && m_host.settings && !m_host.settings->capture.remoteAddress.empty()) {
    m_session.connect("tcp://" + m_host.settings->capture.remoteAddress, retryMs, userInitiated,
                      m_host.settings->capture.remoteTokenFile);
  } else {
    m_connectedRemote = false;
    m_session.connect(agentAddress(), retryMs, userInitiated);
  }
  if (m_host.requestRedraw) m_host.requestRedraw();
}

void CaptureController::connectAtStartup() {
  if (m_host.settings && !m_host.settings->capture.autoConnect) return;
  connectNow(0, /*userInitiated=*/false);
}

void CaptureController::reconnect() {
  connectNow(0, true);
}

void CaptureController::disconnectAgent() {
  m_session.disconnect();
}

bool CaptureController::agentNeedsElevation() const {
  const CaptureSettings empty;
  const CaptureSettings& settings = m_host.settings ? m_host.settings->capture : empty;
  const AgentLaunchPlan plan = planAgentLaunch(settings, currentExecutablePath(), agentAddress(), false);
  return plan.backendNeedsPrivileges && !currentProcessHasCapturePrivileges();
}

void CaptureController::startAgent(bool elevate) {
  m_connectedRemote = false;  // starting an agent means the one on this machine
  const CaptureSettings empty;
  const CaptureSettings& settings = m_host.settings ? m_host.settings->capture : empty;
  const AgentLaunchPlan plan = planAgentLaunch(settings, currentExecutablePath(), agentAddress(), elevate);
  std::error_code ec;
  if (!std::filesystem::exists(plan.spec.exe, ec)) {
    toast(ToastLevel::Error, "Cannot start the agent: " + pathToUtf8(plan.spec.exe) +
                                 " does not exist (set capture.agentExe in the settings)");
    return;
  }
  const Result<LaunchedProcess> launched = m_launcher->launch(plan.spec);
  if (!launched.ok()) {
    toast(ToastLevel::Error, "Cannot start the agent: " + launched.error().message);
    return;
  }
  toast(ToastLevel::Info,
        std::string("Starting the agent (") + plan.backend + (elevate ? ", elevated" : "") + ")...");
  connectNow(elevate ? kElevatedRetryMs : kStartAgentRetryMs, true);
}

// ---- remote agent (WP-61) ----

RemoteProbe probeRemoteAgent(const std::string& hostPort, const std::string& tokenFile) {
  RemoteProbe probe;
  probe.done = true;
  auto endpoint = control::parseTcpEndpoint(hostPort);
  if (!endpoint.ok()) {
    probe.message = endpoint.error().message;
    return probe;
  }
  if (tokenFile.empty()) {
    probe.message = "Choose the token file (the agent's --token-file)";
    return probe;
  }
  auto token = control::loadTokenFile(pathFromUtf8(tokenFile), 1);
  if (!token.ok()) {
    probe.message = token.error().message;
    return probe;
  }
  auto client = control::Client::connect("tcp://" + control::formatTcpEndpoint(*endpoint), *token);
  if (!client.ok()) {
    probe.message = client.error().message;
    return probe;
  }
  auto status = client->call(control::StatusRequest{}, 5000);
  if (!status.ok() || !status->ok) {
    probe.message = status.ok() ? status->error : status.error().message;
    return probe;
  }
  probe.ok = true;
  probe.message = "connected";
  if (const auto* body = std::get_if<control::StatusBody>(&status->body)) {
    probe.message += ": " + (body->backend.empty() ? std::string("agent") : body->backend + " backend") +
                     ", " + std::string(capture::captureStateName(body->state)) +
                     (body->elevated ? ", elevated" : ", not elevated");
  }
  return probe;
}

bool CaptureController::remoteActive() const {
  const AgentSnapshot s = m_session.snapshot();
  return s.remote && s.connection != ConnectionState::Disconnected;
}

bool CaptureController::connectRemote(const std::string& hostPort, const std::string& tokenFile) {
  auto endpoint = control::parseTcpEndpoint(hostPort);
  if (!endpoint.ok()) {
    toast(ToastLevel::Error, "Remote agent address: " + endpoint.error().message);
    return false;
  }
  const std::string normalized = control::formatTcpEndpoint(*endpoint);
  if (m_host.settings) {
    m_host.settings->capture.remoteAddress = normalized;
    m_host.settings->capture.remoteTokenFile = tokenFile;
    if (m_host.saveSettings) m_host.saveSettings();
  }
  m_connectedRemote = true;
  m_session.connect("tcp://" + normalized, 0, true, tokenFile);
  if (m_host.requestRedraw) m_host.requestRedraw();
  return true;
}

void CaptureController::useLocalAgent() {
  m_connectedRemote = false;
  connectNow(0, true);
}

void CaptureController::openRemoteDialog() {
  const CaptureSettings empty;
  const CaptureSettings& c = m_host.settings ? m_host.settings->capture : empty;
  m_remoteAddressText = c.remoteAddress;
  m_remoteTokenText = c.remoteTokenFile;
  m_remoteProbe = {};
  m_remoteDialogOpen = true;
  m_remoteDialogJustOpened = true;
  if (m_host.requestRedraw) m_host.requestRedraw();
}

void CaptureController::testRemote() {
  if (m_probeJob != 0 && m_host.jobs) m_host.jobs->cancel(m_probeJob);
  m_remoteProbe = {};
  const std::string address = m_remoteAddressText;
  const std::string tokenFile = m_remoteTokenText;
  if (!m_host.jobs) {
    m_remoteProbe = probeRemoteAgent(address, tokenFile);
    return;
  }
  auto result = std::make_shared<RemoteProbe>();
  m_probeJob = m_host.jobs->submit(
      "Testing the remote agent",
      [result, address, tokenFile](JobContext&) { *result = probeRemoteAgent(address, tokenFile); },
      [this, result](JobQueue::JobId id, JobOutcome outcome) {
        if (id != m_probeJob) return;
        m_probeJob = 0;
        if (outcome == JobOutcome::Cancelled) return;
        m_remoteProbe = *result;
        if (m_host.requestRedraw) m_host.requestRedraw();
      });
}

void CaptureController::fetchRemote(const std::string& remotePath, bool openWhenDone, bool isCapture,
                                    std::vector<std::string> warnings) {
  if (remotePath.empty()) return;
  for (const RemoteTransfer& t : m_transfers)
    if (t.remotePath == remotePath && t.state == RemoteTransfer::State::Running) return;
  // The file name is the last component of the remote path, whichever OS wrote it.
  const size_t cut = remotePath.find_last_of("/\\");
  std::string name = cut == std::string::npos ? remotePath : remotePath.substr(cut + 1);
  while (!name.empty() && name.front() == '.') name.erase(name.begin());
  if (name.empty()) name = "remote-capture.tachyon";
  const std::filesystem::path dir =
      capturesDir(m_host.settings ? m_host.settings->capture : CaptureSettings());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::path local = dir / pathFromUtf8(name);
  // Never replace a trace that is already here: a second agent can produce the same name. A .part file left
  // by an earlier attempt for this very remote path is resumed, so it counts as free.
  for (int n = 1; n < 1000 && std::filesystem::exists(local, ec); ++n)
    local = dir / pathFromUtf8(pathToUtf8(pathFromUtf8(name).stem()) + "-" + std::to_string(n) +
                               pathToUtf8(pathFromUtf8(name).extension()));
  RemoteTransfer t;
  t.remotePath = remotePath;
  t.localPath = local;
  t.openWhenDone = openWhenDone;
  t.isCapture = isCapture;
  t.warnings = std::move(warnings);
  m_transfers.push_back(std::move(t));
  constexpr size_t kMaxTransfers = 100;
  if (m_transfers.size() > kMaxTransfers)
    m_transfers.erase(m_transfers.begin(),
                      m_transfers.begin() + static_cast<std::ptrdiff_t>(m_transfers.size() - kMaxTransfers));
  startTransfer(m_transfers.size() - 1);
}

void CaptureController::startTransfer(size_t index) {
  RemoteTransfer& t = m_transfers[index];
  t.state = RemoteTransfer::State::Running;
  t.error.clear();
  const AgentSnapshot snap = m_session.snapshot();
  const std::string address = snap.address;
  const std::string tokenFile = m_host.settings ? m_host.settings->capture.remoteTokenFile : std::string();
  const std::string remotePath = t.remotePath;
  const std::filesystem::path local = t.localPath;

  struct Outcome {
    bool ok = false;
    std::string error;
    uint64_t bytes = 0;
  };
  auto outcome = std::make_shared<Outcome>();
  // Runs on a worker thread. It has its own connection, so the session's polling never waits for the data.
  auto work = [outcome, address, tokenFile, remotePath, local](JobContext& ctx) {
    auto token = control::loadTokenFile(pathFromUtf8(tokenFile), 1);
    if (!token.ok()) {
      outcome->error = token.error().message;
      return;
    }
    auto client = control::Client::connectWithRetry(address, 5000, *token);
    if (!client.ok()) {
      outcome->error = client.error().message;
      return;
    }
    control::FetchOptions options;
    options.cancel = ctx.cancelFlag();
    options.progress = [&ctx](uint64_t done, uint64_t total) {
      if (total > 0)
        ctx.setProgress(static_cast<float>(static_cast<double>(done) / static_cast<double>(total)));
      return true;
    };
    auto fetched = control::fetchFile(*client, remotePath, local, options);
    if (fetched.ok()) {
      outcome->ok = true;
      outcome->bytes = fetched->size;
    } else {
      outcome->error = fetched.error().message;
    }
  };
  if (!m_host.jobs) {  // no job queue (some tests): run inline
    JobContext ctx;
    work(ctx);
    onTransferDone(index, outcome->ok, outcome->error, outcome->bytes);
    return;
  }
  const std::string label = "Fetching " + pathToUtf8(local.filename());
  t.jobId = m_host.jobs->submit(label, work, [this, outcome, remotePath](JobQueue::JobId id, JobOutcome how) {
    // The vector may have been trimmed or the transfer retried since: find it by its job.
    for (size_t i = 0; i < m_transfers.size(); ++i) {
      if (m_transfers[i].jobId != id || m_transfers[i].remotePath != remotePath) continue;
      m_transfers[i].jobId = 0;
      if (how == JobOutcome::Cancelled) {
        m_transfers[i].state = RemoteTransfer::State::Cancelled;
        m_transfers[i].error = "cancelled";
        if (m_host.requestRedraw) m_host.requestRedraw();
      } else {
        onTransferDone(i, outcome->ok, outcome->error, outcome->bytes);
      }
      return;
    }
  });
}

void CaptureController::onTransferDone(size_t index, bool ok, const std::string& error, uint64_t bytes) {
  RemoteTransfer& t = m_transfers[index];
  if (!ok) {
    t.state = error == "fetch cancelled" ? RemoteTransfer::State::Cancelled : RemoteTransfer::State::Failed;
    t.error = error;
    if (t.state == RemoteTransfer::State::Failed)
      toast(ToastLevel::Error, "Could not fetch " + pathToUtf8(t.localPath.filename()) + ": " + error +
                                   " (the partial download is kept; Retry in the Capture window resumes it)");
    if (m_host.requestRedraw) m_host.requestRedraw();
    return;
  }
  t.state = RemoteTransfer::State::Done;
  t.bytes = bytes;
  const std::string localText = pathToUtf8(t.localPath);
  char size[32];
  std::snprintf(size, sizeof(size), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  toast(ToastLevel::Info, "Fetched " + pathToUtf8(t.localPath.filename()) + " (" + size + ")");
  for (FlushedCapture& f : m_flushed) {
    if (f.remotePath == t.remotePath) {
      f.path = localText;
      f.fetched = true;
    }
  }
  finishLocalCapture(t.localPath, t.openWhenDone, std::move(t.warnings));
}

void CaptureController::cancelTransfer(size_t index) {
  if (index >= m_transfers.size()) return;
  RemoteTransfer& t = m_transfers[index];
  if (t.state == RemoteTransfer::State::Running && t.jobId != 0 && m_host.jobs) m_host.jobs->cancel(t.jobId);
}

void CaptureController::retryTransfer(size_t index) {
  if (index >= m_transfers.size()) return;
  const RemoteTransfer::State s = m_transfers[index].state;
  if (s == RemoteTransfer::State::Failed || s == RemoteTransfer::State::Cancelled) startTransfer(index);
}

// ---- capture commands ----

void CaptureController::resumeCapture() {
  m_session.resume();
}

void CaptureController::stopCapture() {
  m_session.stop();
}

void CaptureController::stopCaptureAndOpen() {
  m_openNextCapture = true;
  m_session.stop();
}

void CaptureController::cancelCapture() {
  m_session.cancel();
}

// ---- events ----

bool CaptureController::hasPending() const {
  return m_session.hasEvents();
}

bool CaptureController::active() const {
  const AgentSnapshot s = m_session.snapshot();
  return s.connection == ConnectionState::Connecting || s.phase == AgentPhase::Armed ||
         s.phase == AgentPhase::Capturing || s.phase == AgentPhase::Stopping ||
         s.phase == AgentPhase::Starting || s.commandInFlight;
}

bool CaptureController::pump() {
  std::vector<AgentEvent> events = m_session.drainEvents();
  for (const AgentEvent& e : events) handleEvent(e);
  // The capture is over (the agent is idle) and no capture_done took the "open the trace" request: a stop
  // that failed, a capture that produced no trace. Do not let it open the next, unrelated capture.
  if (m_openStale) {
    m_openNextCapture = false;
    m_openStale = false;
  }
  return !events.empty();
}

void CaptureController::handleEvent(const AgentEvent& e) {
  if (m_host.liveEvent) m_host.liveEvent(e);
  switch (e.kind) {
    case AgentEventKind::Connected: {
      if (e.userInitiated) {
        const AgentSnapshot s = m_session.snapshot();
        toast(ToastLevel::Info, "Connected to the agent" +
                                    (s.backend.empty() ? std::string() : " (" + s.backend + " backend)"));
      }
      break;
    }
    case AgentEventKind::ConnectFailed:
      if (e.userInitiated) {
        std::string text = e.message;
        // An agent started as root outside pkexec / sudo (a service, su) creates a socket this user cannot
        // open; the viewer's own pkexec launch and sudo hand the socket over (--socket-owner).
        if (text.find("ermission denied") != std::string::npos || text.find("ACCES") != std::string::npos)
          text += " (the agent runs as another user; start it with --socket-owner <your uid>)";
        toast(ToastLevel::Warning, std::move(text));
      }
      break;
    case AgentEventKind::ConnectionLost:
      m_openNextCapture = false;
      toast(ToastLevel::Error, e.message + " (Capture > Reconnect)");
      break;
    case AgentEventKind::StateChanged:
      if (e.phase == AgentPhase::Idle) m_openStale = true;
      if ((e.phase == AgentPhase::Capturing || e.phase == AgentPhase::Armed) && m_lastPhase != e.phase &&
          m_host.showPanel) {
        m_host.showPanel(kCapturePanelName);
        if (e.phase == AgentPhase::Capturing && m_session.snapshot().circularMb > 0)
          m_host.showPanel(kFlightRecorderPanelName);
      }
      m_lastPhase = e.phase;
      break;
    case AgentEventKind::Flushed: {
      FlushedCapture f;
      f.name = e.flushed.name;
      f.reason = e.flushed.reason;
      f.path = e.flushed.path;
      f.durationS = e.flushed.duration_s;
      f.eventsLost = e.flushed.events_lost;
      f.coalesced = e.flushed.coalesced;
      f.at = std::chrono::system_clock::now();
      const bool remote = m_session.snapshot().remote;
      if (remote) {
        f.remotePath = e.flushed.path;
        f.fetched = false;
      }
      m_flushed.push_back(std::move(f));
      constexpr size_t kMaxFlushed = 200;
      if (m_flushed.size() > kMaxFlushed)
        m_flushed.erase(m_flushed.begin(),
                        m_flushed.begin() + static_cast<std::ptrdiff_t>(m_flushed.size() - kMaxFlushed));
      toast(ToastLevel::Info, "Flight recorder: flushed " + e.flushed.name + " (" + e.flushed.reason + ")");
      if (remote) {
        fetchRemote(e.flushed.path, /*openWhenDone=*/false, /*isCapture=*/false);
      } else if (m_host.capturesChanged) {
        m_host.capturesChanged();
      }
      break;
    }
    case AgentEventKind::ProcessList: {
      m_remoteProcessesPending = false;
      if (!e.message.empty()) {
        m_processError = e.message;
      } else {
        m_processes.clear();
        for (const control::ProcessEntry& p : e.processes)
          m_processes.push_back({p.pid, p.name, p.user, p.cmdline});
        m_processError.clear();
      }
      break;
    }
    case AgentEventKind::LiveGap: toast(ToastLevel::Warning, e.message); break;
    case AgentEventKind::LiveEnded: break;  // the host turns the Live document into a static snapshot
    case AgentEventKind::LiveFailed:
      toast(ToastLevel::Error, "Live view stopped: " + e.message + " (the capture itself continues)");
      break;
    case AgentEventKind::CaptureDone: onCaptureDone(e); break;
    case AgentEventKind::AgentError: toast(ToastLevel::Error, "Agent: " + e.message); break;
    case AgentEventKind::CommandFailed:
      if (e.command == "stop") m_openNextCapture = false;
      toast(ToastLevel::Error, "The agent refused " + e.command + " (" + e.code + "): " + e.message);
      break;
  }
  if (m_host.requestRedraw) m_host.requestRedraw();
}

void CaptureController::onCaptureDone(const AgentEvent& e) {
  const control::CaptureSummary& s = e.summary;
  const std::filesystem::path path = pathFromUtf8(s.path);
  char text[64];
  std::snprintf(text, sizeof(text), "%.1f s", s.duration_s);
  std::string message = "Capture finished: " + pathToUtf8(path.filename()) + " (" + text;
  if (s.events_lost > 0) message += ", " + std::to_string(s.events_lost) + " events lost";
  message += ")";
  toast(s.events_lost > 0 ? ToastLevel::Warning : ToastLevel::Info, message);
  // Problems that did not fail the capture. The trace's own warnings (the CaptureInfo the converters write)
  // are shown by the warnings banner once the trace is open (WP-48); what only the agent knows (spool caps
  // hit, refused connections, merge failures) has no place in the trace, so it is toasted here and (WP-67)
  // handed to the banner for the trace, which shows it when the trace is open.
  constexpr size_t kMaxToasted = 3;
  for (size_t i = 0; i < s.warnings.size() && i < kMaxToasted; ++i) toast(ToastLevel::Warning, s.warnings[i]);
  if (s.warnings.size() > kMaxToasted)
    toast(ToastLevel::Warning,
          "... and " + std::to_string(s.warnings.size() - kMaxToasted) + " more capture warnings");
  for (const std::string& w : s.warnings) TACHYON_LOG_WARN("capture: {}", w);

  if (m_session.snapshot().remote && !s.path.empty()) {
    // The trace is on the agent's machine: bring it here first (in the background); it is listed and opened
    // when it has arrived.
    const bool open = m_openNextCapture || (m_host.settings && m_host.settings->capture.autoOpen);
    m_openNextCapture = false;
    fetchRemote(s.path, open, /*isCapture=*/true, s.warnings);
    return;
  }
  const bool open =
      (m_openNextCapture || (m_host.settings && m_host.settings->capture.autoOpen)) && !s.path.empty();
  m_openNextCapture = false;
  if (!s.path.empty())
    finishLocalCapture(path, open, s.warnings);
  else if (m_host.capturesChanged)
    m_host.capturesChanged();
}

// A finished capture that is a file on this machine: remember it, refresh the Session Explorer and open it.
void CaptureController::finishLocalCapture(const std::filesystem::path& path, bool open,
                                           std::vector<std::string> warnings) {
  if (!warnings.empty() && m_host.captureWarnings) m_host.captureWarnings(path, std::move(warnings));
  if (m_host.settings) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    m_host.settings->addRecentFile(pathToUtf8(ec ? path : absolute));
    if (m_host.saveSettings) m_host.saveSettings();
  }
  if (m_host.capturesChanged) m_host.capturesChanged();
  if (open && m_host.openFile) m_host.openFile(path);
}

// ---- dialogs ----

void CaptureController::openDialog(capture::CaptureMode mode, bool live) {
  const CaptureSettings empty;
  const CaptureSettings& settings = m_host.settings ? m_host.settings->capture : empty;
  m_form = formFromSettings(settings, mode);
  m_form.live = live;
  m_form.remote = m_session.snapshot().remote;
  m_form.outPath.clear();
  m_form.pid = 0;
  m_form.processName.clear();
  m_selectedPid = 0;
  m_errors.clear();
  m_showErrors = false;
  m_formDirty = true;
  m_dialogOpen = true;
  m_dialogJustOpened = true;
  m_processFilter.clear();
  if (mode == capture::CaptureMode::Attach) refreshProcesses();
  if (m_host.requestRedraw) m_host.requestRedraw();
}

void CaptureController::refreshProcesses() {
  if (m_processJob != 0 && m_host.jobs) m_host.jobs->cancel(m_processJob);
  m_processError.clear();
  if (m_session.snapshot().remote) {
    // The processes of the other machine come from the agent (`processes` command).
    if (m_session.snapshot().connection == ConnectionState::Connected) {
      m_remoteProcessesPending = true;
      m_session.listProcesses();
    } else {
      m_processError = "Connect to the remote agent to list its processes";
    }
    return;
  }
  if (!m_host.jobs) {
    auto list = enumerateProcesses();
    if (list.ok()) {
      m_processes = std::move(*list);
    } else {
      m_processError = list.error().message;
    }
    return;
  }
  using Loaded = std::optional<Result<std::vector<ProcessInfo>>>;
  auto loaded = std::make_shared<Loaded>();
  m_processJob = m_host.jobs->submit(
      "Listing processes", [loaded](JobContext&) { loaded->emplace(enumerateProcesses()); },
      [this, loaded](JobQueue::JobId id, JobOutcome outcome) {
        if (id != m_processJob) return;
        m_processJob = 0;
        if (outcome == JobOutcome::Cancelled || !loaded->has_value()) return;
        if ((*loaded)->ok()) {
          m_processes = std::move((*loaded)->value());
        } else {
          m_processError = (*loaded)->error().message;
        }
        if (m_host.requestRedraw) m_host.requestRedraw();
      });
}

void CaptureController::selectProcess(uint32_t pid) {
  m_selectedPid = pid;
  m_form.pid = pid;
  m_form.processName.clear();
  for (const ProcessInfo& p : m_processes) {
    if (p.pid == pid) m_form.processName = p.name;
  }
  m_formDirty = true;
}

void CaptureController::revalidate() {
  m_form.remote = m_session.snapshot().remote;
  m_errors = validateForm(m_form);
  m_validatedForm = m_form;
  m_formDirty = false;
  m_lastValidateFrame = ImGui::GetFrameCount();
}

bool CaptureController::submitDialog() {
  revalidate();
  m_showErrors = true;
  const AgentSnapshot snap = m_session.snapshot();
  if (snap.connection != ConnectionState::Connected) {
    m_errors.push_back({"agent", "The agent is not connected"});
    return false;
  }
  if (snap.phase != AgentPhase::Idle) {
    m_errors.push_back({"agent", std::string("The agent is busy (") + agentPhaseName(snap.phase) + ")"});
    return false;
  }
  if (!m_errors.empty()) return false;
  const std::filesystem::path captures =
      capturesDir(m_host.settings ? m_host.settings->capture : CaptureSettings());
  m_form.remote = snap.remote;
  auto request = buildStartRequest(m_form, captures, std::time(nullptr));
  if (!request.ok()) {
    m_errors.push_back({"outPath", request.error().message});
    return false;
  }
  if (!snap.remote) {
    std::error_code ec;
    const std::filesystem::path out = pathFromUtf8(request->out_path);
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
    if (ec) {
      m_errors.push_back({"outPath", "Cannot create " + pathToUtf8(out.parent_path()) + ": " + ec.message()});
      return false;
    }
  }
  if (m_host.settings) {
    storeFormInSettings(m_form, m_host.settings->capture);
    if (m_host.saveSettings) m_host.saveSettings();
  }
  m_lastRequest = *request;
  // WP-65b: the Live document must exist (and the session must know its model) before the first `live` event
  // can arrive.
  if (request->live && m_host.liveBegin) m_session.setLiveModel(m_host.liveBegin(*request));
  m_openNextCapture = false;
  m_session.start(*request, describeTarget(m_form));
  m_dialogOpen = false;
  if (m_host.showPanel) m_host.showPanel(kCapturePanelName);
  if (m_host.requestRedraw) m_host.requestRedraw();
  return true;
}

void CaptureController::fieldError(const char* field) {
  if (!m_showErrors) return;
  for (const FormError& e : m_errors) {
    if (e.field == field) ImGui::TextColored(kRed, "%s", e.message.c_str());
  }
}

void CaptureController::drawAgentControls() {
  const AgentSnapshot snap = m_session.snapshot();
  ImGui::TextUnformatted("Agent:");
  ImGui::SameLine();
  switch (snap.connection) {
    case ConnectionState::Connected:
      ImGui::TextColored(kGreen, "connected");
      ImGui::SameLine();
      ImGui::TextDisabled("(%s backend%s) %s", snap.backend.c_str(), snap.elevated ? ", elevated" : "",
                          snap.address.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Disconnect")) disconnectAgent();
      return;
    case ConnectionState::Connecting: ImGui::TextColored(kAmber, "connecting..."); return;
    case ConnectionState::Lost: ImGui::TextColored(kRed, "connection lost"); break;
    case ConnectionState::Disconnected: ImGui::TextColored(kAmber, "not connected"); break;
  }
  ImGui::TextDisabled("%s", snap.address.c_str());
  if (ImGui::Button("Reconnect")) reconnect();
  ImGui::SameLine();
  const bool needsElevation = agentNeedsElevation();
  if (needsElevation) {
    if (ImGui::Button(TACHYON_PLATFORM_WINDOWS ? "Start agent (administrator)" : "Start with pkexec"))
      startAgent(true);
    ImGui::SameLine();
    if (ImGui::Button("Start without elevation")) startAgent(false);
    ImGui::TextDisabled("The %s backend needs %s.",
                        m_host.settings && !m_host.settings->capture.agentBackend.empty()
                            ? m_host.settings->capture.agentBackend.c_str()
                            : (TACHYON_PLATFORM_WINDOWS ? "etw" : "perf"),
                        TACHYON_PLATFORM_WINDOWS ? "an elevated agent" : "root or CAP_PERFMON");
  } else if (ImGui::Button("Start agent")) {
    startAgent(false);
  }
}

void CaptureController::drawProcessTable() {
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
  inputString("##processFilter", m_processFilter, "Filter (pid, name, user, command)");
  ImGui::SameLine();
  if (ImGui::Button("Refresh")) refreshProcesses();
  if (processesLoading()) {
    ImGui::SameLine();
    ImGui::TextDisabled("listing...");
  }
  if (!m_processError.empty()) ImGui::TextColored(kRed, "%s", m_processError.c_str());
  const std::vector<size_t> rows = filteredProcesses();
  if (ImGui::BeginTable("##processes", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders,
                        ImVec2(0.0f, ImGui::GetFontSize() * 12.0f))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Command line", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const ProcessInfo& p = m_processes[rows[static_cast<size_t>(i)]];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(p.pid));
        char pidText[16];
        std::snprintf(pidText, sizeof(pidText), "%u", p.pid);
        if (ImGui::Selectable(pidText, m_selectedPid == p.pid, ImGuiSelectableFlags_SpanAllColumns))
          selectProcess(p.pid);
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.user.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.commandLine.c_str());
      }
    }
    ImGui::EndTable();
  }
  if (m_selectedPid != 0) {
    ImGui::Text("Selected: pid %u %s", m_selectedPid, m_form.processName.c_str());
  }
  fieldError("pid");
}

void CaptureController::drawDialog() {
  if (!m_dialogOpen) {
    if (m_picker.isOpen()) m_picker.draw();
    return;
  }
  if (m_formDirty || !(m_form == m_validatedForm) || ImGui::GetFrameCount() - m_lastValidateFrame >= 20)
    revalidate();
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40.0f, 0.0f), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
  const bool remote = m_form.remote;
  if (ImGui::Begin(modeTitle(m_form.mode, remote, m_form.live), &open, flags)) {
    if (m_dialogJustOpened) {
      ImGui::SetWindowFocus();
      m_dialogJustOpened = false;
    }
    drawAgentControls();
    if (remote)
      ImGui::TextColored(kAmber, "Remote agent: paths and processes below are on the agent's machine.");
    ImGui::Separator();
    bool changed = false;
    if (m_form.live) {
      ImGui::TextWrapped(
          "Live: the viewer shows the last 30 seconds of the capture, scrolling, while it records. The "
          "capture "
          "is still written to a normal trace when you stop.");
      int mode = static_cast<int>(m_form.mode);
      const int before = mode;
      ImGui::RadioButton("Run application", &mode, static_cast<int>(capture::CaptureMode::Launch));
      ImGui::SameLine();
      ImGui::RadioButton("Attach to process", &mode, static_cast<int>(capture::CaptureMode::Attach));
      ImGui::SameLine();
      ImGui::RadioButton("System", &mode, static_cast<int>(capture::CaptureMode::System));
      if (mode != before) {
        m_form.mode = static_cast<capture::CaptureMode>(mode);
        if (m_form.mode == capture::CaptureMode::Attach) refreshProcesses();
        changed = true;
      }
      ImGui::Separator();
    }
    if (m_form.mode == capture::CaptureMode::Launch) {
      changed |= inputString(remote ? "Executable (on agent)" : "Executable", m_form.exe);
      ImGui::SameLine();
      if (!remote && ImGui::Button("Browse...##exe")) {
        m_pickTarget = PickTarget::Exe;
        m_picker.open(
            pathFromUtf8(m_form.exe).parent_path(), "Choose executable",
            "Choose the program to run (double-click a file or select it and use \"Use selected file\").");
      }
      fieldError("exe");
      changed |= inputString("Arguments", m_form.args, "quoted like a shell: --scene \"My Level\"");
      fieldError("args");
      changed |= inputString(remote ? "Working directory (on agent)" : "Working directory", m_form.cwd,
                             remote ? "empty = the agent's directory" : "empty = the viewer's directory");
      ImGui::SameLine();
      if (!remote && ImGui::Button("Browse...##cwd")) {
        m_pickTarget = PickTarget::Cwd;
        m_picker.open(pathFromUtf8(m_form.cwd), "Choose working directory",
                      "Choose the directory and use \"Use this directory\".");
      }
      fieldError("cwd");
      ImGui::TextUnformatted("Environment");
      int removeAt = -1;
      for (size_t i = 0; i < m_form.env.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 3.0f);
        changed |= inputString("##env", m_form.env[i], "KEY=VALUE");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::PopID();
      }
      if (removeAt >= 0) {
        m_form.env.erase(m_form.env.begin() + removeAt);
        changed = true;
      }
      if (ImGui::SmallButton("Add variable")) {
        m_form.env.emplace_back();
        changed = true;
      }
      fieldError("env");
      changed |= ImGui::Checkbox("Start immediately", &m_form.startImmediately);
      if (!m_form.startImmediately) {
        ImGui::SameLine();
        ImGui::TextDisabled("(launched parked; press Resume to start recording)");
      }
    } else if (m_form.mode == capture::CaptureMode::Attach) {
      drawProcessTable();
    } else {
      ImGui::TextWrapped(
          remote ? "Samples every process on the agent's machine. The agent needs administrator / "
                   "root rights for this."
                 : "Samples every process on this machine. The agent needs administrator / "
                   "root rights for this.");
    }
    ImGui::Separator();
    changed |= ImGui::InputInt("Sampling rate (Hz)", &m_form.samplingHz, 100, 1000);
    fieldError("samplingHz");
    changed |= ImGui::InputInt("Circular buffer (MB, 0 = off)", &m_form.circularMb, 16, 256);
    if (m_form.live && m_form.circularMb == 0)
      ImGui::TextDisabled("A circular buffer lets the Live window save the last 30 s while recording.");
    fieldError("circularMb");
    changed |= ImGui::Checkbox("Follow children", &m_form.followChildren);
    if (m_form.followChildren) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
      changed |= inputString("Child filter", m_form.childFilter, "e.g. *.exe");
    }
    fieldError("childFilter");
    changed |= ImGui::Checkbox("Kernel stacks", &m_form.kernelStacks);
    changed |=
        inputString(remote ? "Output name (on agent)" : "Output file", m_form.outPath,
                    remote ? "file name only; empty = generated. It is fetched into the captures directory"
                           : "empty = <captures dir>/<name>-<date>.tachyon");
    ImGui::SameLine();
    if (!remote && ImGui::Button("Browse...##out")) {
      m_pickTarget = PickTarget::Out;
      m_picker.open(
          capturesDir(m_host.settings ? m_host.settings->capture : CaptureSettings()),
          "Choose output directory",
          "Choose the directory for the capture and use \"Use this directory\"; the file name is generated.");
    }
    fieldError("outPath");
    if (changed) m_formDirty = true;

    ImGui::Separator();
    if (m_showErrors) fieldError("agent");
    const AgentSnapshot snap = m_session.snapshot();
    const bool canStart = snap.connection == ConnectionState::Connected && snap.phase == AgentPhase::Idle;
    ImGui::BeginDisabled(!canStart);
    if (ImGui::Button(m_form.mode == capture::CaptureMode::Launch && !m_form.startImmediately
                          ? "Launch (parked)"
                          : "Start capture"))
      submitDialog();
    ImGui::EndDisabled();
    if (!canStart && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", snap.connection != ConnectionState::Connected ? "The agent is not connected"
                                                                            : "The agent is busy");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) open = false;
    if (!m_errors.empty() && !m_showErrors) {
      ImGui::SameLine();
      ImGui::TextDisabled("%zu problem(s)", m_errors.size());
    }
  }
  ImGui::End();
  if (!open) m_dialogOpen = false;

  if (auto chosen = m_picker.draw()) {
    switch (m_pickTarget) {
      case PickTarget::Exe: m_form.exe = pathToUtf8(*chosen); break;
      case PickTarget::Cwd: m_form.cwd = pathToUtf8(*chosen); break;
      case PickTarget::Out:
        m_form.outPath.clear();
        if (m_host.settings) {
          m_host.settings->capture.capturesDir = pathToUtf8(*chosen);
          if (m_host.saveSettings) m_host.saveSettings();
        }
        break;
      case PickTarget::None: break;
    }
    m_pickTarget = PickTarget::None;
    m_formDirty = true;
  }
}

void CaptureController::draw() {
  drawDialog();
  drawRemoteDialog();
}

void CaptureController::drawRemoteDialog() {
  if (!m_remoteDialogOpen) return;
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, 0.0f), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
  if (ImGui::Begin("Connect to Remote Agent##remote-dialog", &open, flags)) {
    if (m_remoteDialogJustOpened) {
      ImGui::SetWindowFocus();
      m_remoteDialogJustOpened = false;
    }
    ImGui::TextWrapped(
        "A tachyon-agent on another machine, started with --listen HOST:PORT --token-file FILE. Traces are "
        "copied here when a capture finishes. The connection is not encrypted: use a trusted network or an "
        "SSH tunnel (ssh -L 7331:localhost:7331 host, then connect to localhost:7331).");
    inputString("Address", m_remoteAddressText, "host:port, e.g. build-box:7331 or [::1]:7331");
    inputString("Token file", m_remoteTokenText, "path of the file the agent was started with");
    ImGui::TextDisabled("Only the path of the token file is saved, never the token.");
    const bool testing = m_probeJob != 0;
    ImGui::BeginDisabled(testing || m_remoteAddressText.empty());
    if (ImGui::Button("Test connection")) testRemote();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (testing) {
      ImGui::TextDisabled("testing...");
    } else if (m_remoteProbe.done) {
      ImGui::TextColored(m_remoteProbe.ok ? kGreen : kRed, "%s", m_remoteProbe.message.c_str());
    }
    ImGui::Separator();
    ImGui::BeginDisabled(m_remoteAddressText.empty());
    if (ImGui::Button("Connect")) {
      if (connectRemote(m_remoteAddressText, m_remoteTokenText)) open = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) open = false;
  }
  ImGui::End();
  if (!open) m_remoteDialogOpen = false;
}

void CaptureController::drawTransfers() {
  if (m_transfers.empty()) return;
  ImGui::Separator();
  ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (!ImGui::CollapsingHeader("Transfers from the remote agent")) return;
  std::vector<JobQueue::Info> jobs;
  if (m_host.jobs) jobs = m_host.jobs->active();
  for (size_t i = m_transfers.size(); i-- > 0;) {
    const RemoteTransfer& t = m_transfers[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::TextUnformatted(pathToUtf8(t.localPath.filename()).c_str());
    ImGui::SameLine();
    switch (t.state) {
      case RemoteTransfer::State::Running: {
        float progress = 0.0f;
        for (const JobQueue::Info& j : jobs)
          if (j.id == t.jobId) progress = j.progress;
        ImGui::ProgressBar(progress, ImVec2(ImGui::GetFontSize() * 10.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) cancelTransfer(i);
        break;
      }
      case RemoteTransfer::State::Done: ImGui::TextColored(kGreen, "done"); break;
      case RemoteTransfer::State::Cancelled:
      case RemoteTransfer::State::Failed:
        ImGui::TextColored(t.state == RemoteTransfer::State::Failed ? kRed : kAmber, "%s", t.error.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Retry")) retryTransfer(i);
        break;
    }
    ImGui::PopID();
  }
}

// ---- menu, status bar, panel ----

void CaptureController::drawMenu() {
  if (!ImGui::BeginMenu("Capture")) return;
  const AgentSnapshot snap = m_session.snapshot();
  if (ImGui::MenuItem("Run...")) openDialog(capture::CaptureMode::Launch);
  if (ImGui::MenuItem("Attach...")) openDialog(capture::CaptureMode::Attach);
  if (ImGui::MenuItem("System capture...")) openDialog(capture::CaptureMode::System);
  if (ImGui::MenuItem("Live...")) openDialog(capture::CaptureMode::Launch, true);
  ImGui::Separator();
  if (ImGui::MenuItem("Resume", nullptr, false, snap.phase == AgentPhase::Armed)) resumeCapture();
  if (ImGui::MenuItem("Stop", nullptr, false, snap.phase == AgentPhase::Capturing)) stopCapture();
  if (ImGui::MenuItem("Cancel", nullptr, false,
                      snap.phase == AgentPhase::Armed || snap.phase == AgentPhase::Capturing))
    cancelCapture();
  ImGui::Separator();
  if (ImGui::MenuItem("Capture Window") && m_host.showPanel) m_host.showPanel(kCapturePanelName);
  if (ImGui::MenuItem("Flight Recorder Window") && m_host.showPanel)
    m_host.showPanel(kFlightRecorderPanelName);
  const bool disconnected =
      snap.connection == ConnectionState::Disconnected || snap.connection == ConnectionState::Lost;
  if (ImGui::MenuItem("Reconnect to Agent", nullptr, false, disconnected)) reconnect();
  if (ImGui::MenuItem("Start Agent", nullptr, false, disconnected)) startAgent(agentNeedsElevation());
  ImGui::Separator();
  if (ImGui::MenuItem("Connect to Remote Agent...")) openRemoteDialog();
  if (ImGui::MenuItem("Use Local Agent", nullptr, false, m_connectedRemote || snap.remote)) useLocalAgent();
  ImGui::EndMenu();
}

std::string CaptureController::statusText() const {
  const AgentSnapshot s = m_session.snapshot();
  switch (s.connection) {
    case ConnectionState::Disconnected: return "Agent: not connected";
    case ConnectionState::Connecting: return "Agent: connecting...";
    case ConnectionState::Lost: return "Agent: connection lost";
    case ConnectionState::Connected: break;
  }
  std::string text = std::string(s.remote ? "Remote agent: " : "Agent: ") +
                     (s.backend.empty() ? std::string("connected") : s.backend);
  switch (s.phase) {
    case AgentPhase::Armed: text += " | Armed: " + s.target; break;
    case AgentPhase::Starting: text += " | Starting..."; break;
    case AgentPhase::Capturing:
      text += " | REC " + formatElapsed(s.elapsedSeconds) + (s.target.empty() ? "" : " " + s.target);
      break;
    case AgentPhase::Stopping:
      text += " | Converting " + std::to_string(static_cast<int>(s.progress * 100.0)) + "%";
      break;
    case AgentPhase::Idle:
    case AgentPhase::Unknown: break;
  }
  return text;
}

void CaptureController::drawStatusSegment() {
  const AgentSnapshot s = m_session.snapshot();
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  const std::string text = statusText();
  ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
  if (s.connection == ConnectionState::Lost) color = kRed;
  if (s.connection == ConnectionState::Connected) color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  if (s.phase == AgentPhase::Capturing) color = kRed;
  if (s.phase == AgentPhase::Armed) color = kAmber;
  ImGui::TextColored(color, "%s", text.c_str());
  if (ImGui::IsItemClicked() && m_host.showPanel) m_host.showPanel(kCapturePanelName);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to open the Capture window");
  if (s.connection == ConnectionState::Connected) {
    ImGui::SameLine();
    if (s.phase == AgentPhase::Armed && ImGui::SmallButton("Resume##statusbar")) resumeCapture();
    if (s.phase == AgentPhase::Capturing && ImGui::SmallButton("Stop##statusbar")) stopCapture();
  } else if (s.connection != ConnectionState::Connecting) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Reconnect##statusbar")) reconnect();
  }
}

void CaptureController::drawPanelContents() {
  drawAgentControls();
  ImGui::Separator();
  const AgentSnapshot snap = m_session.snapshot();
  if (snap.connection == ConnectionState::Connected) {
    ImGui::Text("State: %s", agentPhaseName(snap.phase));
    if (!snap.target.empty()) ImGui::Text("Target: %s", snap.target.c_str());
    if (snap.phase == AgentPhase::Capturing)
      ImGui::Text("Elapsed: %s", formatElapsed(snap.elapsedSeconds).c_str());
    if (snap.phase == AgentPhase::Stopping)
      ImGui::ProgressBar(static_cast<float>(snap.progress), ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f),
                         "Converting");
    if (snap.phase == AgentPhase::Idle) {
      if (ImGui::Button("Run...")) openDialog(capture::CaptureMode::Launch);
      ImGui::SameLine();
      if (ImGui::Button("Attach...")) openDialog(capture::CaptureMode::Attach);
      ImGui::SameLine();
      if (ImGui::Button("System...")) openDialog(capture::CaptureMode::System);
      ImGui::SameLine();
      if (ImGui::Button("Live...")) openDialog(capture::CaptureMode::Launch, true);
    }
    if (snap.phase == AgentPhase::Armed) {
      if (ImGui::Button("Resume")) resumeCapture();
      ImGui::SameLine();
    }
    if (snap.phase == AgentPhase::Capturing) {
      if (ImGui::Button("Stop")) stopCapture();
      ImGui::SameLine();
    }
    if (snap.phase == AgentPhase::Armed || snap.phase == AgentPhase::Capturing) {
      if (ImGui::Button("Cancel")) cancelCapture();
    }
  }
  if (!snap.lastError.empty()) ImGui::TextColored(kRed, "%s", snap.lastError.c_str());
  if (ImGui::Button("Connect to remote agent...")) openRemoteDialog();
  drawTransfers();
  drawAgentSettings();
}

void CaptureController::drawAgentSettings() {
  if (!m_host.settings) return;
  ImGui::Separator();
  ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (!ImGui::CollapsingHeader("Agent settings")) return;
  CaptureSettings& c = m_host.settings->capture;
  bool changed = false;
  // Text fields are saved when they lose focus after an edit, the rest at once.
  auto text = [&](const char* label, std::string& value, const char* hint) {
    inputString(label, value, hint);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
  };
  const char* backends[] = {"(default for this platform)", "mock", "perf", "etw"};
  int backend = 0;
  for (int i = 1; i < 4; ++i)
    if (c.agentBackend == backends[i]) backend = i;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  if (ImGui::Combo("Backend of a started agent", &backend, backends, 4)) {
    c.agentBackend = backend == 0 ? std::string() : std::string(backends[backend]);
    changed = true;
  }
  text("Agent executable", c.agentExe, "empty = next to the viewer");
  text("Agent address", c.agentAddress, "empty = the platform default");
  text("Captures directory", c.capturesDir, "empty = <user data dir>/captures");
  changed |= ImGui::Checkbox("Connect to a running agent at startup", &c.autoConnect);
  changed |= ImGui::Checkbox("Open the trace when a capture finishes", &c.autoOpen);
  if (changed && m_host.saveSettings) m_host.saveSettings();
}

// ---- flight recorder ----

FlightRecorderState CaptureController::flightRecorderState() const {
  const AgentSnapshot snap = m_session.snapshot();
  FlightRecorderState st;
  st.connected = snap.connection == ConnectionState::Connected;
  st.capturing = st.connected && snap.phase == AgentPhase::Capturing;
  st.armed = st.capturing && snap.circularMb > 0;
  st.circularUnknown = st.capturing && snap.circularMb == 0;
  st.ringMb = snap.circularMb;
  st.elapsedSeconds = snap.elapsedSeconds;
  st.target = snap.target;
  st.phase = agentPhaseName(snap.phase);
  return st;
}

bool CaptureController::flushNow() {
  return flushSeconds(m_flushSeconds);
}

bool CaptureController::flushSeconds(int requestedSeconds) {
  const FlightRecorderState st = flightRecorderState();
  if (!st.capturing) {
    toast(ToastLevel::Warning, "Nothing to flush: no capture is running");
    return false;
  }
  if (st.circularUnknown)
    toast(ToastLevel::Info, "Asking the agent to flush; this works only if the capture is circular");
  const int seconds = std::clamp(requestedSeconds, 1, 3600);
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if TACHYON_PLATFORM_WINDOWS
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  m_session.flush(std::string("manual-") + stamp + "-" + std::to_string(++m_flushSerial),
                  static_cast<uint32_t>(seconds), "manual (viewer)");
  return true;
}

void CaptureController::openFlushed(size_t index) {
  if (index >= m_flushed.size() || !m_host.openFile) return;
  const FlushedCapture& f = m_flushed[index];
  if (!f.fetched) {  // still on the remote agent: bring it here, then open it
    toast(ToastLevel::Info, "Fetching " + f.name + " from the remote agent...");
    fetchRemote(f.remotePath, /*openWhenDone=*/true, /*isCapture=*/false);
    return;
  }
  m_host.openFile(pathFromUtf8(f.path));
}

void CaptureController::drawFlightRecorderContents() {
  const FlightRecorderState st = flightRecorderState();
  if (!st.connected) {
    ImGui::TextDisabled("Not connected to the agent (Capture > Reconnect to Agent).");
  } else if (st.armed) {
    ImGui::TextColored(kGreen, "ARMED");
    ImGui::SameLine();
    ImGui::Text("%s | ring %u MB | recording %s", st.target.empty() ? "capture" : st.target.c_str(),
                st.ringMb, formatElapsed(st.elapsedSeconds).c_str());
  } else if (st.circularUnknown) {
    ImGui::TextColored(kAmber, "Capturing (started elsewhere)");
    ImGui::TextWrapped(
        "This viewer did not start the capture, so it cannot tell whether it is circular. Flush now works "
        "only for a capture started with a circular buffer.");
  } else {
    ImGui::TextDisabled(
        "Not armed. Start a capture with a circular buffer (Capture > Run/Attach/System, "
        "'Circular buffer (MB)' > 0) to record continuously and flush the last seconds.");
  }
  ImGui::Separator();
  ImGui::BeginDisabled(!st.capturing);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
  ImGui::InputInt("seconds", &m_flushSeconds, 1, 10);
  m_flushSeconds = std::clamp(m_flushSeconds, 1, 3600);
  ImGui::SameLine();
  if (ImGui::Button("Flush now")) flushNow();
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::Text("Flushed captures (%zu)", m_flushed.size());
  if (m_flushed.empty()) {
    ImGui::TextDisabled(
        "None yet. The engine's Tachyon_RequestFlush() and 'tachyon-cli flush' show up here too.");
    return;
  }
  if (!ImGui::BeginTable("##flushed", 5,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                             ImGuiTableFlags_SizingStretchProp,
                         ImVec2(0.0f, 0.0f)))
    return;
  ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
  ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("Span", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
  ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.0f);
  ImGui::TableSetupColumn("##open", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();
  for (size_t i = m_flushed.size(); i-- > 0;) {  // newest first
    const FlushedCapture& f = m_flushed[i];
    ImGui::TableNextRow();
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextColumn();
    const std::time_t t = std::chrono::system_clock::to_time_t(f.at);
    std::tm tm{};
#if TACHYON_PLATFORM_WINDOWS
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char clock[16];
    std::strftime(clock, sizeof(clock), "%H:%M:%S", &tm);
    ImGui::TextUnformatted(clock);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(f.reason.c_str());
    if (f.coalesced > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(+%u)", f.coalesced);
    }
    ImGui::TableNextColumn();
    ImGui::Text("%.1f s", f.durationS);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(pathToUtf8(pathFromUtf8(f.path).filename()).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.path.c_str());
    ImGui::TableNextColumn();
    if (ImGui::SmallButton("Open")) openFlushed(i);
    ImGui::PopID();
  }
  ImGui::EndTable();
}

namespace {

class FlightRecorderPanel final : public IPanel {
 public:
  explicit FlightRecorderPanel(CaptureController& controller) : m_controller(controller) {}
  const char* name() const override { return kFlightRecorderPanelName; }
  void draw(const PanelContext&, bool* open) override {
    if (ImGui::Begin(name(), open)) m_controller.drawFlightRecorderContents();
    ImGui::End();
  }

 private:
  CaptureController& m_controller;
};

class CapturePanel final : public IPanel {
 public:
  explicit CapturePanel(CaptureController& controller) : m_controller(controller) {}
  const char* name() const override { return kCapturePanelName; }
  void draw(const PanelContext&, bool* open) override {
    if (ImGui::Begin(name(), open)) m_controller.drawPanelContents();
    ImGui::End();
  }

 private:
  CaptureController& m_controller;
};

}  // namespace

std::unique_ptr<IPanel> makeCapturePanel(CaptureController& controller) {
  return std::make_unique<CapturePanel>(controller);
}

std::unique_ptr<IPanel> makeFlightRecorderPanel(CaptureController& controller) {
  return std::make_unique<FlightRecorderPanel>(controller);
}

}  // namespace tachyon::viewer
