// CaptureController (WP-41): the capture side of the viewer. Owns the AgentSession, the Run / Attach / System
// dialogs, the process list, agent auto-launch and the live capture state (menu, status bar segment, Capture
// panel). Everything here is main-thread only; the agent traffic runs on AgentSession's worker thread.
#pragma once

#include "viewer/capture/agent_session.h"
#include "viewer/capture/capture_form.h"
#include "viewer/capture/process_launcher.h"
#include "viewer/capture/process_list.h"
#include "viewer/panel.h"
#include "viewer/settings.h"
#include "viewer/status/path_picker.h"
#include "viewer/status/toast.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::viewer {

class JobQueue;

// What the controller asks of the application (all called on the main thread, except `wake`).
struct CaptureHost {
  Settings* settings = nullptr;
  JobQueue* jobs = nullptr;  // null: the process list is read synchronously
  std::function<void(ToastLevel, std::string)> toast;
  std::function<void(const std::filesystem::path&)> openFile;
  std::function<void()> wake;  // thread-safe: wake the event loop
  std::function<void()> requestRedraw;
  std::function<void()> saveSettings;
  std::function<void(const char*)> showPanel;  // open a panel by name
  std::function<void()> capturesChanged;       // a capture finished (refresh the Session Explorer)
};

// What starting the agent would run (see planAgentLaunch).
struct AgentLaunchPlan {
  LaunchSpec spec;
  std::string backend;
  bool backendNeedsPrivileges = false;  // everything but "mock"
};

// Thread-safe: pure function. The agent command line for the settings: backend (setting, else etw on Windows
// and perf on Linux), executable (setting, else tachyon-agent next to `viewerExe`), the address and the extra
// agent arguments. `elevate` fills LaunchSpec::elevate.
AgentLaunchPlan planAgentLaunch(const CaptureSettings& settings, const std::filesystem::path& viewerExe,
                                const std::string& address, bool elevate);
// Thread-safe: pure function. "m:ss.s" / "h:mm:ss".
std::string formatElapsed(double seconds);

class CaptureController {
 public:
  // Main-thread only. `launcher` null = the system launcher.
  CaptureController(CaptureHost host, std::shared_ptr<IProcessLauncher> launcher);
  // Main-thread only. Joins the agent worker.
  ~CaptureController();
  CaptureController(const CaptureController&) = delete;
  CaptureController& operator=(const CaptureController&) = delete;

  // ---- agent connection ----
  // The address from the settings (default control::defaultAgentAddress()).
  std::string agentAddress() const;
  // Silent connection attempt at startup (does nothing unless Settings::capture.autoConnect).
  void connectAtStartup();
  // Connects to the configured address (explicit, reports failure).
  void reconnect();
  // Starts the agent through the launcher and connects to it. `elevate`: pkexec / UAC.
  void startAgent(bool elevate);
  // True if the agent for the current settings needs privileges this process does not have.
  bool agentNeedsElevation() const;
  void disconnectAgent();

  // ---- capture commands (no-ops with a toast when they do not apply) ----
  // Opens the Run / Attach / System dialog.
  void openDialog(capture::CaptureMode mode);
  void closeDialog() { m_dialogOpen = false; }
  void resumeCapture();
  void stopCapture();
  void cancelCapture();

  // ---- frame hooks ----
  // Delivers the agent's events (toasts, auto-open, panel focus). Returns true if anything was delivered.
  bool pump();
  // True while the loop should keep drawing periodically (connecting, armed, capturing, stopping).
  bool active() const;
  // True when a mailbox entry or an unsynchronised snapshot change waits for pump().
  bool hasPending() const;
  // Draws the dialogs and file pickers. Call once per frame outside any window.
  void draw();
  // Draws the "Capture" menu; call inside the main menu bar.
  void drawMenu();
  // Draws the capture segment of the status bar into the current menu bar.
  void drawStatusSegment();
  // Draws the contents of the Capture panel (between Begin/End).
  void drawPanelContents();
  // The text of the status bar segment ("Agent: not connected", "REC 0:12.3 launch /bin/app", ...).
  std::string statusText() const;

  // ---- state (tests) ----
  AgentSession& session() { return m_session; }
  const AgentSession& session() const { return m_session; }
  bool dialogOpen() const { return m_dialogOpen; }
  capture::CaptureMode dialogMode() const { return m_form.mode; }
  CaptureForm& form() { return m_form; }
  const std::vector<FormError>& formErrors() const { return m_errors; }
  // Validates the form and, when it is clean and the agent is connected, sends `start` and closes the dialog.
  // Returns true if the request was sent. Errors land in formErrors() and are drawn under the fields.
  bool submitDialog();
  // The processes of the Attach dialog (all of them; filteredProcesses() applies the filter).
  const std::vector<ProcessInfo>& processes() const { return m_processes; }
  std::vector<size_t> filteredProcesses() const { return filterProcesses(m_processes, m_processFilter); }
  bool processesLoading() const { return m_processJob != 0; }
  const std::string& processError() const { return m_processError; }
  std::string& processFilter() { return m_processFilter; }
  // Starts the (asynchronous) enumeration for the Attach dialog.
  void refreshProcesses();
  // Chooses the process of the Attach dialog.
  void selectProcess(uint32_t pid);
  IProcessLauncher& launcher() { return *m_launcher; }
  std::shared_ptr<IProcessLauncher> sharedLauncher() const { return m_launcher; }
  // The last StartRequest the dialog sent.
  const control::StartRequest& lastRequest() const { return m_lastRequest; }

 private:
  enum class PickTarget : uint8_t { None, Exe, Cwd, Out };

  void handleEvent(const AgentEvent& event);
  void onCaptureDone(const AgentEvent& event);
  void drawDialog();
  void drawAgentControls();
  void drawProcessTable();
  void drawAgentSettings();
  void fieldError(const char* field);
  void revalidate();
  void toast(ToastLevel level, std::string text);
  void connectNow(int retryMs, bool userInitiated);

  CaptureHost m_host;
  std::shared_ptr<IProcessLauncher> m_launcher;
  AgentSession m_session;

  bool m_dialogOpen = false;
  bool m_dialogJustOpened = false;
  CaptureForm m_form;
  std::vector<FormError> m_errors;
  bool m_showErrors = false;
  bool m_formDirty = true;
  CaptureForm m_validatedForm;  // what m_errors was computed for
  int m_lastValidateFrame = -1000;
  control::StartRequest m_lastRequest;

  PathPicker m_picker;
  PickTarget m_pickTarget = PickTarget::None;

  std::vector<ProcessInfo> m_processes;
  std::string m_processFilter;
  std::string m_processError;
  uint64_t m_processJob = 0;
  uint32_t m_selectedPid = 0;

  AgentPhase m_lastPhase = AgentPhase::Unknown;
};

// Thread-safe: factory. The "Capture" panel: agent connection, live capture state, Stop / Cancel / Resume.
std::unique_ptr<IPanel> makeCapturePanel(CaptureController& controller);

inline constexpr const char* kCapturePanelName = "Capture";

}  // namespace tachyon::viewer
