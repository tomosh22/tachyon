#include "viewer/timeline/timeline_data.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace tachyon::viewer::timeline {

std::vector<ThreadSummary> computeThreadSummaries(const model::TraceModel& model) {
  std::vector<ThreadSummary> out(model.threads.size());
  for (size_t t = 0; t < out.size(); ++t) {
    if (t < model.samplesByThread.size()) out[t].sampleCount = model.samplesByThread[t].ts.size();
    if (t < model.statesByThread.size()) {
      uint64_t running = 0;
      for (const model::ThreadStateInterval& s : model.statesByThread[t]) {
        if (s.state == model::ThreadState::Running) running += s.end - s.start;
      }
      out[t].runningTicks = running;
    }
  }
  return out;
}

void buildLodShared(LodShared& shared, const analysis::LodOptions& options, const std::atomic<bool>* cancel,
                    const std::function<void(float)>& progress) {
  const model::TraceModel& model = *shared.model;
  shared.summaries = computeThreadSummaries(model);
  shared.summariesReady.store(true, std::memory_order_release);
  if (cancel && cancel->load(std::memory_order_relaxed)) return;
  Result<analysis::TimelineLod> prepared = analysis::TimelineLod::prepare(model, options);
  if (!prepared.ok()) {
    shared.error = prepared.error().toString();
    shared.failed.store(true, std::memory_order_release);
    return;
  }
  shared.lodOwner = std::make_unique<analysis::TimelineLod>(std::move(prepared).value());
  shared.lod.store(shared.lodOwner.get(), std::memory_order_release);
  const Result<void> built = shared.lodOwner->build(cancel, progress);
  if (!built.ok() && !(cancel && cancel->load(std::memory_order_relaxed))) {
    shared.error = built.error().toString();
    shared.failed.store(true, std::memory_order_release);
  }
}

JobQueue::JobId startLodBuild(JobQueue& jobs, const std::shared_ptr<LodShared>& shared,
                              const analysis::LodOptions& options, std::function<void()> onDone) {
  return jobs.submit(
      "Building timeline index",
      [shared, options](JobContext& ctx) {
        buildLodShared(*shared, options, ctx.cancelFlag(), [&ctx](float f) { ctx.setProgress(f); });
      },
      [shared, onDone = std::move(onDone)](JobQueue::JobId, JobOutcome) {
        shared->finished.store(true, std::memory_order_release);
        if (onDone) onDone();
      });
}

std::vector<FrameTick> buildFrameTicks(const model::TraceModel& model) {
  std::vector<FrameTick> ticks;
  ticks.reserve(model.frameMarks.size());
  for (const model::FrameMark& m : model.frameMarks) ticks.push_back({m.ts, m.frame_number});
  std::stable_sort(ticks.begin(), ticks.end(),
                   [](const FrameTick& a, const FrameTick& b) { return a.ts < b.ts; });
  // Collapse repeated frame numbers (one mark per thread for the same frame), keeping the earliest.
  std::vector<FrameTick> out;
  out.reserve(ticks.size());
  for (const FrameTick& t : ticks) {
    if (!out.empty() && out.back().number == t.number) continue;
    out.push_back(t);
  }
  return out;
}

size_t firstFrameTickAtOrAfter(const std::vector<FrameTick>& ticks, uint64_t t) {
  return static_cast<size_t>(std::lower_bound(ticks.begin(), ticks.end(), t,
                                              [](const FrameTick& a, uint64_t v) { return a.ts < v; }) -
                             ticks.begin());
}

std::string describeCallstackLeaf(const model::TraceModel& model, CallstackIdx node) {
  if (!node.valid() || node.get() >= model.nodes.size()) return "?";
  const FrameIdx fi = model.nodes[node.get()].frame;
  if (!fi.valid() || fi.get() >= model.frames.size()) return "(root)";
  const model::Frame& f = model.frames[fi.get()];
  char buf[96];
  if (!f.module.valid() || f.module.get() >= model.modules.size()) {
    std::snprintf(buf, sizeof buf, "0x%" PRIx64, f.addr_or_rva);
    return buf;
  }
  std::string path = model.modules[f.module.get()].path;
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos) path.erase(0, slash + 1);
  std::snprintf(buf, sizeof buf, "+0x%" PRIx64, f.addr_or_rva);
  return path + buf;
}

uint32_t callstackDepth(const model::TraceModel& model, CallstackIdx node) {
  if (!node.valid() || node.get() >= model.nodes.size()) return 0;
  uint32_t depth = 0;
  for (uint32_t v = node.get(); v != 0 && v < model.nodes.size() && depth < (1u << 20);
       v = model.nodes[v].parent.get()) {
    ++depth;
  }
  return depth;
}

}  // namespace tachyon::viewer::timeline
