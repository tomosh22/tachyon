#include "viewer/interaction/interaction_model.h"

#include "analysis/attribution.h"
#include "analysis/thread_states.h"
#include "analysis/wait_reasons.h"
#include "viewer/timeline/timeline_math.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace tachyon::viewer::interaction {

namespace {

using model::ThreadState;
using model::ThreadStateInterval;

const std::vector<ThreadStateInterval>* intervalsOf(const model::TraceModel& model, ThreadIdx thread) {
  if (!thread.valid() || thread.get() >= model.statesByThread.size()) return nullptr;
  return &model.statesByThread[thread.get()];
}

// A Ready interval that is the second half of a Waiting interval split by a ReadyRec.
bool continuesWait(const ThreadStateInterval& wait, const ThreadStateInterval& ready) {
  return wait.state == ThreadState::Waiting && ready.state == ThreadState::Ready && wait.ready_ts != 0 &&
         wait.ready_ts == ready.ready_ts && wait.end == ready.start &&
         wait.blocking_stack == ready.blocking_stack;
}

std::string moduleFileName(const model::TraceModel& model, ModuleIdx module) {
  std::string path = model.modules[module.get()].path;
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos) path.erase(0, slash + 1);
  return path;
}

std::string hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%" PRIx64, v);
  return buf;
}

}  // namespace

std::optional<WaitEpisode> episodeForInterval(const model::TraceModel& model, ThreadIdx thread,
                                              size_t intervalIndex) {
  const auto* ivs = intervalsOf(model, thread);
  if (!ivs || intervalIndex >= ivs->size()) return std::nullopt;
  const ThreadStateInterval& at = (*ivs)[intervalIndex];
  if (at.state != ThreadState::Waiting && at.state != ThreadState::Ready) return std::nullopt;

  size_t first = intervalIndex;
  if (at.state == ThreadState::Ready && first > 0 && continuesWait((*ivs)[first - 1], at)) --first;
  size_t last = first;
  if (first + 1 < ivs->size() && continuesWait((*ivs)[first], (*ivs)[first + 1])) last = first + 1;

  const ThreadStateInterval& head = (*ivs)[first];
  WaitEpisode e;
  e.thread = thread;
  e.firstInterval = first;
  e.lastInterval = last;
  e.originState = head.state;
  e.switchOutTs = head.start;
  e.switchInTs = (*ivs)[last].end;
  e.waitReason = head.wait_reason;
  e.cpu = head.cpu;
  e.blockingStack = head.blocking_stack;
  e.unblockingStack = head.unblocking_stack;
  e.wakerTid = head.woke_by_tid;
  e.hasWaker = head.woke_by_tid != 0 || head.unblocking_stack.valid();
  if (e.wakerTid != 0) {
    const auto it = model.tidToThread.find(e.wakerTid);
    if (it != model.tidToThread.end()) e.wakerThread = it->second;
  }
  if (head.state == ThreadState::Waiting) {
    e.hasReadyRec = head.ready_ts != 0;
    e.readyTs = e.hasReadyRec ? head.ready_ts : e.switchInTs;
    e.readyLatencyKnown = e.hasReadyRec;
  } else {
    // Preempted: runnable since the switch-out unless a ReadyRec says otherwise.
    e.hasReadyRec = e.hasWaker;
    e.readyTs = head.ready_ts != 0 ? head.ready_ts : e.switchOutTs;
    e.readyLatencyKnown = true;
  }
  e.readyTs = std::clamp(e.readyTs, e.switchOutTs, e.switchInTs);
  e.waitTicks = e.readyTs - e.switchOutTs;
  e.readyLatencyTicks = e.switchInTs - e.readyTs;
  return e;
}

std::optional<WaitEpisode> episodeAt(const model::TraceModel& model, ThreadIdx thread, uint64_t ts) {
  const auto* ivs = intervalsOf(model, thread);
  if (!ivs) return std::nullopt;
  const ThreadStateInterval* s = analysis::stateAt(model, thread, ts);
  if (!s) return std::nullopt;
  return episodeForInterval(model, thread, static_cast<size_t>(s - ivs->data()));
}

std::optional<WaitEpisode> episodeStartingAt(const model::TraceModel& model, ThreadIdx thread,
                                             uint64_t switchOutTs) {
  const auto* ivs = intervalsOf(model, thread);
  if (!ivs) return std::nullopt;
  const auto it = std::lower_bound(ivs->begin(), ivs->end(), switchOutTs,
                                   [](const ThreadStateInterval& iv, uint64_t v) { return iv.start < v; });
  if (it == ivs->end() || it->start != switchOutTs) return std::nullopt;
  std::optional<WaitEpisode> e = episodeForInterval(model, thread, static_cast<size_t>(it - ivs->begin()));
  if (e && e->switchOutTs != switchOutTs) return std::nullopt;
  return e;
}

