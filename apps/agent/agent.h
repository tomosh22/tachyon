// Agent core: owns the capture backend and the single-capture state machine, and answers control requests.
//
// State machine (one capture at a time):
//   idle -> starting -> capturing -> converting -> done -> idle
// `done` is transient: after conversion the agent emits state(done), capture_done, then state(idle); the
// state itself is already idle when capture_done is emitted, so a status request or start sent in reaction to
// capture_done (or error) never observes `done`. A start while the state
// is not idle is rejected with EBUSY. A failed start or conversion returns to idle (conversion failures
// additionally emit an `error` event).
//
// Deferred launch (WP-13): a `start` with mode launch and start_immediately=false launches the target parked
// (suspended) and leaves the state at `starting`. Exactly then a second `start` with start_immediately=true
// is the trigger: the backend starts recording and resumes the target, the state becomes `capturing`; every
// other field of that second request is ignored (the CLI sends exe "-", which is never launched:
// with no parked launch such a request is answered ENOTFOUND "no parked launch"). `cancel` in this state
// terminates the parked process and returns to idle; `stop` is rejected with EBUSY. Only backends
// implementing capture::ITargetWatch (the ETW backend) can defer.
//
// Target exit (WP-13): for such backends the agent watches the launched or attached target of a capturing
// session and, when ITargetWatch::targetExited() turns true, finishes the capture by itself exactly as if a
// `stop` had arrived (converting, capture_done, idle). That is what lets `tachyon-cli capture` end when the
// game exits.
//
// Environment: a launched process gets TACHYON_AGENT_PIPE=<AgentConfig::agentAddress> merged over the
// request's env, so the SDK connects to this agent.
//
// Instrumentation (WP-52): while a capture is starting or capturing, an `inst_hello` from an SDK client opens
// a per-process listener (instrumentationAddress) and a spool file next to the output trace
// (`<out>.inst/inst-<pid>.tachyon-inst`); raw wire frames are appended to the spool as they arrive. When the
// capture ends the connections are drained, the backend converts as usual, and the spools are merged into the
// finished trace (inst::mergeInstrumentation: scope / fiber / frame-mark / marker / plot / thread-name
// chunks, kFlagHasInstrumentation). A `flush` merges a snapshot of the spools into the flushed trace, clipped
// to its window. With no spooled data the trace of a stop is left byte for byte as the backend wrote it (a
// flush still gets its flush_reason entry). A merge failure is logged and the spool directory is kept
// (`tachyon-cli merge` can redo it); the capture still completes. An `inst_hello` while no capture is running
// is answered ENOTFOUND: the SDK then falls back to its sidecar file and retries. Only pids that belong to
// the capture may say hello (else ENOPRIV): any pid in system mode, the attach pid in attach mode, the
// launched root pid (ITargetWatch::rootPid) in launch mode; with follow_children the descendants are not
// known through the backend interface, so any pid is accepted there (the peer pid check below still ties the
// data connection to the hello pid). Backends without ITargetWatch (the mock) accept any pid in launch mode.
// Per capture the agent caps instrumented processes, connections per process and spool bytes (AgentConfig),
// and closes data connections whose peer pid differs from the hello pid.
//
// Live view (WP-65a, docs/specs/live.md): a start with live=true gives the backend a LiveEncoder as its
// ILiveSink (and the instrumentation ingest forwards the SDK's scopes, frame marks, markers and plots to it).
// The encoder interns the raw events into live indices, cuts a batch every 250 ms (or 256 KiB) and retains
// the last AgentConfig::liveWindowSeconds of them. A client that sends `live_subscribe` gets a keyframe
// (every table definition plus the retained batches, in pieces) and then every new batch as `live` events,
// sent by a thread of its own that never blocks the others: when the client's outbound queue is full the
// thread waits, and a client that falls behind the retained window (or stalls for kLiveStallMs) gets
// `live_gap` and must resubscribe. `live_end` follows the last batch when the capture ends. `status` carries
// live / live_seq while a live capture runs. Local and authenticated remote clients alike may subscribe.
//
// Flight recorder (WP-60): a start with circular_mb > 0 is a circular capture. The backend keeps the newest
// samples; the instrumentation spools become rings (inst/spool.h) that keep the newest max(circular_mb, 64)
// MiB per process. Any number of flushes can follow; each writes `<dir of the output>/<name>.tachyon` with
// the last `seconds` of every kind of data (sampling and instrumentation clipped to the same window,
// instrumented scopes of the window balanced) and a CaptureInfo that carries flush_reason, flush_name and
// flush_window_s. `name` is made a safe file name (characters outside [A-Za-z0-9._-] become '_') and gets a
// -N suffix when the file exists. A flush of a capture that is not circular is rejected. After a flush the
// agent emits `flushed` to every client.
//   * A flush request (CLI, viewer) is synchronous: the response carries the path.
//   * A trigger (FlushRequest::trigger, sent by Tachyon_RequestFlush) is asynchronous so the engine thread is
//     never held up: the agent uses its own window (AgentConfig::flushSeconds), names the file
//     `<process>-<reason>-<YYYYMMDD-HHMMSS>`, answers ok with the planned path at once and flushes on a
//     worker thread; a failure arrives as an `error` event. At most one flush runs at a time; triggers that
//     arrive while one runs, or within flushCoalesceMs after the previous accepted trigger, are answered ok
//     with the previous path, counted, and reported as `coalesced` by the next flushed event. A trigger while
//     the capture is not circular is refused (the SDK call then returns 0) and logged once per capture.
#pragma once

