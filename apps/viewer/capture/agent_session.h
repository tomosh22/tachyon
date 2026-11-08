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
#include "model/live_model.h"

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
  LiveGap,         // WP-65b: the agent dropped this client's live stream (message); the session resubscribes
  LiveEnded,       // WP-65b: live_end: the live capture is over
  LiveFailed,      // WP-65b: the live stream cannot be kept (message): a second corrupt batch, a refusal
};

// WP-65b: the state of the live feed (the live_subscribe stream of a capture started with live = true).
struct LiveFeedStatus {
  enum class State : uint8_t {
    Off,          // no live capture of this viewer
    Subscribing,  // started; waiting for the agent to be capturing so live_subscribe is accepted
    Streaming,    // batches arrive and are applied to the LiveModel
    Resyncing,    // after a gap or a corrupt batch: waiting for the keyframe of the new subscription
    Ended,        // live_end (or the connection went away)
    Failed        // gave up (error)
  } state = State::Off;
  uint64_t seq = 0;             // sequence number of the newest batch applied
  uint64_t events = 0;          // live events applied
  uint64_t bytes = 0;           // payload bytes applied
  uint32_t gaps = 0;            // live_gap events received
  uint32_t resubscribes = 0;    // live_subscribe calls after the first
  uint32_t corruptBatches = 0;  // batches LiveModel::apply refused
  std::string error;            // Failed: why
  std::chrono::steady_clock::time_point lastEventAt{};
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
  // WP-65b. Stops the live feed of this viewer: sends live_unsubscribe and ignores whatever is in flight.
  // The capture itself goes on.
  void stopLiveFeed();

  // ---- observation (thread-safe) ----
  AgentSnapshot snapshot() const;
  // Moves the queued events out (oldest first).
  std::vector<AgentEvent> drainEvents();
  // The last start request handed to start() (tests).
  control::StartRequest lastStartRequest() const;
  // True while events are queued.
  bool hasEvents() const;

  // ---- WP-65b: live view ----
  // Thread-safe. The model that `live` events are applied to (on the worker thread, which is the one writer;
  // LiveModel is internally locked, so another thread may snapshot() it meanwhile). Set it BEFORE start() of
  // a request with live = true: the session subscribes by itself once the agent is capturing, resubscribes
  // after a live_gap and once after a corrupt batch (a second one is a LiveFailed event), and ends the feed
  // at live_end. Null stops applying events.
  void setLiveModel(std::shared_ptr<model::LiveModel> model);
  // Thread-safe.
  LiveFeedStatus liveStatus() const;
  // Thread-safe. Called on the worker after a batch was applied (the live document wakes its snapshot
  // thread); must be cheap and thread-safe. Set before the first start().
  void setLiveNotify(std::function<void()> notify);

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
      Processes,
      LiveOff
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
  // Live feed (worker thread).
  bool doCall(const char* name, control::RequestBody body);
  void onLiveEvent(const control::Event& event);
  void beginLiveFeed();
  void tryLiveSubscribe();
  void resubscribeLive(const char* why);
  void failLive(const std::string& message);
  void endLiveFeed(const std::string& why);
  void pollLive();
  bool commandPending() const;
  void workerMain();
  void handleCommand(const Command& command);
  void doConnect(const Command& command);
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

  // WP-65b. m_liveModel / m_liveStatus / m_liveNotify are guarded by m_mutex; the flags below are
  // worker-only.
  std::shared_ptr<model::LiveModel> m_liveModel;
  LiveFeedStatus m_liveStatus;
  std::function<void()> m_liveNotify;
  bool m_liveWanted = false;      // a live start succeeded: keep (re)subscribing
  bool m_liveSubscribed = false;  // live_subscribe was accepted and the stream is not over
  bool m_liveIgnore = false;      // drop live events until the pending (re)subscribe is answered
  bool m_liveResync = false;      // a gap / corrupt batch: resubscribe at the next opportunity
  std::string m_liveResyncWhy;
  std::chrono::steady_clock::time_point m_lastCorruptAt{};
  std::chrono::steady_clock::time_point m_lastLiveTry{};
  int m_liveIdlePolls = 0;

  std::mutex m_linkMutex;  // guards m_activeClient (shutdown closes it from another thread)
  control::Client* m_activeClient = nullptr;

  // Worker-thread only.
  std::optional<control::Client> m_client;
  std::chrono::steady_clock::time_point m_lastPoll{};

  std::thread m_worker;
};

}  // namespace tachyon::viewer
