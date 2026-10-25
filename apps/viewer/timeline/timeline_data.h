// Data side of the threads timeline (WP-42): the background LOD build, per-thread summaries used for sorting,
// the frame-mark index and frame/callstack descriptions. Everything here is derived from one document and is
// owned by the widget keyed by the document generation (see TimelineWidget).
#pragma once

#include "analysis/lod.h"
#include "model/model.h"
#include "symbols/function_key.h"
#include "symbols/symbol_service.h"
#include "viewer/job_queue.h"
#include "viewer/timeline/timeline_math.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer::timeline {

// Per-thread numbers for the row sort. Running ticks are the sum of the Running state intervals.
struct ThreadSummary {
  uint64_t runningTicks = 0;
  uint64_t sampleCount = 0;
};

// State shared between the main thread and the LOD build job. The job fills `summaries`, then `lod`, then
// builds; the main thread only reads through the atomics. Kept alive by shared_ptr from both sides, and it
// owns the model so the LOD never outlives it.
struct LodShared {
  explicit LodShared(std::shared_ptr<const model::TraceModel> m) : model(std::move(m)) {}

  const std::shared_ptr<const model::TraceModel> model;
  std::vector<ThreadSummary> summaries;  // valid once summariesReady
  std::atomic<bool> summariesReady{false};
  std::unique_ptr<analysis::TimelineLod> lodOwner;  // written by the job before `lod` is published
  std::atomic<const analysis::TimelineLod*> lod{nullptr};
  std::atomic<bool> finished{false};  // the job body returned (success, failure or cancellation)
  std::atomic<bool> failed{false};
  std::string error;  // valid when failed (written before the release store of `failed`)

  // Thread-safe.
  const analysis::TimelineLod* lodIfPrepared() const { return lod.load(std::memory_order_acquire); }
  // Thread-safe.
  const std::vector<ThreadSummary>* summariesIfReady() const {
    return summariesReady.load(std::memory_order_acquire) ? &summaries : nullptr;
  }
};

// Thread-safe (pure): the per-thread summaries of a model with built thread states.
std::vector<ThreadSummary> computeThreadSummaries(const model::TraceModel& model);

// Runs the whole build on the calling thread: summaries, prepare, build. Not thread-safe against a second
// call on the same `shared`; the main thread may read `shared` concurrently through its accessors. `cancel`
// may be null.
void buildLodShared(LodShared& shared, const analysis::LodOptions& options, const std::atomic<bool>* cancel,
                    const std::function<void(float)>& progress);

// Main-thread only. Submits the job that computes the summaries, prepares the LOD and builds it. `onDone`
// (main thread, from JobQueue::pump) is called when the job finished or was cancelled.
JobQueue::JobId startLodBuild(JobQueue& jobs, const std::shared_ptr<LodShared>& shared,
                              const analysis::LodOptions& options, std::function<void()> onDone);

// Frame marks as vertical ticks: sorted by time, one entry per frame number (marks from several threads with
// the same number collapse to the earliest).
struct FrameTick {
  uint64_t ts = 0;
  uint64_t number = 0;
};
// Thread-safe (pure).
std::vector<FrameTick> buildFrameTicks(const model::TraceModel& model);
// Thread-safe (pure). Index of the first tick with ts >= t.
size_t firstFrameTickAtOrAfter(const std::vector<FrameTick>& ticks, uint64_t t);

// Where a frame sits in its callstack. Return addresses of non-leaf frames resolve at rva-1
// (analysis-query.md, "Return-address rule"); the leaf frame of a kernel-mode sample is the exception.
enum class FramePosition : uint8_t { Leaf, NonLeaf };

// Thread-safe (reads what SymbolService::lookup reads). THE one place where the timeline turns a frame into a
// function key (selection, hover, highlight, by-function colours): when WP-32b's caller variant of
// functionKeyFor lands, only the body of this function changes. `position` and `kernelLeaf` (the sample's
// kernel-leaf flag) are the inputs that rule needs; until then every frame is resolved as a leaf.
//
// Returns the key of the function the frame of `node` belongs to (spec 3.7, WP-20): symbols::functionKeyFor
// when `symbols` is given, otherwise the unresolved form
// ({module, rva} or {invalid, address}). nullopt for invalid nodes and the root. Keys change when the frame's
// module resolves.
std::optional<symbols::FunctionKey> functionKeyForNode(const model::TraceModel& model,
                                                       const symbols::SymbolService* symbols,
                                                       CallstackIdx node,
                                                       FramePosition position = FramePosition::Leaf,
                                                       bool kernelLeaf = false);
// Thread-safe (same). "engine.exe!Render::draw" for a resolved frame, "engine.exe!0x1a2b" (module file name,
// rva) while the module is unresolved or the address is in no function, "0x7ff61234" for an unmapped frame,
// "(root)" for the root and "?" for an invalid node.
std::string describeCallstackLeaf(const model::TraceModel& model, CallstackIdx node,
                                  const symbols::SymbolService* symbols = nullptr);
// Thread-safe (pure). Depth of a node (root = 0, outermost frame = 1), 0 for invalid nodes.
uint32_t callstackDepth(const model::TraceModel& model, CallstackIdx node);

// ---- frame marks on screen ----
struct FrameMarkItem {
  float x = 0;            // screen x of the tick
  bool hasLabel = false;  // the frame number is drawn to the right of the tick
  uint64_t number = 0;
};
// Thread-safe (pure). Which frame marks are drawn for a plot area [x0, x0 + width) showing `view`: ticks
// closer than 3 px to the previous drawn one are skipped (binary search, not a scan), and a label is placed
// only where it fits behind the previous label and inside the plot (`charWidth` is the width of one digit).
std::vector<FrameMarkItem> layoutFrameMarks(const std::vector<FrameTick>& ticks, const TimeView& view,
                                            const TimeLimits& limits, float x0, float width, float charWidth);

// ---- tooltip ----
struct TooltipInput {
  const model::TraceModel* model = nullptr;
  const symbols::SymbolService* symbols = nullptr;
  ThreadIdx thread;
  uint64_t ts = 0;        // absolute tick under the cursor
  double relTimeNs = 0;   // ts relative to the trace start
  double nsPerPixel = 1;  // sets the precision of the time
  double clockHz = 1e9;
  bool pending = false;  // the thread's LOD is not ready
  CallstackIdx stack;    // dominant stack of the column (strip) or the flame span's node
  float share = 0;       // share of `stack` among the samples of the column
  bool hasSpan = false;  // hovering a flame span
  uint32_t spanDepth = 0;
  CallstackIdx spanNode;  // invalid for a merged span
  uint64_t spanTicks = 0;
  uint32_t spanMerged = 1;
};
// Thread-safe (pure). The tooltip text, one entry per line: thread, time, state with wait reason and
// category, interval length, and the top function (strip) or the hovered span.
std::vector<std::string> tooltipLines(const TooltipInput& input);

}  // namespace tachyon::viewer::timeline