#include "capture/capture_backend.h"
#include "capture/windows_etw/target_watch.h"
#include "control/live_encoder.h"
#include "control/safe_file.h"
#include "control/server.h"
#include "inst/ingest.h"
#include "inst/merge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace tachyon::agent {

struct AgentConfig {
  std::string backendName;   // reported by status
  std::string agentAddress;  // used to derive instrumentation addresses for inst_hello
  bool elevated = false;     // reported by status
  // WP-41b: owner of the per-process SDK sockets created for inst_hello (the control socket is handled by
  // listenOn). Unset: the agent's own user.
  control::SocketOwner socketOwner;

  // Instrumentation ingestion (WP-52).
  // Per-process spool cap in bytes; frames past it are dropped and reported as tachyon.dropped markers.
  uint64_t maxInstSpoolBytes = inst::kDefaultMaxSpoolBytes;
  // Directory for the spool directories; empty: next to the output trace.
  std::filesystem::path instSpoolRoot;
  // At the end of a capture each SDK connection is read until silent for instDrainIdleMs, at most
  // instDrainMaxMs.
  int instDrainIdleMs = 200;
  int instDrainMaxMs = 2000;
  // Keep the spool files after a successful merge (diagnostics).
  bool keepInstSpool = false;
  // Caps per capture: instrumented processes (further inst_hello get EBUSY), simultaneous connections per
  // process (further ones are closed), and spool bytes over all processes (further frames are dropped and
  // counted like the per-process cap).
  uint32_t maxInstProcesses = 256;
  uint32_t maxInstConnectionsPerProcess = 16;
  uint64_t maxInstTotalSpoolBytes = inst::kDefaultMaxTotalSpoolBytes;
  // Close SDK connections whose peer process id is not the pid that said hello (Linux SO_PEERCRED, Windows
  // GetNamedPipeClientProcessId).
  bool verifyInstPeerPid = true;

  // Remote clients (WP-61): clients that came in over TCP.
  // `start` with mode launch from a remote client is refused with ENOPRIV unless this is set
  // (--allow-remote-launch): it lets anyone holding the token run programs as the agent's user.
  bool allowRemoteLaunch = false;
  // Where the traces of captures requested by remote clients are written: out_path is reduced to its file
  // name inside this directory, so a remote client cannot make the agent write elsewhere. Empty: the agent's
  // working directory.
  std::filesystem::path remoteOutDir;

