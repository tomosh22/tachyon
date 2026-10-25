// Pure logic of the thread interaction feature (WP-46): wait episodes (switch-out -> optional wake-up ->
// switch-in) derived from the per-thread state intervals of WP-31 (spec 3.8 rule 4), the "N longest waits"
// scan, stack formatting with the return-address rule, the compact tooltip and navigation ranges. No ImGui.
#pragma once

#include "core/strong_id.h"
#include "model/model.h"
#include "symbols/function_key.h"
#include "symbols/symbol_service.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer::interaction {

// Window title of the panel; the timeline asks the shell to open it by this name.
inline constexpr const char* kThreadInteractionPanelName = "Thread Interaction";

// One off-CPU episode of a thread: the Waiting interval [switchOut, ready) and, when a ReadyRec split the
// wait, the Ready interval [ready, switchIn) that follows it (thread_states.h: both pieces carry the same
// blocking / unblocking stacks, woke_by_tid and ready_ts). A switch-out that left the thread runnable
// (preemption) is a single Ready interval and an episode too (originState = Ready).
struct WaitEpisode {
  ThreadIdx thread;
  size_t firstInterval = 0, lastInterval = 0;                    // indices into statesByThread[thread]
  model::ThreadState originState = model::ThreadState::Waiting;  // Waiting or Ready
  uint64_t switchOutTs = 0;
  uint64_t switchInTs = 0;  // end of the episode: the next switch-in, or the end of the thread's coverage
  uint8_t waitReason = 0;
  uint32_t cpu = 0;
  CallstackIdx blockingStack, unblockingStack;
  // A ReadyRec was seen: readyTs is when the thread became runnable. For a preemption without ReadyRec
  // readyTs = switchOutTs (runnable all along) and hasReadyRec is false. For a wait without ReadyRec readyTs
  // is unknown (== switchInTs, so waitTicks covers the whole episode) and readyLatencyKnown is false.
  bool hasReadyRec = false;
  uint64_t readyTs = 0;
  bool hasWaker = false;  // woke_by_tid / unblocking stack present
  uint32_t wakerTid = 0;
  ThreadIdx wakerThread;           // invalid when the waker tid is not a thread of the model
  uint64_t waitTicks = 0;          // switchOut -> readyTs (or -> switchIn without a ReadyRec)
  uint64_t readyLatencyTicks = 0;  // readyTs -> switchIn
  bool readyLatencyKnown = false;

  bool isWait() const { return originState == model::ThreadState::Waiting; }
  bool operator==(const WaitEpisode&) const = default;
};

// All functions below: thread-safe for concurrent const access to the model unless stated otherwise.

// The episode containing interval `intervalIndex` of `thread`, or nullopt when that interval is Running or
// Unknown (or the index is out of range / states are not built).
std::optional<WaitEpisode> episodeForInterval(const model::TraceModel& model, ThreadIdx thread,
                                              size_t intervalIndex);
// The episode of the Waiting / Ready interval containing `ts`, or nullopt.
std::optional<WaitEpisode> episodeAt(const model::TraceModel& model, ThreadIdx thread, uint64_t ts);
// The episode whose switch-out is exactly `switchOutTs` (how WaitSelection identifies it), or nullopt.
std::optional<WaitEpisode> episodeStartingAt(const model::TraceModel& model, ThreadIdx thread,
                                             uint64_t switchOutTs);

// Name of a thread for display: "name (tid N)", "(unnamed) (tid N)" or "tid N" for an unknown thread.
std::string threadLabel(const model::TraceModel& model, ThreadIdx thread, uint32_t tidFallback = 0);

// The fields the Thread Interaction panel shows for an episode, as display strings.
struct EpisodeDetails {
  std::string woken;  // "name (tid N)"
  std::string waker;  // "name (tid N)", "tid N" (thread not in the trace) or "unknown (no wake-up record)"
  std::string waitReason;     // "WrQueue", "S (interruptible)" ..., or "Preempted" for a Ready origin
  std::string waitCategory;   // "Synchronization", "I/O", ...
  std::string waitDuration;   // switch-out -> wake-up (or switch-in without a ReadyRec)
  std::string readyLatency;   // wake-up -> switch-in, "n/a" when no ReadyRec
  std::string switchOutTime;  // relative to the trace start
};
EpisodeDetails describeEpisode(const model::TraceModel& model, const WaitEpisode& episode, double clockHz);

// ---- longest waits ----
struct WaitQuery {
  uint64_t t0 = 0, t1 = UINT64_MAX;               // half-open filter range
  std::optional<std::vector<ThreadIdx>> threads;  // nullopt = all threads
  size_t maxResults = 50;
  bool operator==(const WaitQuery&) const = default;
};
struct WaitScanResult {
  std::vector<WaitEpisode> rows;  // longest first
  uint64_t totalWaits = 0;        // waits that matched the filter (before the cut to maxResults)
  bool cancelled = false;
};
// The Waiting-origin episodes whose wait [switchOut, readyTs or switchIn) overlaps [t0, t1) on the selected
// threads, ranked by waitTicks (descending; ties: earlier switch-out, then lower thread index). The duration
// is the whole wait, not clipped to the filter. Preemptions are not waits and are skipped. Cancelled results
// are empty. `cancel` may be null (checked every few thousand intervals).
WaitScanResult longestWaits(const model::TraceModel& model, const WaitQuery& query,
                            const std::atomic<bool>* cancel);

// ---- stacks ----
struct StackLine {
  std::string text;  // "module!function", "module!0x<rva>" or "0x<addr>"
  FrameIdx frame;
  size_t position = 0;  // 0 = leaf
  bool exactAddress = false;
  std::optional<symbols::FunctionKey> key;  // the function the frame belongs to (return-address rule)
};
// Innermost frame first. The leaf is looked up at its exact address, outer frames as return addresses
// (analysis::usesExactAddress with kernelLeafSample = false). `maxFrames` 0 = all. Empty for an invalid
// stack. `symbols` may be null (unresolved names).
std::vector<StackLine> describeStack(const model::TraceModel& model, const symbols::SymbolService* symbols,
                                     CallstackIdx leaf, size_t maxFrames = 0);

// Text for the Ctrl-hover tooltip: waker, wait / latency and the top `maxFrames` frames of both stacks.
std::vector<std::string> interactionTooltipLines(const model::TraceModel& model,
                                                 const symbols::SymbolService* symbols,
                                                 const WaitEpisode& episode, double clockHz,
                                                 size_t maxFrames = 8);

// ---- navigation ----
struct NavTarget {
  ThreadIdx thread;
  uint64_t t0 = 0, t1 = 0;
};
// Where "Go to waker" shows: the waker's row around readyTs, wide enough to see both ends of the wait.
// nullopt without a waker thread in the model.
std::optional<NavTarget> wakerTarget(const WaitEpisode& episode, double clockHz);
// Where "Go to woken" shows: the woken thread's row, the whole episode.
NavTarget wokenTarget(const WaitEpisode& episode, double clockHz);

}  // namespace tachyon::viewer::interaction