std::string threadLabel(const model::TraceModel& model, ThreadIdx thread, uint32_t tidFallback) {
  char buf[256];
  if (thread.valid() && thread.get() < model.threads.size()) {
    const model::Thread& t = model.threads[thread.get()];
    std::snprintf(buf, sizeof buf, "%s (tid %u)", t.name.empty() ? "(unnamed)" : t.name.c_str(), t.tid);
  } else {
    std::snprintf(buf, sizeof buf, "tid %u", tidFallback);
  }
  return buf;
}

EpisodeDetails describeEpisode(const model::TraceModel& model, const WaitEpisode& e, double clockHz) {
  EpisodeDetails d;
  d.woken = threadLabel(model, e.thread);
  d.waker =
      e.hasWaker ? threadLabel(model, e.wakerThread, e.wakerTid) : std::string("unknown (no wake-up record)");
  const uint32_t platform = model.header.platform;
  if (e.isWait()) {
    d.waitReason = std::string(analysis::waitReasonName(platform, e.waitReason));
    d.waitCategory =
        std::string(analysis::waitCategoryName(analysis::waitReasonCategory(platform, e.waitReason)));
  } else {
    d.waitReason = "Preempted";
    d.waitCategory = std::string(analysis::waitCategoryName(analysis::WaitCategory::Preempted));
  }
  d.waitDuration = timeline::formatDurationTicks(e.waitTicks, clockHz);
  d.readyLatency =
      e.readyLatencyKnown ? timeline::formatDurationTicks(e.readyLatencyTicks, clockHz) : std::string("n/a");
  const uint64_t start = model.header.start_ts;
  const double ns = e.switchOutTs > start ? static_cast<double>(e.switchOutTs - start) * 1e9 / clockHz : 0.0;
  d.switchOutTime = timeline::formatTimeNs(ns, 1.0);
  return d;
}

