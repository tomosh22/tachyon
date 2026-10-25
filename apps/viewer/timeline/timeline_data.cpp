#include "viewer/timeline/timeline_data.h"

#include "analysis/thread_states.h"
#include "analysis/wait_reasons.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
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

namespace {

const model::Frame* frameOf(const model::TraceModel& model, CallstackIdx node, FrameIdx& frameIdx) {
  if (!node.valid() || node.get() >= model.nodes.size()) return nullptr;
  frameIdx = model.nodes[node.get()].frame;
  if (!frameIdx.valid() || frameIdx.get() >= model.frames.size()) return nullptr;
  return &model.frames[frameIdx.get()];
}

}  // namespace

std::optional<symbols::FunctionKey> functionKeyForNode(const model::TraceModel& model,
                                                       const symbols::SymbolService* symbols,
                                                       CallstackIdx node, FramePosition position,
                                                       bool kernelLeaf) {
  FrameIdx fi;
  const model::Frame* f = frameOf(model, node, fi);
  if (!f) return std::nullopt;
  if (symbols) {
    // Return-address rule: non-leaf frames resolve at rva-1, except the interrupted user IP.
    const bool exact = position == FramePosition::Leaf || kernelLeaf;
    return exact ? symbols::functionKeyFor(*symbols, fi) : symbols::functionKeyForCaller(*symbols, fi);
  }
  return symbols::FunctionKey{f->module.valid() ? f->module : ModuleIdx::invalid(), f->addr_or_rva};
}

std::string describeCallstackLeaf(const model::TraceModel& model, CallstackIdx node,
                                  const symbols::SymbolService* symbols) {
  if (!node.valid() || node.get() >= model.nodes.size()) return "?";
  FrameIdx fi;
  const model::Frame* f = frameOf(model, node, fi);
  if (!f) return "(root)";
  char buf[96];
  if (!f->module.valid() || f->module.get() >= model.modules.size()) {
    std::snprintf(buf, sizeof buf, "0x%" PRIx64, f->addr_or_rva);
    return buf;
  }
  std::string path = model.modules[f->module.get()].path;
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos) path.erase(0, slash + 1);
  if (symbols) {
    const symbols::ResolvedAddress& r = symbols->lookup(fi);
    if (r.resolved && r.function.valid()) {
      const std::string_view name = symbols->symbolStrings().get(r.function);
      if (!name.empty()) return path + "!" + std::string(name);
    }
  }
  std::snprintf(buf, sizeof buf, "!0x%" PRIx64, f->addr_or_rva);
  return path + buf;
}

std::vector<FrameMarkItem> layoutFrameMarks(const std::vector<FrameTick>& ticks, const TimeView& view,
                                            const TimeLimits& limits, float x0, float width,
                                            float charWidth) {
  std::vector<FrameMarkItem> out;
  if (ticks.empty() || !(width > 0.0f)) return out;
  const TickRange window = queryWindow(view, limits);
  const double pxPerTick = width / view.span;
  size_t i = firstFrameTickAtOrAfter(ticks, window.t0);
  if (i > 0) --i;  // the tick just before the window may have its label inside it
  float lastX = -1e9f, lastLabelEnd = -1e9f;
  for (; i < ticks.size() && ticks[i].ts <= window.t1; ++i) {
    const float x = static_cast<float>(relTickToX(view, toRelative(limits, ticks[i].ts), x0, width));
    if (x < lastX + 3.0f) {
      // Skip ahead to the first tick at least 3 px later instead of visiting every tick.
      const uint64_t nextTs = ticks[i].ts + static_cast<uint64_t>(std::ceil(3.0 / pxPerTick));
      const size_t j = firstFrameTickAtOrAfter(ticks, nextTs);
      if (j > i + 1) i = j - 1;
      continue;
    }
    lastX = x;
    FrameMarkItem item;
    item.x = x;
    item.number = ticks[i].number;
    if (x >= x0 - 1.0f && x <= x0 + width) {
      size_t digits = 1;
      for (uint64_t n = ticks[i].number; n >= 10; n /= 10) ++digits;
      const float textWidth = static_cast<float>(digits) * charWidth;
      if (x > lastLabelEnd + 4.0f && x + 2.0f + textWidth < x0 + width) {
        item.hasLabel = true;
        lastLabelEnd = x + 2.0f + textWidth;
      }
    }
    out.push_back(item);
  }
  return out;
}

std::vector<std::string> tooltipLines(const TooltipInput& in) {
  std::vector<std::string> lines;
  const model::TraceModel& model = *in.model;
  const auto fmt = [](const char* f, auto... args) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, args...);
    return std::string(buf);
  };
  const model::Thread& th = model.threads[in.thread.get()];
  lines.push_back(fmt("%s (tid %u)", th.name.empty() ? "(unnamed)" : th.name.c_str(), th.tid));
  lines.push_back("Time: " + formatTimeNs(in.relTimeNs, std::max(in.nsPerPixel, 1.0)));
  if (const model::ThreadStateInterval* s = analysis::stateAt(model, in.thread, in.ts)) {
    switch (s->state) {
      case model::ThreadState::Running: lines.push_back(fmt("State: Running (cpu %u)", s->cpu)); break;
      case model::ThreadState::Ready: lines.push_back("State: Ready (runnable, waiting for a cpu)"); break;
      case model::ThreadState::Waiting: {
        const std::string_view reason = analysis::waitReasonName(model.header.platform, s->wait_reason);
        const std::string_view cat =
            analysis::waitCategoryName(analysis::waitReasonCategory(model.header.platform, s->wait_reason));
        lines.push_back("State: Waiting - " + std::string(reason) + " (" + std::string(cat) + ")");
        break;
      }
      case model::ThreadState::Unknown: lines.push_back("State: Unknown"); break;
    }
    lines.push_back("Interval: " + formatDurationTicks(s->end - s->start, in.clockHz));
  }
  if (in.pending) {
    lines.push_back("Timeline index still building...");
  } else if (in.hasSpan) {
    const std::string fn =
        in.spanNode.valid() ? describeCallstackLeaf(model, in.spanNode, in.symbols) : "(merged)";
    lines.push_back(fmt("Depth %u: %s", in.spanDepth, fn.c_str()));
    lines.push_back("Span: " + formatDurationTicks(in.spanTicks, in.clockHz));
    if (in.spanMerged > 1) lines.push_back(fmt("%u samples merged", in.spanMerged));
  } else if (in.stack.valid()) {
    lines.push_back(fmt("Top function: %s (%.0f%% of samples here)",
                        describeCallstackLeaf(model, in.stack, in.symbols).c_str(),
                        static_cast<double>(in.share) * 100.0));
  } else {
    lines.push_back("No samples");
  }
  return lines;
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
