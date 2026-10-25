#include "timeline.h"

#include <algorithm>

namespace tachyon::tracegen {

Timeline::Timeline(const Scenario& scenario, const ThreadSpec& thread)
    : m_thread(thread), m_rng(streamSeed(scenario.seed, "timeline", thread.tid)) {
  m_t = thread.fromStart ? 0 : thread.startOffset;
  m_end = scenario.duration;
  if (!thread.intervals.empty()) m_nextIsRun = thread.intervals.back().state != SegState::Run;
}

bool Timeline::produce(Segment& seg) {
  seg = Segment{};
  if (m_idx < m_thread.intervals.size()) {
    const IntervalSpec& iv = m_thread.intervals[m_idx];
    seg.fromStart = (m_idx == 0 && m_thread.fromStart);
    ++m_idx;
    seg.state = iv.state;
    seg.start = m_t;
    seg.end = iv.toEnd ? m_end : m_t + iv.duration;
    seg.waitReason = iv.waitReason;
    seg.waitMode = iv.waitMode;
    seg.priority = iv.priority;
    seg.blockingStack = iv.blockingStack;
    seg.readyAt = iv.readyAfter ? m_t + iv.readyAfter : 0;
    seg.wakerTid = iv.wakerTid;
    seg.wakerStack = iv.wakerStack;
    seg.mix = iv.mixOverride;
  } else if (m_thread.pattern.present) {
    const PatternSpec& p = m_thread.pattern;
    seg.start = m_t;
    if (m_nextIsRun) {
      seg.state = SegState::Run;
      seg.end = m_t + m_rng.range(p.runLo, p.runHi);
    } else {
      const uint64_t dur = m_rng.range(p.waitLo, p.waitHi);
      seg.end = m_t + dur;
      seg.priority = static_cast<uint8_t>(p.priority);
      if (!p.blockingStacks.empty())
        seg.blockingStack = p.blockingStacks[m_rng.below(p.blockingStacks.size())];
      if (m_rng.below(100) < p.preemptPct) {
        seg.state = SegState::Ready;
      } else {
        seg.state = SegState::Wait;
        seg.waitReason = static_cast<uint8_t>(p.waitReasons[m_rng.below(p.waitReasons.size())]);
        if (m_rng.below(100) < p.readyPct && dur >= 3 && !p.wakerTids.empty()) {
          const uint64_t latency = std::clamp<uint64_t>(m_rng.range(p.latencyLo, p.latencyHi), 1, dur - 1);
          seg.readyAt = seg.end - latency;
          seg.wakerTid = p.wakerTids[m_rng.below(p.wakerTids.size())];
          seg.wakerStack = p.wakerStack;
        }
      }
    }
    m_nextIsRun = !m_nextIsRun;
  } else {
    return false;
  }
  if (seg.end <= seg.start) seg.end = seg.start + 1;
  m_t = seg.end;
  if (seg.start >= m_end) return false;
  if (seg.end > m_end) seg.end = m_end;
  if (seg.readyAt != 0 && (seg.readyAt <= seg.start || seg.readyAt >= seg.end)) seg.readyAt = 0;
  return true;
}

bool Timeline::next(Segment& out) {
  if (m_done) return false;
  if (!m_have) {
    if (!produce(m_cur)) {
      m_done = true;
      return false;
    }
    m_have = true;
  }
  Segment nxt;
  out = m_cur;
  if (produce(nxt)) {
    m_cur = nxt;
  } else {
    out.end = m_end;  // the last segment lasts until the end of the trace
    m_done = true;
  }
  return true;
}

}  // namespace tachyon::tracegen
