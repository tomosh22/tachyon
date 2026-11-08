// LiveSession (WP-65b): the viewer-side machinery of one live capture.
//
//   agent --live events--> AgentSession worker --apply()--> LiveModel          (batches, any time)
//                                                              |
//                                              snapshot worker | every >= refreshMs, when the generation
//                                              moved
//                                                              v
//        TraceModel snapshot  +  thread states  +  timeline index (LOD)  +  symbol service
//                                                              |  mailbox (newest wins)
//                                                              v
//                                      App::pumpLive() on the UI thread: take() -> Document -> swap
//
// Everything expensive happens on ONE background thread (the snapshot worker): LiveModel::snapshot() (about
// 40 ms for the 30 s x 8 kHz x 16 threads window), analysis::buildThreadStates, the timeline's LOD build and
// the symbol epoch bookkeeping (LiveSymbols). The UI thread only takes the finished bundle out of a one-slot
// mailbox and swaps the document, so its per-frame cost does not depend on the window size. Pause stops the
// worker from producing and the UI from taking (the LiveModel keeps receiving); Resume continues with the
// newest data.
#pragma once

#include "model/live_model.h"
#include "symbols/symbol_service.h"
#include "viewer/document.h"
#include "viewer/live/live_symbols.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace tachyon::viewer {

// What the snapshot worker hands to the UI thread.
struct LivePublished {
  std::shared_ptr<const model::TraceModel> model;  // thread states are built
  std::shared_ptr<timeline::LodShared> lod;        // finished timeline index of `model` (null: not built)
  std::shared_ptr<symbols::SymbolService> symbols;
  LiveSnapshotInfo info;
  uint64_t serial = 0;  // 1, 2, ... per published bundle
  double snapshotMs = 0, statesMs = 0, lodMs = 0, symbolsMs = 0;
  std::chrono::steady_clock::time_point publishedAt;
};

// Thread-safe: pure function on `model` (which must not be shared yet). WP-65b/c: drops, from a LiveModel
// snapshot, the threads that have no sample in the window, are not a thread of the target and carry no scope
// event, frame mark or marker. LiveModel keeps every thread it has ever heard of (system-wide scheduling
// events name every thread of the machine, a busy machine runs hundreds of short-lived ones, and a viewer
// that profiles the machine it runs on keeps creating threads of its own); each costs the timeline index
// about a megabyte at the 30 s window whether or not it has samples, so unpruned the index (and the build
// time) would be set by the machine's noise rather than by the program. A thread that only switched is not
// a row worth drawing; the target's threads stay even when they are blocked all window. The threads that
// remain keep their relative order; ThreadIdx values after the first dropped thread shift down, which
// the viewer maps by (pid, tid) as for any model replacement (threadMapBetween). Returns the number of
// threads dropped.
size_t pruneIdleThreads(model::TraceModel& model);

struct LiveSessionOptions {
  // Snapshots start at least this far apart (and at least 100 ms after the previous one finished).
  uint32_t refreshMs = 500;
  uint64_t windowNs = 30ull * 1'000'000'000ull;
  // Called from the worker when a bundle was published. Thread-safe; must not outlive what it touches.
  std::function<void()> wake;
  bool buildTimelineIndex = true;
  uint64_t lodMaxBytes = 0;  // LodOptions::maxBytes (memory ceiling), 0 = none
  // Thread-state and LOD build workers. 1 (the default) builds on the snapshot worker itself: a profiler that
  // takes every core from the program it profiles disturbs what it measures, and workers created per build
  // are new threads (new thread ids) every 500 ms, which a system-wide capture then lists as threads. 0 = all
  // cores.
  unsigned buildThreads = 1;
  // Test hook: called on the worker thread before each snapshot starts (e.g. to hold the worker).
  std::function<void()> beforeSnapshot;
};

struct LiveSessionStats {
  uint64_t published = 0;
  uint64_t taken = 0;
  double lastSnapshotMs = 0, lastStatesMs = 0, lastLodMs = 0, lastSymbolsMs = 0, lastTotalMs = 0;
  double maxTotalMs = 0;
  LiveSymbolStats symbols;
};

class LiveSession {
 public:
  // Main-thread only.
  explicit LiveSession(LiveSessionOptions options);
  // Main-thread only. Stops and joins the worker.
  ~LiveSession();
  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;

  // Main-thread only. Starts a new live capture: a fresh LiveModel (the previous one, if any, is dropped and
  // its worker joined) and a worker for it, resolving symbols with `symbols`. The returned model is what the
  // AgentSession applies batches to.
  std::shared_ptr<model::LiveModel> begin(LiveSymbolsConfig symbols);
  // Main-thread only. Stops the worker and drops the model and the mailbox (the document on screen stays).
  void close();

  // Thread-safe. A batch was applied: the worker looks at the model's generation.
  void notify();
  // Thread-safe. The feed is over (live_end, connection lost, failure): the worker takes one last snapshot
  // (unless paused: then the frozen view stays until Resume) and goes idle.
  void markEnded();

  // Main-thread only. The newest bundle the UI has not taken yet; null when there is none or while paused.
  std::shared_ptr<LivePublished> take();
  // Main-thread only. Pause freezes what take() delivers; the model keeps receiving.
  void setPaused(bool paused);
  bool paused() const { return m_paused.load(); }
  // Thread-safe. Between begin() and close().
  bool active() const { return m_active.load(); }
  // Thread-safe. A bundle waits in the mailbox and the UI may take it (false while paused).
  bool hasPublished() const;
  // Thread-safe. markEnded() was called for this capture.
  bool ended() const { return m_ended.load(); }
  // Thread-safe. The model of the capture, or null.
  std::shared_ptr<model::LiveModel> model() const;

  // Main-thread only. Replaces the symbol configuration (new symbol path, Retry): the worker resolves from
  // scratch at its next snapshot.
  void reconfigureSymbols(LiveSymbolsConfig config);

  // Thread-safe.
  LiveSessionStats stats() const;
  uint64_t streamId() const { return m_streamId; }
  void setStreamId(uint64_t id) { m_streamId = id; }

  // Thread-safe. The local clock in the trace clock of `model` (ticks), when this machine's clock is the one
  // the trace was captured with (Linux CLOCK_MONOTONIC_RAW nanoseconds, Windows QPC); nullopt otherwise. The
  // latency of a displayed sample is this minus its timestamp.
  static std::optional<uint64_t> localTraceTicks(const model::TraceModel& model);

 private:
  void workerMain(std::shared_ptr<model::LiveModel> model, std::shared_ptr<LiveSymbols> symbols);
  void stopWorker();

  LiveSessionOptions m_options;
  uint64_t m_streamId = 0;

  mutable std::mutex m_mutex;  // guards the members below
  std::condition_variable m_cv;
  std::shared_ptr<model::LiveModel> m_model;
  std::shared_ptr<LiveSymbols> m_symbols;
  std::shared_ptr<LivePublished> m_mailbox;
  std::optional<LiveSymbolsConfig> m_pendingSymbols;
  LiveSessionStats m_stats;
  bool m_stop = false;

  std::atomic<bool> m_active{false};
  std::atomic<bool> m_ended{false};
  std::atomic<bool> m_paused{false};
  std::atomic<bool> m_cancel{false};  // aborts a LOD build in progress
  std::thread m_worker;
};

}  // namespace tachyon::viewer
