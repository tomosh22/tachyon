// AgentSession (WP-41): the viewer's connection to tachyon-agent. One control::Client lives on a dedicated
// worker thread (the Client is not thread-safe); the UI thread posts commands, reads a snapshot and drains an
// event mailbox. Nothing here ever blocks the caller on the socket.
//
//   Connection: Disconnected -> Connecting -> Connected -> (agent gone) Lost -> connect() again.
//   Phase (while Connected): Idle, Starting, Armed (a launch parked by start_immediately=false, waiting for
//   resume()), Capturing, Stopping (converting).
//
// The worker polls `status` about once a second while connected (the agent also pushes state events), so a
// dead agent turns into Lost within a poll interval plus the call timeout. Every command is answered by a
// CommandFailed event on failure; success is visible through the snapshot and StateChanged events.
#pragma once

#include "control/client.h"
#include "control/fetch.h"
#include "control/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::viewer {

enum class ConnectionState : uint8_t { Disconnected, Connecting, Connected, Lost };
enum class AgentPhase : uint8_t { Unknown, Idle, Starting, Armed, Capturing, Stopping };

// Thread-safe: pure functions.
const char* connectionStateName(ConnectionState state);
const char* agentPhaseName(AgentPhase phase);

struct AgentSnapshot {
  ConnectionState connection = ConnectionState::Disconnected;
  AgentPhase phase = AgentPhase::Unknown;  // Unknown unless Connected
  std::string address;
  bool remote = false;  // WP-61: address is tcp://host:port, a network connection to another machine
  std::string backend;  // as the agent reports it ("mock", "perf", "etw")
  bool elevated = false;
  double progress = 0.0;        // conversion progress while Stopping
  double elapsedSeconds = 0.0;  // since the capture started recording (Capturing); 0 otherwise
  std::string target;           // what is being captured ("launch /bin/app", "attach pid 12", "system")
  std::string lastError;        // last connection / command / agent error, "" if none
  bool commandInFlight = false;
  // Flight recorder (WP-60): circular_mb of the capture this viewer started (0: not circular, or started by
  // someone else, in which case the viewer cannot know).
  uint32_t circularMb = 0;
};

enum class AgentEventKind : uint8_t {
  Connected,       // connect() succeeded
  ConnectFailed,   // connect() did not reach an agent (message)
  ConnectionLost,  // the connection broke while Connected (message)
  StateChanged,    // phase changed
  CaptureDone,     // capture_done (summary)
  AgentError,      // unsolicited error event of the agent (message)
  CommandFailed,   // a command was refused or failed (command, code, message)
  Flushed,         // the agent wrote a flushed trace of a circular capture (flushed), whoever asked
  ProcessList,     // answer of listProcesses() (processes, or message when the request failed)
};

struct AgentEvent {
  AgentEventKind kind = AgentEventKind::StateChanged;
  AgentPhase phase = AgentPhase::Unknown;
  control::CaptureSummary summary;               // CaptureDone
  control::FlushedEvent flushed;                 // Flushed
  std::vector<control::ProcessEntry> processes;  // ProcessList
  std::string message;
  std::string command;        // CommandFailed: "start", "stop", ...
  std::string code;           // CommandFailed: protocol code ("EBUSY", ...) or "ETIMEOUT"/"EIO" for transport
  bool userInitiated = true;  // Connected / ConnectFailed: false for the silent startup attempt
};

struct AgentSessionOptions {
  int callTimeoutMs = 5000;   // per request
  int pollIntervalMs = 1000;  // status poll while connected
  int eventWaitMs = 50;       // latency of commands while connected
  // Called from the worker after something was added to the mailbox or the snapshot changed. Must be
  // thread-safe and must not outlive what it touches.
  std::function<void()> wake;
};

class AgentSession {
 public:
  using Options = AgentSessionOptions;

  // Main-thread only. The worker thread is started by the first connect().
  explicit AgentSession(Options options = {});
  // Main-thread only. Closes the connection (unblocking a request in flight) and joins the worker.
  ~AgentSession();
  AgentSession(const AgentSession&) = delete;
  AgentSession& operator=(const AgentSession&) = delete;

  // ---- commands (thread-safe, asynchronous; results arrive as snapshot changes and events) ----
  // Connects to `address`, retrying for up to `retryMs` (0 = one attempt). Replaces a current connection.
  // `userInitiated` false marks the startup attempt (no error toast). A tcp://host:port address (WP-61) needs
  // `tokenFile`: the file is read on the worker thread at each attempt, so the token itself is never kept.
  void connect(std::string address, int retryMs = 0, bool userInitiated = true, std::string tokenFile = {});
  // Closes the connection; the state becomes Disconnected.
  void disconnect();
  // Sends `start`; `targetLabel` is shown while the capture runs.
  void start(control::StartRequest request, std::string targetLabel);
  // Triggers a launch parked by start(start_immediately=false).
  void resume();
  void stop();
  void cancel();
  // Flight recorder: asks the agent to write the last `seconds` of the circular capture to <name>.tachyon.
  // The result arrives as a Flushed event (or CommandFailed).
  void flush(std::string name, uint32_t seconds, std::string reason);
  // Asks the agent for its status now.
  void refreshStatus();
  // WP-61. Asks the agent for its process list; the answer arrives as a ProcessList event (the Attach dialog
  // of a remote agent: the processes are those of the other machine).
  void listProcesses();

  // ---- observation (thread-safe) ----
  AgentSnapshot snapshot() const;
  // Moves the queued events out (oldest first).
  std::vector<AgentEvent> drainEvents();
  // The last start request handed to start() (tests).
  control::StartRequest lastStartRequest() const;
  // True while events are queued.
  bool hasEvents() const;

 private:
  struct Command {
    enum class Kind : uint8_t {
      Connect,
      Disconnect,
      Start,
      Resume,
      Stop,
      Cancel,
      Flush,
      Refresh,
      Processes
    } kind = Kind::Refresh;
    std::string address;
    std::string tokenFile;
    int retryMs = 0;
    bool userInitiated = true;
    control::StartRequest start;
    std::string label;
    control::FlushRequest flush;
  };

  void post(Command command);
  bool commandPending() const;
  void workerMain();
  void handleCommand(const Command& command);
  void doConnect(const Command& command);
  void doCall(const char* name, control::RequestBody body);
  void pollStatus();
  void applyStatus(const control::StatusBody& status);
  void applyEvent(const control::Event& event);
  void applyPhase(capture::CaptureState state);
  void setPhase(AgentPhase phase);
  void lose(const std::string& why);
  void dropClient();
  void push(AgentEvent event);
  void notify();

  Options m_options;

  mutable std::mutex m_mutex;  // guards the members below
  std::condition_variable m_cv;
  std::deque<Command> m_commands;
  std::vector<AgentEvent> m_events;
  AgentSnapshot m_snapshot;
  std::chrono::steady_clock::time_point m_captureStart{};
  control::StartRequest m_lastStart;
  bool m_expectParked = false;
  bool m_quit = false;
  std::atomic<bool> m_quitFlag{false};  // m_quit, readable without m_mutex
  bool m_workerStarted = false;

  std::mutex m_linkMutex;  // guards m_activeClient (shutdown closes it from another thread)
  control::Client* m_activeClient = nullptr;

  // Worker-thread only.
  std::optional<control::Client> m_client;
  std::chrono::steady_clock::time_point m_lastPoll{};

  std::thread m_worker;
};

}  // namespace tachyon::viewer