  // Flight recorder (WP-60).
  // Circular captures keep the newest instrumentation in a ring of segment files of max(circular_mb,
  // minRingBytes) per process (the total cap above still applies). ringSegmentBytes 0: ring / 8.
  uint64_t minRingBytes = inst::kMinRingBytes;
  uint64_t ringSegmentBytes = 0;
  // Window of a flush requested by the SDK (Tachyon_RequestFlush), in seconds of trace.
  uint32_t flushSeconds = 10;
  // SDK triggers that arrive within this long after the previous accepted one are coalesced into it.
  int flushCoalesceMs = 2000;
  // A trigger flush waits this long before it looks at the capture, so that what the requester sent just
  // before asking has been read from its connection.
  int flushSettleMs = 100;

  // Live view (WP-65a).
  // Seconds of batches the encoder retains for late subscribers (--live-window-seconds).
  uint32_t liveWindowSeconds = 30;
  // Simultaneous live subscribers; further `live_subscribe` get EBUSY.
  uint32_t maxLiveSubscribers = 16;
  // A subscriber whose outbound queue stays full this long gets live_gap.
  int liveStallMs = 5000;
};

// Thread-safe. A warning (empty when there is none) about the directory remote traces are written to: a
// directory other users can write to without the sticky bit lets them delete or rename files the agent
// created. Output files are reserved with O_EXCL|O_NOFOLLOW and written through O_NOFOLLOW, so a planted
// link is never written through; a swapped file is caught by the identity check. POSIX only.
std::string remoteOutDirWarning(const std::filesystem::path& dir);
// Thread-safe. A reason to refuse --listen (empty when there is none): the directory is owned by another
// user, who could replace the traces the agent writes there. POSIX only.
std::string remoteOutDirProblem(const std::filesystem::path& dir);

class Agent final : public control::IRequestHandler {
 public:
  using EventSink = std::function<void(const control::Event&)>;

  // Thread-safe.
  Agent(std::unique_ptr<capture::ICaptureBackend> backend, AgentConfig config);
  // Thread-safe: calls shutdown().
  ~Agent() override;

  // Not thread-safe: call before serving requests. Events are emitted from request and worker threads.
  void setEventSink(EventSink sink);

  // Thread-safe.
  control::Response handle(control::ClientId client, const control::Request& request) override;
  // Thread-safe (WP-61). Remember which clients are remote so their requests can be restricted.
  void onClientAccepted(control::ClientId client, const control::ClientInfo& info) override;
  void onDisconnect(control::ClientId client) override;
  // Thread-safe (WP-65a). Live subscriptions start after their response is queued and use the server to send.
  void afterResponse(control::ClientId client, const control::Request& request,
                     const control::Response& response) override;
  void onServerStarted(control::Server& server) override;
  void onServerStopping() override;

  // Thread-safe, idempotent. Stops a running capture (discarding nothing: the result is converted and
  // reported like a normal stop) and joins the conversion worker. Stop the server first.
  void shutdown();

  // Thread-safe.
  capture::CaptureState state() const { return m_state.load(); }
  // Thread-safe. How many whole-file SHA-256 computations fetch requests have caused (a cache hit adds none).
  uint64_t hashComputations() const { return m_hashComputations.load(); }

  // Thread-safe. Records `path` (a trace this agent produced) as fetchable. The path is lstat'ed, not
  // canonicalised: a symlink is refused and, when `reserved` is set, the file must have that identity.
  void noteProduced(const std::filesystem::path& path,
                    const std::optional<control::FileIdentity>& reserved = {});
  // Thread-safe. True when noteProduced accepted `path` (the final component is not resolved).
  bool isProduced(const std::filesystem::path& path);

