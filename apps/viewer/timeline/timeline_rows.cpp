#include "viewer/timeline/timeline_rows.h"

#include <algorithm>
#include <limits>

namespace tachyon::viewer::timeline {

namespace {

bool lessByKey(const ThreadSortInfo& a, const ThreadSortInfo& b, SortKey key, bool reverse) {
  const auto cmp = [&](auto x, auto y) -> int { return x < y ? -1 : (y < x ? 1 : 0); };
  int c = 0;
  switch (key) {
    case SortKey::RunningTime:
      c = -cmp(a.runningTicks, b.runningTicks);  // "most running first" is the natural order
      break;
    case SortKey::SampleCount: c = -cmp(a.sampleCount, b.sampleCount); break;
    case SortKey::Name: c = a.name.compare(b.name) < 0 ? -1 : (a.name.compare(b.name) > 0 ? 1 : 0); break;
    case SortKey::ThreadId: c = cmp(a.tid, b.tid); break;
    case SortKey::Natural: c = 0; break;
  }
  if (reverse) c = -c;
  if (c != 0) return c < 0;
  return a.thread.get() < b.thread.get();
}

}  // namespace

std::vector<ThreadIdx> orderRows(const std::vector<ThreadSortInfo>& infos, const std::vector<RowFlags>& flags,
                                 SortKey key, bool reverse) {
  const auto flagOf = [&](const ThreadSortInfo& i) {
    return i.thread.get() < flags.size() ? flags[i.thread.get()] : RowFlags{};
  };
  std::vector<const ThreadSortInfo*> pinned, normal;
  for (const ThreadSortInfo& i : infos) {
    const RowFlags f = flagOf(i);
    if (f.hidden) continue;
    (f.pinned ? pinned : normal).push_back(&i);
  }
  const auto less = [&](const ThreadSortInfo* a, const ThreadSortInfo* b) {
    return lessByKey(*a, *b, key, reverse);
  };
  std::sort(pinned.begin(), pinned.end(), less);
  std::sort(normal.begin(), normal.end(), less);
  std::vector<ThreadIdx> out;
  out.reserve(pinned.size() + normal.size());
  for (const auto* i : pinned) out.push_back(i->thread);
  for (const auto* i : normal) out.push_back(i->thread);
  return out;
}

float bandTop(const RowGeometry& geom) {
  return geom.rowPadding;
}
float stripTop(const RowGeometry& geom) {
  return geom.rowPadding + geom.bandHeight;
}
float flameTop(const RowGeometry& geom) {
  return geom.rowPadding + geom.bandHeight + geom.stripHeight + geom.flameGap;
}
float flameDepthTop(const RowGeometry& geom, uint32_t depth) {
  return flameTop(geom) + static_cast<float>(depth > 0 ? depth - 1 : 0) * geom.flameDepthHeight;
}

std::vector<RowLayout> layoutRows(const std::vector<ThreadIdx>& order, const std::vector<RowFlags>& flags,
                                  const std::vector<uint32_t>& flameDepthsByThread, const RowGeometry& geom) {
  std::vector<RowLayout> out;
  out.reserve(order.size());
  float y = 0;
  for (const ThreadIdx t : order) {
    RowLayout r;
    r.thread = t;
    r.y0 = y;
    r.expanded = t.get() < flags.size() && flags[t.get()].expanded;
    r.flameDepths = t.get() < flameDepthsByThread.size() ? flameDepthsByThread[t.get()] : 0;
    r.height = geom.rowPadding * 2 + geom.bandHeight + geom.stripHeight;
    if (r.expanded && r.flameDepths > 0) {
      r.height += geom.flameGap + static_cast<float>(r.flameDepths) * geom.flameDepthHeight;
    }
    y += r.height + geom.rowSpacing;
    out.push_back(r);
  }
  return out;
}

float contentHeight(const std::vector<RowLayout>& layout, const RowGeometry& geom) {
  if (layout.empty()) return 0;
  return layout.back().y0 + layout.back().height + geom.rowSpacing;
}

RowRange visibleRows(const std::vector<RowLayout>& layout, float scrollY, float viewHeight) {
  RowRange r;
  // First row whose bottom is below scrollY.
  size_t lo = 0, hi = layout.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (layout[mid].y0 + layout[mid].height <= scrollY) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  r.first = lo;
  size_t end = lo;
  while (end < layout.size() && layout[end].y0 < scrollY + viewHeight) ++end;
  r.last = end;
  return r;
}

size_t rowAt(const std::vector<RowLayout>& layout, const RowGeometry& geom, float contentY) {
  (void)geom;
  if (layout.empty() || contentY < 0) return std::numeric_limits<size_t>::max();
  size_t lo = 0, hi = layout.size();
  while (lo < hi) {  // last row with y0 <= contentY
    const size_t mid = (lo + hi) / 2;
    if (layout[mid].y0 <= contentY) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return std::numeric_limits<size_t>::max();
  const size_t i = lo - 1;
  if (contentY >= layout[i].y0 + layout[i].height + geom.rowSpacing)
    return std::numeric_limits<size_t>::max();
  return i;
}

RowHit hitRowSection(const RowLayout& row, const RowGeometry& geom, float yInRow) {
  RowHit hit;
  if (yInRow < 0 || yInRow >= row.height) return hit;
  if (yInRow < stripTop(geom)) {
    hit.section = RowSection::Band;  // includes the top padding: the band is the primary target
  } else if (yInRow < stripTop(geom) + geom.stripHeight + (row.expanded ? 0.0f : geom.rowPadding)) {
    hit.section = RowSection::Strip;
  } else if (row.expanded && row.flameDepths > 0 && yInRow >= flameTop(geom)) {
    const float dy = yInRow - flameTop(geom);
    const uint32_t depth = 1 + static_cast<uint32_t>(dy / geom.flameDepthHeight);
    if (depth <= row.flameDepths) {
      hit.section = RowSection::Flame;
      hit.depth = depth;
    }
  } else if (row.expanded) {
    hit.section = RowSection::Strip;  // the gap between strip and flame chart
  }
  return hit;
}

const analysis::TimelineSpan* findSpanAt(const std::vector<analysis::TimelineSpan>& spans, uint32_t depth,
                                         uint64_t ts) {
  // Spans are sorted by (depth, t0) and never overlap within a depth.
  const auto it =
      std::upper_bound(spans.begin(), spans.end(), std::pair<uint32_t, uint64_t>(depth, ts),
                       [](const std::pair<uint32_t, uint64_t>& key, const analysis::TimelineSpan& s) {
                         return key.first != s.depth ? key.first < s.depth : key.second < s.t0;
                       });
  if (it == spans.begin()) return nullptr;
  const analysis::TimelineSpan& s = *(it - 1);
  if (s.depth == depth && ts >= s.t0 && ts < s.t1) return &s;
  return nullptr;
}

uint32_t columnForTick(uint64_t t0, uint64_t t1, uint32_t columns, uint64_t ts) {
  if (columns == 0 || t1 <= t0) return 0;
  if (ts <= t0) return 0;
  if (ts >= t1) return columns - 1;
  // Inverse of pixelEdge(c) = t0 + floor(c * (t1 - t0) / P): the largest c with edge(c) <= ts.
  const uint64_t num = (ts - t0) * columns;  // span <= 2^47 and columns <= 2^16: no overflow
  uint32_t c = static_cast<uint32_t>(num / (t1 - t0));
  c = std::min(c, columns - 1);
  while (c + 1 < columns && analysis::pixelEdge(t0, t1, columns, c + 1) <= ts) ++c;
  while (c > 0 && analysis::pixelEdge(t0, t1, columns, c) > ts) --c;
  return c;
}

}  // namespace tachyon::viewer::timeline
