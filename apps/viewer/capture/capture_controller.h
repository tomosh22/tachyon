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

#include <chrono>
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
  // WP-67: the free-text warnings of a finished capture's `capture_done`, for the trace at `path` (shown by
  // the warnings banner when that trace is open). Called before openFile.
  std::function<void(const std::filesystem::path&, std::vector<std::string>)> captureWarnings;
  // WP-65b (live view). Called right before a start request with live = true is sent: the host prepares its
  // Live document and returns the model the AgentSession applies the agent's `live` events to.
  std::function<std::shared_ptr<model::LiveModel>(const control::StartRequest&)> liveBegin;
  // WP-65b. Every agent event, before the controller handles it (the host reacts to LiveGap / LiveEnded /
  // LiveFailed, to a refused start, a lost connection and capture_done). Main thread.
  std::function<void(const AgentEvent&)> liveEvent;
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

// One trace written by a flush of the circular capture (a `flushed` event of the agent).
struct FlushedCapture {
  std::string name;
  std::string reason;
  // The trace on this machine. With a remote agent it is the path on the agent until the background
  // transfer has finished (`fetched`), then the local copy.
  std::string path;
  std::string remotePath;  // WP-61: as the agent reported it ("" for a local agent)
  bool fetched = true;     // false while a remote trace has not arrived yet
  double durationS = 0.0;
  uint64_t eventsLost = 0;
  uint32_t coalesced = 0;                    // triggers folded into this flush
  std::chrono::system_clock::time_point at;  // when the viewer heard of it
};

// WP-61: a trace being copied from a remote agent into the captures directory (a `capture_done` or
// `flushed` event of a tcp:// agent). The data goes to `<localPath>.part`; Retry after a failure or a cancel
// resumes from there.
struct RemoteTransfer {
  enum class State : uint8_t { Running, Done, Failed, Cancelled };
  std::string remotePath;
  std::filesystem::path localPath;
  State state = State::Running;
  std::string error;
  uint64_t jobId = 0;         // JobQueue job while Running (0 when run inline)
  bool openWhenDone = false;  // open the document once it has arrived (autoOpen of a finished capture)
  bool isCapture = true;      // capture_done (false: a flush of the flight recorder)
  uint64_t bytes = 0;         // size when Done
  std::vector<std::string> warnings;  // WP-67: capture_done warnings, handed to the banner on arrival
};

// Result of "Test connection" in the Connect to remote agent dialog.
struct RemoteProbe {
  bool done = false;
  bool ok = false;
  std::string message;
};

// Thread-safe: no shared state. Connects to `hostPort` ("host:port") with the token in `tokenFile`, asks for
// the status and disconnects. The message says what was found ("connected: perf backend, idle, elevated") or
// why not. Blocks for the connect timeout at most: call it off the UI thread.
RemoteProbe probeRemoteAgent(const std::string& hostPort, const std::string& tokenFile);

