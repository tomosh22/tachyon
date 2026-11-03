#include "viewer/timeline/scope_lanes.h"

#include "analysis/locks.h"
#include "viewer/analysis_context.h"
#include "viewer/common/rect_batch.h"
#include "viewer/common/scope_view_options.h"
#include "viewer/document.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace tachyon::viewer::timeline {

namespace {

ImU32 toIm(Color32 c) {
  return static_cast<ImU32>(c);
}

void addInterval(ScopeLane& lane, const analysis::ScopeInstance* inst, uint64_t begin, uint64_t end,
                 uint64_t& dropped) {
  if (inst->depth >= kMaxScopeDepth) {
    ++dropped;
    return;
  }
  if (lane.depths.size() <= inst->depth) lane.depths.resize(inst->depth + 1);
  ScopeTrack& t = lane.depths[inst->depth];
  t.begin.push_back(begin);
  t.end.push_back(end);
  t.instance.push_back(inst);
  t.fill.push_back(scopeColor(inst->color, inst->id));
  const uint8_t f = static_cast<uint8_t>((inst->color == analysis::kLockColorSentinel ? kTrackLock : 0) |
                                         (inst->unterminated ? kTrackOpen : 0));
  t.flags.push_back(f);
  const size_t block = (t.flags.size() - 1) / ScopeTrack::kFlagBlock;
  if (t.flagBlocks.size() <= block) t.flagBlocks.resize(block + 1, 0);
  t.flagBlocks[block] = static_cast<uint8_t>(t.flagBlocks[block] | f);
  ++lane.intervalCount;
}

// First position in [from, v.size()) whose value is >= target, found by galloping from `from`: the answer is
// usually close (the intervals of a pixel), so this is O(log distance) instead of O(log n).
size_t firstAtOrAfter(const std::vector<uint64_t>& v, size_t from, uint64_t target) {
  const size_t n = v.size();
  if (from >= n || v[from] >= target) return from;
  size_t a = from, step = 1, b = from + 1;  // v[a] < target
  while (b < n && v[b] < target) {
    a = b;
    step *= 2;
    b = a + step;
  }
  b = std::min(b, n);
  return static_cast<size_t>(std::lower_bound(v.begin() + static_cast<ptrdiff_t>(a) + 1,
                                              v.begin() + static_cast<ptrdiff_t>(b), target) -
                             v.begin());
}

// Sorts the lanes by (thread, fiberId) and fills lanesOfThread.
void finishSet(ScopeLaneSet& set, size_t threadCount) {
  std::vector<uint32_t> order(set.lanes.size());
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const ScopeLane &x = set.lanes[a], &y = set.lanes[b];
    if (x.thread.get() != y.thread.get()) return x.thread.get() < y.thread.get();
    return x.fiberId < y.fiberId;
  });
  std::vector<ScopeLane> sorted;
  sorted.reserve(set.lanes.size());
  for (const uint32_t i : order) sorted.push_back(std::move(set.lanes[i]));
  set.lanes = std::move(sorted);
  set.lanesOfThread.assign(threadCount, {});
  set.intervalCount = 0;
  for (size_t i = 0; i < set.lanes.size(); ++i) {
    const uint32_t t = set.lanes[i].thread.get();
    if (t < threadCount) set.lanesOfThread[t].push_back(static_cast<uint32_t>(i));
    set.intervalCount += set.lanes[i].intervalCount;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Lane set

uint32_t ScopeTrack::fillOf(size_t i) const {
  if (i < fill.size()) return fill[i];
  return scopeColor(instance[i]->color, instance[i]->id);
}

uint8_t ScopeTrack::flagsOf(size_t i) const {
  if (i < flags.size()) return flags[i];
  return static_cast<uint8_t>((instance[i]->color == analysis::kLockColorSentinel ? kTrackLock : 0) |
                              (instance[i]->unterminated ? kTrackOpen : 0));
}

uint8_t ScopeTrack::flagsOver(size_t first, size_t count) const {
  if (count <= 1) return first < size() ? flagsOf(first) : uint8_t{0};
  const size_t last = std::min(size(), first + count);
  uint8_t out = 0;
  size_t i = first;
  if (flagBlocks.empty()) {  // a hand-built track: visit the intervals (cap the work)
    for (; i < last && i < first + 4096; ++i) out = static_cast<uint8_t>(out | flagsOf(i));
    return out;
  }
  for (; i < last && i % kFlagBlock != 0; ++i) out = static_cast<uint8_t>(out | flags[i]);
  for (; i + kFlagBlock <= last; i += kFlagBlock) out = static_cast<uint8_t>(out | flagBlocks[i / kFlagBlock]);
  for (; i < last; ++i) out = static_cast<uint8_t>(out | flags[i]);
  return out;
}

const analysis::LockRow* ScopeLaneSet::lockRow(uint64_t address) const {
  if (!locks) return nullptr;
  const auto it = lockByAddress.find(address);
  return it == lockByAddress.end() ? nullptr : &locks->locks[it->second];
}

const std::vector<uint32_t>& ScopeLaneSet::lanesFor(ThreadIdx thread) const {
  static const std::vector<uint32_t> kNone;
  return thread.valid() && thread.get() < lanesOfThread.size() ? lanesOfThread[thread.get()] : kNone;
}

std::vector<LaneLine> ScopeLaneSet::lines(ThreadIdx thread, bool expanded) const {
  std::vector<LaneLine> out;
  for (const uint32_t li : lanesFor(thread)) {
    const size_t depths = lanes[li].depths.size();
    const size_t n = std::min<size_t>(depths, expanded ? kMaxLinesPerLane : 1);
    for (uint32_t d = 0; d < n; ++d) {
      if (out.size() >= kMaxLinesPerRow) return out;
      out.push_back({li, d});
    }
  }
  return out;
}

uint32_t ScopeLaneSet::lineCount(ThreadIdx thread, bool expanded) const {
  uint32_t n = 0;
  for (const uint32_t li : lanesFor(thread)) {
    n += static_cast<uint32_t>(std::min<size_t>(lanes[li].depths.size(), expanded ? kMaxLinesPerLane : 1));
    if (n >= kMaxLinesPerRow) return kMaxLinesPerRow;
  }
  return n;
}

std::shared_ptr<ScopeLaneSet> buildScopeLanes(std::shared_ptr<const analysis::PairedScopes> paired,
                                              size_t threadCount) {
  auto set = std::make_shared<ScopeLaneSet>();
  set->plain = paired;
  for (size_t t = 0; t < paired->byThread.size(); ++t) {
    const auto& scopes = paired->byThread[t];
    if (scopes.empty()) continue;
    ScopeLane lane;
    lane.thread = ThreadIdx(static_cast<uint32_t>(t));
    for (const analysis::ScopeInstance& s : scopes) addInterval(lane, &s, s.begin, s.end, set->droppedDeep);
    if (lane.intervalCount > 0) set->lanes.push_back(std::move(lane));
  }
  finishSet(*set, std::max(threadCount, paired->byThread.size()));
  return set;
}

std::shared_ptr<ScopeLaneSet> buildScopeLanes(std::shared_ptr<const analysis::PairedFibers> paired,
                                              size_t threadCount) {
  if (!paired->fiberAware) {
    auto set =
        buildScopeLanes(std::shared_ptr<const analysis::PairedScopes>(paired, &paired->scopes), threadCount);
    set->fibers = paired;
    return set;
  }
  auto set = std::make_shared<ScopeLaneSet>();
  set->fibers = paired;
  set->grouped = true;
  std::unordered_map<uint64_t, uint32_t> laneOf;  // (fiber lane index, thread) -> index in set->lanes
  for (size_t fl = 0; fl < paired->lanes.size(); ++fl) {
    const analysis::FiberLane& src = paired->lanes[fl];
    for (const analysis::ScopePiece& p : src.pieces) {
      if (p.scope >= src.scopes.size() || !p.thread.valid()) continue;
      const uint64_t key = (static_cast<uint64_t>(fl) << 32) | p.thread.get();
      auto [it, inserted] = laneOf.emplace(key, static_cast<uint32_t>(set->lanes.size()));
      if (inserted) {
        set->lanes.emplace_back();
        set->lanes.back().fiberId = src.fiberId;
        set->lanes.back().thread = p.thread;
      }
      addInterval(set->lanes[it->second], &src.scopes[p.scope], p.begin, p.end, set->droppedDeep);
    }
  }
  size_t n = threadCount;
  for (const ScopeLane& l : set->lanes) n = std::max<size_t>(n, l.thread.get() + 1);
  finishSet(*set, n);
  return set;
}

// ---------------------------------------------------------------------------------------------------------
// Item layout

bool layoutTrack(const ScopeTrack& track, const PlotArea& area, size_t maxItems, std::vector<LaneItem>& out) {
  const size_t n = track.size();
  if (n == 0) return true;
  const double scale = area.view.span > 0.0 ? static_cast<double>(area.width) / area.view.span : 0.0;
  if (!(scale > 0.0)) return true;
  const double base = static_cast<double>(area.limits.traceStart);
  const auto xOf = [&](uint64_t t) {
    return static_cast<double>(area.x0) + ((static_cast<double>(t) - base) - area.view.start) * scale;
  };
  const double xLeft = area.x0, xRight = static_cast<double>(area.x0) + area.width;
  const double tA = base + area.view.start, tB = base + area.view.end();
  const uint64_t tAFloor = tA > 0.0 ? static_cast<uint64_t>(std::floor(tA)) : 0;

  size_t pos =
      static_cast<size_t>(std::lower_bound(track.end.begin(), track.end.end(), tAFloor) - track.end.begin());
  const size_t startSize = out.size();
  bool prevMerged = false;
  while (pos < n) {
    const uint64_t b = track.begin[pos];
    if (static_cast<double>(b) >= tB) break;
    if (out.size() - startSize >= maxItems) return false;
    const double xa = xOf(b), xb = xOf(track.end[pos]);
    if (xb - xa >= kMinItemWidth) {
      LaneItem it;
      it.x0 = static_cast<float>(std::max(xa, xLeft));
      it.x1 = static_cast<float>(std::min(xb, xRight));
      if (it.x1 < it.x0 + kMinItemWidth) it.x1 = it.x0 + kMinItemWidth;
      it.first = static_cast<uint32_t>(pos);
      it.count = 1;
      out.push_back(it);
      prevMerged = false;
      ++pos;
      continue;
    }
    // Sub-pixel interval: everything that begins in the same pixel merges into one item.
    const double px = std::floor(xa);
    const double edgeTick = tA + (px + 1.0 - area.x0) / scale;
    const uint64_t edge = edgeTick > 0.0 ? static_cast<uint64_t>(std::ceil(edgeTick)) : 0;
    const size_t q0 = firstAtOrAfter(track.begin, pos + 1, edge);
    size_t q = std::max(q0, pos + 1);
    if (q - pos > 1) {
      // Only the last of the group can be wide (the intervals are disjoint): it is its own item.
      const size_t last = q - 1;
      if (xOf(track.end[last]) - xOf(track.begin[last]) >= kMinItemWidth) q = last;
    }
    const double xe = xOf(track.end[q - 1]);
    float ix0 = static_cast<float>(std::max(xa, xLeft));
    float ix1 = static_cast<float>(std::min(std::max(xe, xa + kMinItemWidth), xRight));
    if (ix1 < ix0 + kMinItemWidth) ix1 = ix0 + kMinItemWidth;
    const uint32_t count = static_cast<uint32_t>(q - pos);
    if (prevMerged && out.size() > startSize && out.back().x1 >= ix0 - 0.5f) {
      out.back().count += count;
      out.back().x1 = std::max(out.back().x1, ix1);
    } else {
      LaneItem it;
      it.x0 = ix0;
      it.x1 = ix1;
      it.first = static_cast<uint32_t>(pos);
      it.count = count;
      out.push_back(it);
    }
    prevMerged = true;
    pos = q;
  }
  return true;
}

std::optional<TrackHit> hitTrack(const ScopeTrack& track, const PlotArea& area, float x, float tolerancePx) {
  const size_t n = track.size();
  if (n == 0 || !(area.view.span > 0.0)) return std::nullopt;
  const double base = static_cast<double>(area.limits.traceStart);
  const double ts = base + xToRelTick(area.view, x, area.x0, area.width);
  const double tol = static_cast<double>(std::max(tolerancePx, 0.0f)) * area.ticksPerPixel();
  const auto toTick = [](double v) { return v > 0.0 ? static_cast<uint64_t>(std::floor(v)) : uint64_t{0}; };
  const uint64_t lo = toTick(ts - tol), hi = toTick(ts + tol);
  const size_t pos0 =
      static_cast<size_t>(std::lower_bound(track.end.begin(), track.end.end(), lo) - track.end.begin());
  const size_t pos1 =
      static_cast<size_t>(std::upper_bound(track.begin.begin(), track.begin.end(), hi) - track.begin.begin());
  if (pos0 >= pos1) return std::nullopt;
  // An interval that contains the cursor wins over its neighbours within the tolerance.
  const uint64_t tsTick = toTick(ts);
  const size_t after = static_cast<size_t>(std::upper_bound(track.begin.begin(), track.begin.end(), tsTick) -
                                           track.begin.begin());
  if (after > 0) {
    const size_t c = after - 1;
    if (c >= pos0 && c < pos1 && track.end[c] > tsTick) return TrackHit{static_cast<uint32_t>(c), 1};
  }
  return TrackHit{static_cast<uint32_t>(pos0), static_cast<uint32_t>(pos1 - pos0)};
}

// ---------------------------------------------------------------------------------------------------------
// Colours, labels, tooltips

Color32 scopeColor(uint32_t rgba, StringIdx id) {
  if (rgba == analysis::kLockColorSentinel) return kLockScopeColor;
  if (rgba == 0xFFFFFFFFu || rgba == 0) return hashColor(0x5C09E000ULL + id.get());
  return makeColor(static_cast<uint8_t>(rgba >> 24), static_cast<uint8_t>(rgba >> 16),
                   static_cast<uint8_t>(rgba >> 8));
}

std::string scopeLabel(std::string_view id, std::string_view data) {
  std::string out(id);
  if (!data.empty()) {
    out += " [";
    out += data;
    out += ']';
  }
  return out;
}

std::string clipLabel(std::string_view text, float widthPx, float charPx) {
  if (!(widthPx > 0.0f) || !(charPx > 0.0f)) return {};
  const size_t maxChars = static_cast<size_t>(widthPx / charPx);
  // Byte offset after the first `n` UTF-8 characters (or text.size()).
  const auto offsetAfter = [&](size_t n) {
    size_t i = 0, chars = 0;
    while (i < text.size() && chars < n) {
      ++i;
      while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
      ++chars;
    }
    return i;
  };
  if (offsetAfter(maxChars) == text.size()) return std::string(text);  // everything fits
  if (maxChars < 3) return {};
  std::string out(text.substr(0, offsetAfter(maxChars - 2)));
  out += "..";
  return out;
}

std::vector<std::string> scopeTooltipLines(const ScopeTooltipInput& in) {
  std::vector<std::string> lines;
  if (!in.model || !in.lane || !in.track || in.hit.first >= in.track->size()) return lines;
  const model::TraceModel& model = *in.model;
  char buf[256];
  const analysis::ScopeInstance& inst = *in.track->instance[in.hit.first];
  const std::string_view id = model.strings.get(inst.id);
  if (in.hit.count > 1) {
    std::snprintf(buf, sizeof buf, "%u scopes (merged, zoom in to separate them)", in.hit.count);
    lines.push_back(buf);
    uint64_t covered = 0;
    const size_t end = std::min<size_t>(in.track->size(), static_cast<size_t>(in.hit.first) + in.hit.count);
    for (size_t i = in.hit.first; i < end && i < static_cast<size_t>(in.hit.first) + 100000; ++i) {
      covered += in.track->end[i] - in.track->begin[i];
    }
    std::snprintf(buf, sizeof buf, "first: %.*s  (covering %s)", static_cast<int>(id.size()), id.data(),
                  formatDurationTicks(covered, in.clockHz).c_str());
    lines.push_back(buf);
    if (inst.depth > 0 || in.lane->fiberId != 0) {
      std::snprintf(buf, sizeof buf, "depth %u", inst.depth);
      lines.push_back(buf);
    }
    return lines;
  }
  const bool lock = inst.color == analysis::kLockColorSentinel;
  lines.push_back(std::string(lock ? "Lock wait: " : "Scope: ") + std::string(id));
  const std::string_view data = model.strings.get(inst.data);
  if (!data.empty()) lines.push_back("data: " + std::string(data));
  const uint64_t wall = inst.end - inst.begin;
  std::snprintf(buf, sizeof buf, "duration: %s", formatDurationTicks(wall, in.clockHz).c_str());
  lines.push_back(buf);
  const uint64_t tb = in.track->begin[in.hit.first], te = in.track->end[in.hit.first];
  if (tb != inst.begin || te != inst.end) {
    std::snprintf(buf, sizeof buf, "this piece: %s of %s (the fiber was switched out in between)",
                  formatDurationTicks(te - tb, in.clockHz).c_str(),
                  formatDurationTicks(wall, in.clockHz).c_str());
    lines.push_back(buf);
  }
  std::snprintf(buf, sizeof buf, "start: +%s",
                formatTimeNs(static_cast<double>(inst.begin - std::min(inst.begin, model.header.start_ts)) *
                                 1e9 / std::max(in.clockHz, 1.0),
                             1.0)
                    .c_str());
  lines.push_back(buf);
  std::snprintf(buf, sizeof buf, "depth: %u", inst.depth);
  lines.push_back(buf);
  if (in.lane->thread.valid() && in.lane->thread.get() < model.threads.size()) {
    const model::Thread& th = model.threads[in.lane->thread.get()];
    std::snprintf(buf, sizeof buf, "thread: %s (tid %u)", th.name.empty() ? "(unnamed)" : th.name.c_str(),
                  th.tid);
    lines.push_back(buf);
  }
  if (in.lane->fiberId != 0) {
    std::snprintf(buf, sizeof buf, "fiber: 0x%llx", static_cast<unsigned long long>(in.lane->fiberId));
    lines.push_back(buf);
  }
  if (inst.unterminated)
    lines.push_back("never ended: drawn to the end of the trace (or the fiber's unregister)");
  if (lock) {
    uint64_t address = 0;
    if (analysis::parseLockAddress(data, address)) {
      std::snprintf(buf, sizeof buf, "lock address: 0x%llx", static_cast<unsigned long long>(address));
      lines.push_back(buf);
      if (const analysis::LockRow* row = in.set ? in.set->lockRow(address) : nullptr) {
        std::snprintf(buf, sizeof buf, "contention: %u waits, total %s, max %s", row->contentionCount,
                      formatDurationTicks(row->totalWaitTicks, in.clockHz).c_str(),
                      formatDurationTicks(row->maxWaitTicks, in.clockHz).c_str());
        lines.push_back(buf);
        const size_t shown = std::min<size_t>(row->topThreads.size(), 3);
        for (size_t i = 0; i < shown; ++i) {
          const analysis::LockWaiter& w = row->topThreads[i];
          const std::string name = w.thread.valid() && w.thread.get() < model.threads.size()
                                       ? model.threads[w.thread.get()].name
                                       : std::string();
          std::snprintf(buf, sizeof buf, "  waiter: %s  %s (%u waits)",
                        name.empty() ? "(thread)" : name.c_str(),
                        formatDurationTicks(w.waitTicks, in.clockHz).c_str(), w.waits);
          lines.push_back(buf);
        }
      }
    } else {
      lines.push_back("lock address: unknown (no hex address in the data)");
    }
  }
  return lines;
}

// ---------------------------------------------------------------------------------------------------------
// The view

void ScopeLanesView::reset() {
  m_query.cancel();
  m_set.reset();
  m_shownGrouped = false;
  m_haveRequested = false;
  m_requestedGrouped = false;
  m_requestedDocument = 0;
  m_error.clear();
  m_highlightId.reset();
  m_stats = {};
  m_items.clear();
  m_candidates.clear();
  m_candidatesDocument = ~0ull;
}

void ScopeLanesView::inject(std::shared_ptr<const ScopeLaneSet> set, bool grouped) {
  m_set = std::move(set);
  m_shownGrouped = grouped;
}

void ScopeLanesView::update(const PanelContext& context, bool grouped) {
  if (!context.document || !context.queryExecutor) return;
  if (auto result = m_query.poll()) {
    if (result->ok()) {
      m_set = result->value();
      m_shownGrouped = m_requestedGrouped;
      m_error.clear();
    } else {
      m_error = result->error().toString();
    }
    context.requestRedraw();
  }
  if (m_haveRequested && m_requestedGrouped == grouped && m_requestedDocument == context.documentGeneration) {
    if (m_query.pending()) context.requestRedraw();
    return;
  }
  const Document& doc = *context.document;
  if (m_candidatesDocument != context.documentGeneration) {
    // One pass over the raw events (a small direct-mapped cache of tid -> thread keeps it at a few ns each).
    const model::TraceModel& model = doc.model();
    m_candidates.assign(model.threads.size(), 0);
    struct Slot {
      uint32_t tid = 0;
      uint32_t thread = kInvalidIdx;
    };
    std::vector<Slot> cache(4096);
    auto mark = [&](uint32_t tid) {
      Slot& slot = cache[(tid * 2654435761u) >> 20];
      if (slot.thread == kInvalidIdx || slot.tid != tid) {
        const auto it = model.tidToThread.find(tid);
        slot.tid = tid;
        slot.thread = it == model.tidToThread.end() ? kInvalidIdx - 1 : it->second.get();
      }
      if (slot.thread < m_candidates.size()) m_candidates[slot.thread] = 1;
    };
    for (const trace::ScopeEventRec& e : model.scopeEvents) mark(e.tid);
    for (const model::FiberSwitch& e : model.fiberSwitches) {
      if (e.thread.valid() && e.thread.get() < m_candidates.size()) m_candidates[e.thread.get()] = 1;
    }
    m_candidatesDocument = context.documentGeneration;
  }
  analysis::AggregationOptions options;
  common::EffectiveScopeOptions scope = common::effectiveScopeOptions(context);
  scope.groupByFiber = grouped;
  common::applyScopeOptions(options, scope);
  std::shared_ptr<const AnalysisSnapshot> snapshot =
      doc.analysis().snapshot(options, context.symbolGeneration, context.queryExecutor);
  if (!snapshot) {
    m_error = "Analysis is not available for this document.";
    return;
  }
  m_haveRequested = true;
  m_requestedGrouped = grouped;
  m_requestedDocument = context.documentGeneration;
  analysis::QueryExecutor& executor = *context.queryExecutor;
  using Value = std::shared_ptr<const ScopeLaneSet>;
  m_query.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [grouped](const analysis::QueryContext& qc, analysis::Cancel cancel) -> Result<Value> {
          const size_t threads = qc.model().threads.size();
          std::shared_ptr<ScopeLaneSet> set;
          if (grouped) {
            auto paired = analysis::pairedFibersFor(qc, cancel);
            if (!paired.ok()) return paired.error();
            set = buildScopeLanes(*paired, threads);
          } else {
            auto paired = analysis::pairedScopesFor(qc, cancel);
            if (!paired.ok()) return paired.error();
            set = buildScopeLanes(*paired, threads);
          }
          if (cancel.flag && cancel.flag->load(std::memory_order_relaxed)) {
            return makeError(ErrorCode::Cancelled, "cancelled");
          }
          // Lock waits get their contention summary for the tooltip (whole trace); a failure only costs that.
          auto locks = analysis::lockSummaryShared(qc, analysis::Filter{}, cancel);
          if (locks.ok()) {
            set->locks = *locks;
            for (size_t i = 0; i < set->locks->locks.size(); ++i) {
              set->lockByAddress.emplace(set->locks->locks[i].address, static_cast<uint32_t>(i));
            }
          }
          return Value(std::move(set));
        });
  });
}

