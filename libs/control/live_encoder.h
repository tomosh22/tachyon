// LiveEncoder (WP-65a, docs/specs/live.md section 2): turns the raw-identity events of a live capture
// (capture::ILiveSink) into batches of records with live indices, keeps the last `windowSeconds` of batches
// and the table definitions for late subscribers, and serves them to subscribers (the agent's `live_subscribe`).
//
// INTERNING. Strings, processes, threads, modules, frames (module + RVA, or an absolute address) and callstack
// nodes (a prefix tree, node 0 = root) get dense live indices in order of first use. All tables are capped
// (live_wire::kMax*): an identity that does not fit maps to an overflow entry (string 1 "<overflow>", one
// overflow thread, one overflow frame, a truncated stack) and is counted in LiveEncoderStats::overflows.
// Samples repeat the same callchains, so whole-stack resolutions are cached (direct mapped, validated against
// the module epoch of the process tables).
//
// BATCHES. Events accumulate and are cut into a batch every `batchMs` (a timer thread) or when about
// `batchBytes` are pending. A batch is a string of records (see model/live_model.h for the encoding):
// definitions created since the previous batch first, then the events, then TimeAdvance / Dropped. Batch
// sequence numbers start at 1. A heartbeat batch (TimeAdvance only) is cut whenever the capture's clock moved
// but nothing else happened, so a quiet capture still scrolls.
//
// RETENTION. Batches whose newest timestamp is older than `now - window` are discarded, and the retained
// bytes are capped (oldest first). A KEYFRAME (beginKeyframe) is every table definition re-encoded from the
// tables as they are now, followed by the event part of every retained batch; it is delivered as several
// pieces of at most live_wire::kMaxEventBytes each.
//
// All public methods are thread-safe (one mutex); the ILiveSink methods may be called from the backend's
// thread and from instrumentation ingest threads.
#pragma once

#include "capture/capture_backend.h"
#include "model/live_model.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tachyon::control {

struct LiveEncoderConfig {
  uint32_t windowSeconds = 30;
  int batchMs = 250;
  size_t batchBytes = 256 * 1024;
  // Retained batch bytes over all batches; the oldest are dropped beyond it.
  uint64_t maxRetainedBytes = 1ull << 30;
  // Run the timer thread that cuts batches every batchMs. Tests that drive flush() themselves turn it off.
  bool autoTick = true;
  // Table caps (tests lower them).
  uint32_t maxStrings = model::live_wire::kMaxStrings;
  uint32_t maxProcesses = model::live_wire::kMaxProcesses;
  uint32_t maxThreads = model::live_wire::kMaxThreads;
  uint32_t maxModules = model::live_wire::kMaxModules;
  uint32_t maxFrames = model::live_wire::kMaxFrames;
  uint32_t maxNodes = model::live_wire::kMaxNodes;
};

struct LiveEncoderStats {
  uint64_t lastSeq = 0;
  uint64_t batches = 0;
  uint64_t retainedBatches = 0;
  uint64_t retainedBytes = 0;
  uint64_t samples = 0;
  uint64_t droppedLive = 0;  // reported by the source (onDropped)
  uint64_t overflows = 0;    // identities that did not fit a capped table
  uint64_t strings = 0, processes = 0, threads = 0, modules = 0, frames = 0, nodes = 0;
  uint64_t nowTs = 0;
};

// A batch as delivered to subscribers. `data` is the whole wire batch (definitions then events); the events
// alone start at data[defsBytes].
struct LiveBatch {
  uint64_t seq = 0;
  std::shared_ptr<const std::string> data;
  size_t defsBytes = 0;
  uint64_t endTs = 0;  // newest timestamp of the capture when the batch was cut
};

enum class LiveWait {
  Ready,    // `out` holds the requested batch
  Timeout,  // not produced yet
  Gap,      // the requested batch is no longer retained
  Ended,    // the capture ended and the requested batch will never exist
};

// Splits `blob` (a sequence of wire records) into pieces of at most `maxBytes`, cutting only at record
// boundaries (a single record larger than maxBytes is one piece). Thread-safe: pure function.
std::vector<std::string_view> splitRecords(std::string_view blob, size_t maxBytes);

// The pieces of one keyframe, in order. Not thread-safe (one subscriber).
class LiveKeyframe {
 public:
  // The sequence number of the last batch the keyframe includes: the next batch a subscriber gets is seq()+1.
  uint64_t seq() const { return m_seq; }
  // Writes the next piece (at most kMaxEventBytes) to `chunk`; false when there is none left.
  bool next(std::string& chunk);

 private:
  friend class LiveEncoder;
  uint64_t m_seq = 0;
  std::vector<std::string> m_defs;  // table definitions, already split
  size_t m_defIndex = 0;
  std::vector<LiveBatch> m_batches;
  size_t m_batchIndex = 0;
  size_t m_offset = 0;  // inside the events part of m_batches[m_batchIndex]
};

class LiveEncoder final : public capture::ILiveSink {
 public:
  explicit LiveEncoder(LiveEncoderConfig config = {});
  // Calls finish().
  ~LiveEncoder() override;
  LiveEncoder(const LiveEncoder&) = delete;
  LiveEncoder& operator=(const LiveEncoder&) = delete;

  // ---- capture::ILiveSink ----
  void onSessionInfo(const capture::LiveSessionInfo& info) override;
  void onProcess(const capture::LiveProcess& p) override;
  void onThread(const capture::LiveThread& t) override;
  void onModule(const capture::LiveModule& m) override;
  void onSample(uint64_t ts, uint32_t pid, uint32_t tid, uint32_t cpu, uint16_t flags,
                std::span<const uint64_t> ips) override;
  void onContextSwitch(const capture::LiveContextSwitch& c) override;
  void onReady(const capture::LiveReady& r) override;
  void onScopeEvent(const capture::LiveScopeEvent& e) override;
  void onFrameMark(const capture::LiveFrameMark& f) override;
  void onMarker(const capture::LiveMarker& m) override;
  void onPlot(const capture::LivePlot& p) override;
  void onTimeAdvance(uint64_t ts) override;
  void onDropped(uint64_t events) override;

  // Thread-safe. Cuts a batch now when anything is pending (or the clock moved); returns its sequence number
  // (0: nothing to cut).
  uint64_t flush();
  // Thread-safe. The capture is over: cuts the last batch and wakes the subscribers; later events are ignored.
  void finish();
  // Thread-safe.
  bool ended() const;

  // Thread-safe. Sequence number of the newest batch (0: none yet).
  uint64_t lastSeq() const;
  // Thread-safe. True when a subscriber that wants batch `seq` next can be served from the retained batches
  // (seq in [oldest retained, lastSeq + 1]).
  bool retains(uint64_t seq) const;
  // Thread-safe. Waits up to `timeout` for batch `seq`.
  LiveWait waitBatch(uint64_t seq, LiveBatch& out, std::chrono::milliseconds timeout);
  // Thread-safe. See the class comment.
  LiveKeyframe beginKeyframe();

  // Thread-safe.
  LiveEncoderStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace tachyon::control