// What the Flight Recorder panel shows.
struct FlightRecorderState {
  bool connected = false;
  bool capturing = false;
  bool armed = false;            // capturing and known to be circular: a flush will work
  bool circularUnknown = false;  // capturing, but started by someone else: the viewer cannot tell
  uint32_t ringMb = 0;           // circular_mb of the capture the viewer started
  double elapsedSeconds = 0.0;
  std::string target;
  std::string phase;
};

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

  // ---- remote agent (WP-61) ----
  // Connects to the agent at `hostPort` ("host:port", IPv6 in brackets) with the token read from `tokenFile`;
  // both are remembered in the settings (the path, never the token). False (with a toast) when the address
  // is malformed.
  bool connectRemote(const std::string& hostPort, const std::string& tokenFile);
  // Back to the agent on this machine (the configured address).
  void useLocalAgent();
  // True while connected (or connecting) to a tcp:// agent.
  bool remoteActive() const;
  // The "Connect to remote agent" dialog.
  void openRemoteDialog();
  void closeRemoteDialog() { m_remoteDialogOpen = false; }
  bool remoteDialogOpen() const { return m_remoteDialogOpen; }
  std::string& remoteAddressField() { return m_remoteAddressText; }
  std::string& remoteTokenFileField() { return m_remoteTokenText; }
  // Runs probeRemoteAgent for the dialog's fields in the background (inline without a JobQueue).
  void testRemote();
  const RemoteProbe& remoteProbe() const { return m_remoteProbe; }
  // Transfers of this run, oldest first (at most 100 are kept).
  const std::vector<RemoteTransfer>& transfers() const { return m_transfers; }
  // Cancels a running transfer (the .part file stays). Retry resumes it.
  void cancelTransfer(size_t index);
  void retryTransfer(size_t index);
  // Copies the trace `remotePath` of the connected remote agent into the captures directory.
  void fetchRemote(const std::string& remotePath, bool openWhenDone, bool isCapture,
                   std::vector<std::string> warnings = {});
  // True if the agent for the current settings needs privileges this process does not have.
  bool agentNeedsElevation() const;
  void disconnectAgent();

  // ---- capture commands (no-ops with a toast when they do not apply) ----
  // Opens the Run / Attach / System dialog. WP-65b: `live` makes it the Live dialog (the same fields plus a
  // mode choice; the capture starts with live:true).
  void openDialog(capture::CaptureMode mode, bool live = false);
  void closeDialog() { m_dialogOpen = false; }
  void resumeCapture();
  void stopCapture();
  // WP-65b. Stops the capture and opens its trace when it is done, whatever Settings::capture.autoOpen says
  // (the Live window's Stop).
  void stopCaptureAndOpen();
  void cancelCapture();

  // ---- flight recorder (WP-60) ----
  FlightRecorderState flightRecorderState() const;
  // Flushed traces of this run of the viewer, oldest first (at most 200).
  const std::vector<FlushedCapture>& flushedCaptures() const { return m_flushed; }
  // The "seconds" field of the panel's Flush now button.
  int& flushSeconds() { return m_flushSeconds; }
  // Asks the agent for a flush of the last flushSeconds() seconds. False (with a toast) when the capture is
  // not a flight recorder or there is none.
  bool flushNow();
  // WP-65b. Asks the agent to write the last `seconds` seconds of the circular capture ("Save last 30 s" of
  // the Live window). Same rules as flushNow().
  bool flushSeconds(int seconds);
  // Opens the flushed trace #index in the viewer.
  void openFlushed(size_t index);
  // Draws the contents of the Flight Recorder panel (between Begin/End).
  void drawFlightRecorderContents();

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
  bool processesLoading() const { return m_processJob != 0 || m_remoteProcessesPending; }
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
  void finishLocalCapture(const std::filesystem::path& path, bool open,
                          std::vector<std::string> warnings = {});
  void startTransfer(size_t index);
  void onTransferDone(size_t index, bool ok, const std::string& error, uint64_t bytes);
  void drawRemoteDialog();
  void drawTransfers();

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

  // WP-61
  bool m_connectedRemote = false;  // the last connect was to a remote agent (Reconnect goes there again)
  bool m_remoteDialogOpen = false;
  bool m_remoteDialogJustOpened = false;
  std::string m_remoteAddressText, m_remoteTokenText;
  RemoteProbe m_remoteProbe;
  uint64_t m_probeJob = 0;
  std::vector<RemoteTransfer> m_transfers;
  bool m_remoteProcessesPending = false;

  bool m_openStale =
      false;  // WP-65c: the agent went idle: an armed "open the trace" that is still unused is void
  bool m_openNextCapture = false;  // WP-65b: the next capture_done opens its trace regardless of autoOpen
  std::vector<FlushedCapture> m_flushed;
  int m_flushSeconds = 10;
  uint32_t m_flushSerial = 0;
};

// Thread-safe: factory. The "Capture" panel: agent connection, live capture state, Stop / Cancel / Resume.
std::unique_ptr<IPanel> makeCapturePanel(CaptureController& controller);

// Thread-safe: factory. The "Flight Recorder" panel: is the circular capture armed, Flush now, and the list
// of flushed traces.
std::unique_ptr<IPanel> makeFlightRecorderPanel(CaptureController& controller);

inline constexpr const char* kCapturePanelName = "Capture";
inline constexpr const char* kFlightRecorderPanelName = "Flight Recorder";

}  // namespace tachyon::viewer