WaitScanResult longestWaits(const model::TraceModel& model, const WaitQuery& query,
                            const std::atomic<bool>* cancel) {
  struct Cand {
    uint64_t ticks;
    uint64_t start;
    uint32_t thread;
    size_t index;
  };
  // "a ranks before b".
  const auto better = [](const Cand& a, const Cand& b) {
    if (a.ticks != b.ticks) return a.ticks > b.ticks;
    if (a.start != b.start) return a.start < b.start;
    if (a.thread != b.thread) return a.thread < b.thread;
    return a.index < b.index;
  };

  WaitScanResult result;
  std::vector<uint32_t> threads;
  if (query.threads) {
    for (const ThreadIdx t : *query.threads) {
      if (t.valid() && t.get() < model.statesByThread.size()) threads.push_back(t.get());
    }
    std::sort(threads.begin(), threads.end());
    threads.erase(std::unique(threads.begin(), threads.end()), threads.end());
  } else {
    threads.resize(model.statesByThread.size());
    for (size_t i = 0; i < threads.size(); ++i) threads[i] = static_cast<uint32_t>(i);
  }

  std::vector<Cand> heap;  // heap of the best maxResults candidates; the front is the worst of them
  uint64_t visited = 0;
  for (const uint32_t t : threads) {
    const std::vector<ThreadStateInterval>& ivs = model.statesByThread[t];
    size_t i = static_cast<size_t>(
        std::partition_point(ivs.begin(), ivs.end(),
                             [&](const ThreadStateInterval& iv) { return iv.end <= query.t0; }) -
        ivs.begin());
    for (; i < ivs.size() && ivs[i].start < query.t1; ++i) {
      if ((++visited & 0xFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.rows.clear();
        result.totalWaits = 0;
        return result;
      }
      const ThreadStateInterval& iv = ivs[i];
      // The Waiting piece of an episode ends at the wake-up (or the switch-in): its length is the wait.
      if (iv.state != ThreadState::Waiting) continue;
      ++result.totalWaits;
      if (query.maxResults == 0) continue;
      const Cand c{iv.end - iv.start, iv.start, t, i};
      if (heap.size() < query.maxResults) {
        heap.push_back(c);
        std::push_heap(heap.begin(), heap.end(), better);
      } else if (better(c, heap.front())) {
        std::pop_heap(heap.begin(), heap.end(), better);
        heap.back() = c;
        std::push_heap(heap.begin(), heap.end(), better);
      }
    }
  }
  std::sort(heap.begin(), heap.end(), better);
  result.rows.reserve(heap.size());
  for (const Cand& c : heap) {
    if (auto e = episodeForInterval(model, ThreadIdx(c.thread), c.index)) result.rows.push_back(*e);
  }
  return result;
}

std::vector<StackLine> describeStack(const model::TraceModel& model, const symbols::SymbolService* symbols,
                                     CallstackIdx leaf, size_t maxFrames) {
  std::vector<StackLine> out;
  if (!leaf.valid() || leaf.get() == 0 || leaf.get() >= model.nodes.size()) return out;
  uint32_t n = leaf.get();
  for (size_t pos = 0;
       n != 0 && n < model.nodes.size() && (maxFrames == 0 || pos < maxFrames) && pos < model.nodes.size();
       ++pos, n = model.nodes[n].parent.get()) {
    StackLine line;
    line.position = pos;
    line.frame = model.nodes[n].frame;
    line.exactAddress = analysis::usesExactAddress(model, leaf, pos, false);
    if (!line.frame.valid() || line.frame.get() >= model.frames.size()) {
      line.text = "?";
      out.push_back(std::move(line));
      continue;
    }
    const model::Frame& f = model.frames[line.frame.get()];
    const bool mapped = f.module.valid() && f.module.get() < model.modules.size();
    if (symbols) {
      line.key = line.exactAddress ? symbols::functionKeyFor(*symbols, line.frame)
                                   : symbols::functionKeyForCaller(*symbols, line.frame);
    } else {
      line.key = symbols::FunctionKey{mapped ? f.module : ModuleIdx::invalid(), f.addr_or_rva};
    }
    if (!mapped) {
      line.text = hex(f.addr_or_rva);
    } else {
      std::string name;
      if (symbols) {
        const symbols::ResolvedAddress& r =
            line.exactAddress ? symbols->lookup(line.frame) : symbols->lookupCaller(line.frame);
        if (r.resolved && r.function.valid()) name = std::string(symbols->symbolStrings().get(r.function));
      }
      line.text = moduleFileName(model, f.module) + "!" + (name.empty() ? hex(f.addr_or_rva) : name);
    }
    out.push_back(std::move(line));
  }
  return out;
}

std::vector<std::string> interactionTooltipLines(const model::TraceModel& model,
                                                 const symbols::SymbolService* symbols, const WaitEpisode& e,
                                                 double clockHz, size_t maxFrames) {
  std::vector<std::string> lines;
  if (e.hasWaker) {
    lines.push_back("Woken by: " + threadLabel(model, e.wakerThread, e.wakerTid));
  } else {
    lines.push_back("Woken by: unknown (no wake-up record)");
  }
  std::string timing = "Wait: " + timeline::formatDurationTicks(e.waitTicks, clockHz);
  timing += "   Ready latency: " + (e.readyLatencyKnown
                                        ? timeline::formatDurationTicks(e.readyLatencyTicks, clockHz)
                                        : std::string("n/a"));
  lines.push_back(std::move(timing));
  const auto addStack = [&](const char* title, CallstackIdx stack) {
    lines.push_back(title);
    std::vector<StackLine> frames = describeStack(model, symbols, stack, maxFrames + 1);
    if (frames.empty()) {
      lines.push_back("  (none)");
      return;
    }
    const bool more = frames.size() > maxFrames;
    if (more) frames.resize(maxFrames);
    for (const StackLine& l : frames) lines.push_back("  " + l.text);
    if (more) lines.push_back("  ...");
  };
  addStack("Blocking stack:", e.blockingStack);
  addStack("Unblocking stack:", e.unblockingStack);
  return lines;
}

std::optional<NavTarget> wakerTarget(const WaitEpisode& e, double clockHz) {
  if (!e.hasWaker || !e.wakerThread.valid()) return std::nullopt;
  const uint64_t minHalf = std::max<uint64_t>(8, static_cast<uint64_t>(clockHz * 10e-6));
  const uint64_t half = std::max({e.readyTs - e.switchOutTs, e.switchInTs - e.readyTs, minHalf});
  NavTarget t;
  t.thread = e.wakerThread;
  t.t0 = e.readyTs > half ? e.readyTs - half : 0;
  t.t1 = e.readyTs + half;
  return t;
}

NavTarget wokenTarget(const WaitEpisode& e, double clockHz) {
  const uint64_t minSpan = std::max<uint64_t>(16, static_cast<uint64_t>(clockHz * 20e-6));
  NavTarget t;
  t.thread = e.thread;
  t.t0 = e.switchOutTs;
  t.t1 = std::max(e.switchInTs, e.switchOutTs + minSpan);
  return t;
}

}  // namespace tachyon::viewer::interaction