 private:
  control::Response handleStatus(std::optional<int64_t> id);
  control::Response handleStart(const control::StartRequest& req, bool remote);
  control::Response handleFetch(const control::FetchRequest& req);
  control::Response handleProcesses(const control::ProcessesRequest& req);
  control::Response handleLiveSubscribe(control::ClientId client, const control::LiveSubscribeRequest& req);
  // Stops and joins the live subscription of `client`, if any.
  void stopLiveSubscriber(control::ClientId client);
  void stopAllLiveSubscribers();
  // Ends the running live capture's encoder (the last batch is cut, subscribers get live_end).
  void endLive();
  bool isRemoteClient(control::ClientId client) const;
  control::Response handleStop();
  control::Response handleFlush(const control::FlushRequest& req);
  struct FlushJob {
    std::string name;    // final file stem
    std::string reason;  // for the flushed event and the trace
    uint32_t seconds = 0;
    bool trigger = false;
    uint32_t coalesced = 0;
    std::optional<control::FileIdentity> reserved;  // the file reserved for the flushed trace
    std::filesystem::path dir;                      // where it is
  };
  // Runs one flush (backend flush, instrumentation merge, flushed event). m_flushing is held by the caller.
  Result<control::CaptureSummary> performFlush(const FlushJob& job);
  // Clears m_flushing and wakes the waiters.
  void endFlush();
  // A file stem for `requested` in the output directory of the session: the file <stem>.tachyon is created
  // empty (O_EXCL, never through a symlink) so the name is ours and nobody can plant a link there before
  // the backend writes it; a taken name gets "-N". `identity` is the reserved file. Fails with IoError.
  Result<std::string> reserveFlushName(const std::string& requested, control::FileIdentity& identity);
  control::Response handleCancel();
  control::Response handleInstHello(const control::InstHelloRequest& req);

  // Locked: Capturing -> Converting and starts the conversion worker.
  void beginConversionLocked();
  // True while a deferred launch waits for its trigger (guarded by m_mutex).
  bool armedLocked() const;
  void watcherLoop();
  void setStateLocked(capture::CaptureState state, double progress);
  void emit(const control::Event& event);
  // Locked: state -> idle, then emits `finalEvent` and state(idle) (see the definition for the ordering).
  void finishCaptureLocked(const control::Event& finalEvent);
  void conversionWorker();

  // The instrumentation ingest of the running capture, or null.
  std::shared_ptr<inst::Ingest> currentIngest();
  // Detaches the session's ingest (null when none) and reports whether it is a circular capture.
  std::shared_ptr<inst::Ingest> takeIngest(bool& circular);
  // Stops the ingest, runs the backend's stop() and merges the spools into the result. Never holds m_mutex.
  // `stillReserved` is set to the reserved identity of the trace when nothing replaced the file after the
  // check (no instrumentation merge), so noteProduced can compare it.
  Result<capture::CaptureResult> stopAndMerge(std::vector<std::string>& warnings,
                                              std::optional<control::FileIdentity>& stillReserved);
  // True when inst_hello from `pid` belongs to the running capture (see the class comment).
  bool instPidAllowedLocked(uint32_t pid) const;
  // Merges the spools into the trace of `result`; logs problems instead of failing the capture.
  // Returns the problems worth telling the client (they end up in CaptureSummary::warnings).
  std::vector<std::string> mergeSpools(inst::Ingest& ingest, const std::vector<inst::SpoolInfo>& spools,
                                       const capture::CaptureResult& result, bool clip, bool removeWhenDone,
                                       const std::string& captureInfoPatch = {});

  std::unique_ptr<capture::ICaptureBackend> m_backend;
  AgentConfig m_config;
  EventSink m_sink;

  // Serialises state transitions and the events they emit. Never held across a backend call (start, stop,
  // flush, checkPrivileges): the backend's progress callback takes it, and a slow backend must not stall
  // status requests or other clients.
  mutable std::mutex m_remoteMutex;  // guards m_remoteClients
  std::set<control::ClientId> m_remoteClients;
  std::mutex m_producedMutex;  // guards m_produced and m_hashCache
  // Canonical paths of traces produced by this agent process, with the identity of the file when it was
  // produced: a fetch serves the file it opened only when that is still this file.
  std::map<std::filesystem::path, control::FileIdentity> m_produced;
  struct CachedHash {
    uint64_t size = 0;
    int64_t mtimeNs = 0;
    std::string sha256;
  };
  std::atomic<uint64_t> m_hashComputations{0};
  std::map<std::filesystem::path, CachedHash> m_hashCache;  // whole-file SHA-256 per (path, size, mtime)

