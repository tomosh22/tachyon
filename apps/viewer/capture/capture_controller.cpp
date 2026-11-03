#include "viewer/capture/capture_controller.h"

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

namespace tachyon::viewer {

using capture_ui::inputString;

namespace {

constexpr int kStartAgentRetryMs = 8000;
constexpr int kElevatedRetryMs = 60000;  // time to type a password / answer the UAC prompt
constexpr ImVec4 kRed(1.0f, 0.4f, 0.4f, 1.0f);
constexpr ImVec4 kAmber(1.0f, 0.75f, 0.2f, 1.0f);
constexpr ImVec4 kGreen(0.4f, 0.85f, 0.45f, 1.0f);

const char* modeTitle(capture::CaptureMode mode) {
  switch (mode) {
    case capture::CaptureMode::Launch: return "Run Application##capture-dialog";
    case capture::CaptureMode::Attach: return "Attach to Process##capture-dialog";
    case capture::CaptureMode::System: return "System Capture##capture-dialog";
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
  m_session.connect(agentAddress(), retryMs, userInitiated);
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

// ---- capture commands ----

void CaptureController::resumeCapture() {
  m_session.resume();
}

void CaptureController::stopCapture() {
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
  return !events.empty();
}

void CaptureController::handleEvent(const AgentEvent& e) {
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
        // An agent started as another user (pkexec / sudo) creates a socket this user cannot open.
        if (text.find("ermission denied") != std::string::npos || text.find("ACCES") != std::string::npos)
          text += " (the agent runs as another user and its socket is not accessible to you)";
        toast(ToastLevel::Warning, std::move(text));
      }
      break;
    case AgentEventKind::ConnectionLost:
      toast(ToastLevel::Error, e.message + " (Capture > Reconnect)");
      break;
    case AgentEventKind::StateChanged:
      if ((e.phase == AgentPhase::Capturing || e.phase == AgentPhase::Armed) && m_lastPhase != e.phase &&
          m_host.showPanel)
        m_host.showPanel(kCapturePanelName);
      m_lastPhase = e.phase;
      break;
    case AgentEventKind::CaptureDone: onCaptureDone(e); break;
    case AgentEventKind::AgentError: toast(ToastLevel::Error, "Agent: " + e.message); break;
    case AgentEventKind::CommandFailed:
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
  // hit, refused connections, merge failures) has no place in the trace, so it is toasted here.
  constexpr size_t kMaxToasted = 3;
  for (size_t i = 0; i < s.warnings.size() && i < kMaxToasted; ++i) toast(ToastLevel::Warning, s.warnings[i]);
  if (s.warnings.size() > kMaxToasted)
    toast(ToastLevel::Warning,
          "... and " + std::to_string(s.warnings.size() - kMaxToasted) + " more capture warnings");
  for (const std::string& w : s.warnings) TACHYON_LOG_WARN("capture: {}", w);

  if (m_host.settings && !s.path.empty()) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    m_host.settings->addRecentFile(pathToUtf8(ec ? path : absolute));
    if (m_host.saveSettings) m_host.saveSettings();
  }
  if (m_host.capturesChanged) m_host.capturesChanged();
  if (m_host.settings && m_host.settings->capture.autoOpen && !s.path.empty() && m_host.openFile)
    m_host.openFile(path);
}

// ---- dialogs ----

void CaptureController::openDialog(capture::CaptureMode mode) {
  const CaptureSettings empty;
  const CaptureSettings& settings = m_host.settings ? m_host.settings->capture : empty;
  m_form = formFromSettings(settings, mode);
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
  auto request = buildStartRequest(m_form, captures, std::time(nullptr));
  if (!request.ok()) {
    m_errors.push_back({"outPath", request.error().message});
    return false;
  }
  std::error_code ec;
  const std::filesystem::path out = pathFromUtf8(request->out_path);
  if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
  if (ec) {
    m_errors.push_back({"outPath", "Cannot create " + pathToUtf8(out.parent_path()) + ": " + ec.message()});
    return false;
  }
  if (m_host.settings) {
    storeFormInSettings(m_form, m_host.settings->capture);
    if (m_host.saveSettings) m_host.saveSettings();
  }
  m_lastRequest = *request;
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
  if (m_processJob != 0) {
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
  if (ImGui::Begin(modeTitle(m_form.mode), &open, flags)) {
    if (m_dialogJustOpened) {
      ImGui::SetWindowFocus();
      m_dialogJustOpened = false;
    }
    drawAgentControls();
    ImGui::Separator();
    bool changed = false;
    if (m_form.mode == capture::CaptureMode::Launch) {
      changed |= inputString("Executable", m_form.exe);
      ImGui::SameLine();
      if (ImGui::Button("Browse...##exe")) {
        m_pickTarget = PickTarget::Exe;
        m_picker.open(
            pathFromUtf8(m_form.exe).parent_path(), "Choose executable",
            "Choose the program to run (double-click a file or select it and use \"Use selected file\").");
      }
      fieldError("exe");
      changed |= inputString("Arguments", m_form.args, "quoted like a shell: --scene \"My Level\"");
      fieldError("args");
      changed |= inputString("Working directory", m_form.cwd, "empty = the viewer's directory");
      ImGui::SameLine();
      if (ImGui::Button("Browse...##cwd")) {
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
          "Samples every process on this machine. The agent needs administrator / root rights for this.");
    }
    ImGui::Separator();
    changed |= ImGui::InputInt("Sampling rate (Hz)", &m_form.samplingHz, 100, 1000);
    fieldError("samplingHz");
    changed |= ImGui::InputInt("Circular buffer (MB, 0 = off)", &m_form.circularMb, 16, 256);
    fieldError("circularMb");
    changed |= ImGui::Checkbox("Follow children", &m_form.followChildren);
    if (m_form.followChildren) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
      changed |= inputString("Child filter", m_form.childFilter, "e.g. *.exe");
    }
    fieldError("childFilter");
    changed |= ImGui::Checkbox("Kernel stacks", &m_form.kernelStacks);
    changed |= inputString("Output file", m_form.outPath, "empty = <captures dir>/<name>-<date>.tachyon");
    ImGui::SameLine();
    if (ImGui::Button("Browse...##out")) {
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
}

// ---- menu, status bar, panel ----

void CaptureController::drawMenu() {
  if (!ImGui::BeginMenu("Capture")) return;
  const AgentSnapshot snap = m_session.snapshot();
  if (ImGui::MenuItem("Run...")) openDialog(capture::CaptureMode::Launch);
  if (ImGui::MenuItem("Attach...")) openDialog(capture::CaptureMode::Attach);
  if (ImGui::MenuItem("System capture...")) openDialog(capture::CaptureMode::System);
  ImGui::Separator();
  if (ImGui::MenuItem("Resume", nullptr, false, snap.phase == AgentPhase::Armed)) resumeCapture();
  if (ImGui::MenuItem("Stop", nullptr, false, snap.phase == AgentPhase::Capturing)) stopCapture();
  if (ImGui::MenuItem("Cancel", nullptr, false,
                      snap.phase == AgentPhase::Armed || snap.phase == AgentPhase::Capturing))
    cancelCapture();
  ImGui::Separator();
  if (ImGui::MenuItem("Capture Window") && m_host.showPanel) m_host.showPanel(kCapturePanelName);
  const bool disconnected =
      snap.connection == ConnectionState::Disconnected || snap.connection == ConnectionState::Lost;
  if (ImGui::MenuItem("Reconnect to Agent", nullptr, false, disconnected)) reconnect();
  if (ImGui::MenuItem("Start Agent", nullptr, false, disconnected)) startAgent(agentNeedsElevation());
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
  std::string text = "Agent: " + (s.backend.empty() ? std::string("connected") : s.backend);
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

namespace {

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

}  // namespace tachyon::viewer
