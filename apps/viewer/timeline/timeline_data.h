// Data side of the threads timeline (WP-42): the background LOD build, per-thread summaries used for sorting,
// the frame-mark index and frame/callstack descriptions. Everything here is derived from one document and is
// owned by the widget keyed by the document generation (see TimelineWidget).
#pragma once

#include "analysis/lod.h"
#include "model/model.h"
#include "viewer/job_queue.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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

// Thread-safe (pure). "engine.exe+0x1a2b" (module file name plus rva) or "0x7ff61234" for an absolute
// address;
// "?" for an invalid node. Symbols are not available yet (WP-20); this is the placeholder description.
std::string describeCallstackLeaf(const model::TraceModel& model, CallstackIdx node);
// Thread-safe (pure). Depth of a node (root = 0, outermost frame = 1), 0 for invalid nodes.
uint32_t callstackDepth(const model::TraceModel& model, CallstackIdx node);

}  // namespace tachyon::viewer::timeline