uint32_t ScopeLanesView::linesFor(ThreadIdx thread, bool expanded) const {
  if (!m_set) return 1;  // the pending look
  return m_set->lineCount(thread, expanded);
}

bool ScopeLanesView::mayHaveScopes(ThreadIdx thread) const {
  if (m_set) return !m_set->lanesFor(thread).empty();
  // Pending: only the threads that have scope or fiber events can get lanes. Before the candidates are known
  // (no update() yet) every row may.
  if (m_candidatesDocument == ~0ull) return true;
  return thread.valid() && thread.get() < m_candidates.size() && m_candidates[thread.get()] != 0;
}

void ScopeLanesView::beginFrame() {
  m_stats = {};
  m_items.clear();
  m_labelBudget = 600;
}

void ScopeLanesView::drawRow(ImDrawList* dl, const model::TraceModel& model, const RowLayout& row,
                             const RowGeometry& geom, float rowTop, const PlotArea& area, float gutterX0) {
  if (row.scopeLines == 0) return;
  ++m_stats.rows;
  const float x0 = area.x0, x1 = area.x0 + area.width;
  if (!m_set) {
    const float y0 = rowTop + scopeLineTop(row, geom, 0);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + geom.scopeLineHeight - 1.0f),
                      IM_COL32(44, 44, 52, 255));
    return;
  }
  const std::vector<LaneLine> lines = m_set->lines(row.thread, row.expanded);
  const float fontSize = ImGui::GetFontSize();
  const float charPx = std::max(ImGui::CalcTextSize("0").x, 1.0f);
  ImFont* font = ImGui::GetFont();
  const size_t maxItems = static_cast<size_t>(area.width) + 8;
  const uint32_t shown = std::min<uint32_t>(row.scopeLines, static_cast<uint32_t>(lines.size()));
  const ImU32 stripe = IM_COL32(20, 20, 24, 255);
  const float labelMinWidth = charPx * 3.0f;
  for (uint32_t li = 0; li < shown; ++li) {
    const LaneLine ll = lines[li];
    const ScopeLane& lane = m_set->lanes[ll.lane];
    if (ll.depth >= lane.depths.size()) continue;
    const ScopeTrack& track = lane.depths[ll.depth];
    const float y0 = rowTop + scopeLineTop(row, geom, li);
    const float y1 = y0 + geom.scopeLineHeight - 1.0f;
    ++m_stats.lines;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), stripe);
    if (ll.depth == 0) {  // gutter label of the lane
      char label[64];
      if (lane.fiberId != 0) {
        std::snprintf(label, sizeof label, "  fiber 0x%llx", static_cast<unsigned long long>(lane.fiberId));
      } else {
        std::snprintf(label, sizeof label, m_set->grouped ? "  thread" : "  scopes");
      }
      const ImVec4 clip(gutterX0, y0, x0 - 4.0f, y1);
      dl->AddText(font, fontSize * 0.9f, ImVec2(gutterX0 + 4.0f, y0), IM_COL32(150, 190, 220, 255), label,
                  nullptr, 0.0f, &clip);
    }
    m_scratch.clear();
    layoutTrack(track, area, maxItems, m_scratch);
    m_decorated.clear();
    // Pass 1: every rectangle in one batch, from the compact track columns only. Items that need more (flags,
    // a label, the selection outline, the test record) are noted for pass 2.
    {
      common::RectBatch batch(dl, m_scratch.size());
      for (const LaneItem& it : m_scratch) {
        const bool merged = it.count > 1;
        Color32 c = track.fillOf(it.first);
        if (merged) c = scaleBrightness(c, 0.75f);
        batch.add(it.x0, y0, it.x1, y1, toIm(c));
        ++m_stats.items;
        m_stats.merged += merged ? 1 : 0;
        if (m_record || track.flagsOver(it.first, it.count) != 0 || (!merged && it.x1 - it.x0 > labelMinWidth) ||
            (m_highlightId && !merged)) {
          m_decorated.push_back(it);
        }
      }
    }
    // Pass 2: decorations and labels.
    for (const LaneItem& it : m_decorated) {
      const analysis::ScopeInstance& inst = *track.instance[it.first];
      const bool merged = it.count > 1;
      const uint8_t flags = track.flagsOver(it.first, it.count);
      const bool lock = (flags & kTrackLock) != 0;
      const float w = it.x1 - it.x0;
      DrawnScopeItem drawn;
      if (m_record) {
        drawn.thread = row.thread;
        drawn.lane = ll.lane;
        drawn.line = li;
        drawn.depth = ll.depth;
        drawn.x0 = it.x0;
        drawn.x1 = it.x1;
        drawn.y0 = y0;
        drawn.y1 = y1;
        drawn.first = it.first;
        drawn.count = it.count;
        drawn.lock = lock;
        drawn.unterminated = (flags & kTrackOpen) != 0;
        drawn.id = inst.id;
      }
      if (lock) {
        if (w >= 6.0f && m_stats.lockHatches < 400) {
          ++m_stats.lockHatches;
          dl->PushClipRect(ImVec2(it.x0, y0), ImVec2(it.x1, y1), true);
          const float h = y1 - y0;
          for (float x = it.x0 - h; x < it.x1; x += 6.0f) {
            dl->AddLine(ImVec2(x, y1), ImVec2(x + h, y0), IM_COL32(255, 220, 200, 120));
          }
          dl->PopClipRect();
        } else if (w < 6.0f) {
          dl->AddLine(ImVec2(it.x0, y0), ImVec2(it.x1, y0), IM_COL32(255, 220, 200, 255));
        }
      }
      if ((flags & kTrackOpen) != 0 && it.x1 < x1 - 1.0f) {
        ++m_stats.openEnds;
        dl->AddTriangleFilled(ImVec2(it.x1 - 5.0f, y0), ImVec2(it.x1, (y0 + y1) * 0.5f),
                              ImVec2(it.x1 - 5.0f, y1), IM_COL32(255, 255, 255, 230));
      }
      if (!merged && m_highlightId && *m_highlightId == inst.id) {
        dl->AddRect(ImVec2(it.x0, y0), ImVec2(it.x1, y1), IM_COL32(255, 255, 255, 255));
      }
      if (!merged && w > labelMinWidth && m_labelBudget > 0) {
        const std::string text =
            clipLabel(scopeLabel(model.strings.get(inst.id), model.strings.get(inst.data)), w - 4.0f, charPx);
        if (!text.empty()) {
          --m_labelBudget;
          ++m_stats.labels;
          drawn.labelled = true;
          const ImVec4 clip(it.x0, y0, it.x1, y1);
          dl->AddText(font, fontSize, ImVec2(it.x0 + 2.0f, y0), toIm(textColorFor(track.fillOf(it.first))),
                      text.c_str(), nullptr, 0.0f, &clip);
        }
      }
      if (m_record && m_items.size() < kMaxDrawnItems) m_items.push_back(drawn);
    }
  }
}

std::optional<ScopeHit> ScopeLanesView::hit(const RowLayout& row, uint32_t line, const PlotArea& area,
                                            float x) const {
  if (!m_set) return std::nullopt;
  const std::vector<LaneLine> lines = m_set->lines(row.thread, row.expanded);
  if (line >= lines.size() || line >= row.scopeLines) return std::nullopt;
  const LaneLine ll = lines[line];
  const ScopeLane& lane = m_set->lanes[ll.lane];
  if (ll.depth >= lane.depths.size()) return std::nullopt;
  const auto h = hitTrack(lane.depths[ll.depth], area, x, 2.0f);
  if (!h) return std::nullopt;
  ScopeHit out;
  out.laneIndex = ll.lane;
  out.depth = ll.depth;
  out.track = *h;
  out.lane = &lane;
  out.trackPtr = &lane.depths[ll.depth];
  return out;
}

}  // namespace tachyon::viewer::timeline