  std::mutex m_mutex;
  std::condition_variable m_flushDone;             // signalled when m_flushing drops
  bool m_flushing = false;                         // a backend->flush() is in flight (guarded by m_mutex)
  const capture::ITargetWatch* m_watch = nullptr;  // backend capability, null when absent
  bool m_resuming = false;   // a deferred launch is being triggered or cancelled (guarded by m_mutex)
  bool m_watchStop = false;  // guarded by m_mutex
  std::condition_variable m_watchCv;
  std::thread m_watcher;
  std::atomic<capture::CaptureState> m_state{capture::CaptureState::Idle};
  std::atomic<double> m_progress{0.0};
  std::thread m_worker;

  std::thread m_flushWorker;                              // trigger flushes; guarded by m_mutex
  std::filesystem::path m_sessionOutDir;                  // guarded by m_mutex: where flushes are written
  std::string m_sessionExeStem;                           // launch target's file name without extension
  bool m_loggedNotCircular = false;                       // guarded by m_mutex
  bool m_haveTrigger = false;                             // guarded by m_mutex
  std::chrono::steady_clock::time_point m_lastTrigger{};  // guarded by m_mutex
  std::string m_lastFlushPath;  // guarded by m_mutex: answer of coalesced triggers (flush in flight or last)
  std::optional<control::FileIdentity> m_outIdentity;  // reserved trace of a remote capture; m_mutex
  std::filesystem::path m_outReservedPath;             // where it is; m_mutex
  uint32_t m_coalesced = 0;                            // guarded by m_mutex: since the last flushed event
  std::shared_ptr<inst::Ingest> m_ingest;              // guarded by m_mutex
  std::shared_ptr<control::LiveEncoder> m_live;  // the live capture's encoder (or the last one); m_mutex

  // Live subscribers (WP-65a).
  struct LiveSubscriber {
    control::ClientId client = 0;
    std::shared_ptr<control::LiveEncoder> encoder;
    std::optional<control::LiveKeyframe> keyframe;  // to send first (absent: a continuation from `next`)
    uint64_t next = 0;                              // the next batch to send
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};  // the thread is done: the slot may be reused
    std::thread thread;
  };
  void liveSubscriberLoop(LiveSubscriber& sub);
  void liveSubscriberBody(LiveSubscriber& sub);
  // Joins and forgets the subscribers whose thread has ended, so their slots count no more.
  void reapFinishedSubscribers();
  // Queues one line for the subscriber, waiting while its queue is full. False: stop (gone, stopped,
  // stalled); `stalled` says it was the stall limit.
  bool sendLiveLine(LiveSubscriber& sub, std::string line, bool& stalled);
  std::mutex m_subMutex;  // guards m_subscribers and m_pendingSubs
  std::map<control::ClientId, std::unique_ptr<LiveSubscriber>> m_subscribers;
  std::map<control::ClientId, std::unique_ptr<LiveSubscriber>> m_pendingSubs;  // responded to, not started
  std::mutex m_serverMutex;                                                    // guards m_server
  control::Server* m_server = nullptr;  // set between onServerStarted and onServerStopping
  bool m_circular = false;              // guarded by m_mutex: the running capture is a flight recorder
  capture::CaptureMode m_sessionMode = capture::CaptureMode::System;  // guarded by m_mutex
  uint32_t m_sessionPid = 0;                                          // attach target; guarded by m_mutex
  bool m_sessionFollow = false;                                       // follow_children; guarded by m_mutex
};

}  // namespace tachyon::agent
