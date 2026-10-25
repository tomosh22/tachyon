#include "viewer/stats/instance_index.h"

#include <algorithm>
#include <cmath>

namespace tachyon::viewer::stats {

namespace {
const std::vector<analysis::Instance> kNoInstances;
}  // namespace

InstanceIndex::InstanceIndex(std::shared_ptr<const std::vector<analysis::Instance>> instances,
                             size_t threadCount)
    : m_instances(std::move(instances)), m_rows(threadCount) {
  if (!m_instances) return;
  const std::vector<analysis::Instance>& all = *m_instances;
  for (uint32_t i = 0; i < all.size(); ++i) {
    const ThreadIdx t = all[i].thread;
    if (!t.valid() || t.get() >= threadCount) continue;
    Row& r = m_rows[t.get()];
    r.index.push_back(i);
    r.begin.push_back(all[i].begin);
    r.end.push_back(all[i].end);
    r.maxEnd.push_back(r.maxEnd.empty() ? all[i].end : std::max(r.maxEnd.back(), all[i].end));
  }
}

const std::vector<analysis::Instance>& InstanceIndex::instances() const {
  return m_instances ? *m_instances : kNoInstances;
}

size_t InstanceIndex::countOn(ThreadIdx thread) const {
  return thread.valid() && thread.get() < m_rows.size() ? m_rows[thread.get()].index.size() : 0;
}

size_t InstanceIndex::countOverlapping(ThreadIdx thread, uint64_t t0, uint64_t t1) const {
  if (!thread.valid() || thread.get() >= m_rows.size()) return 0;
  size_t n = 0;
  const Row& row = m_rows[thread.get()];
  for (size_t i = 0; i < row.index.size(); ++i)
    if (row.end[i] >= t0 && row.begin[i] <= t1) ++n;
  return n;
}

bool InstanceIndex::rectsFor(ThreadIdx thread, const timeline::TimeView& view,
                             const timeline::TimeLimits& limits, float x0, float width, size_t maxRects,
                             std::vector<OverlayRect>& out) const {
  if (!thread.valid() || thread.get() >= m_rows.size() || width <= 0.0f) return true;
  const Row& row = m_rows[thread.get()];
  if (row.index.empty()) return true;

  // Visible absolute window.
  const double relStart = std::max(view.start, 0.0);
  const uint64_t vt0 = limits.traceStart + static_cast<uint64_t>(relStart);
  const uint64_t vt1 = limits.traceStart + static_cast<uint64_t>(std::ceil(std::max(view.end(), 0.0)));
  const float plotEnd = x0 + width;

  // First candidate: the first position whose running maximum end reaches the window.
  size_t k =
      static_cast<size_t>(std::lower_bound(row.maxEnd.begin(), row.maxEnd.end(), vt0) - row.maxEnd.begin());
  const size_t n = row.index.size();
  size_t produced = 0;
  float lastX0 = 0, lastX1 = -1;
  bool haveLast = false, lastDense = false;
  while (k < n) {
    const uint64_t begin = row.begin[k], end = row.end[k];
    if (begin > vt1) break;
    if (end < vt0) {
      ++k;
      continue;
    }
    const double a = timeline::relTickToX(view, timeline::toRelative(limits, begin), x0, width);
    const double b = timeline::relTickToX(view, timeline::toRelative(limits, end), x0, width);
    const float xa = static_cast<float>(std::max(a, static_cast<double>(x0)));
    float xb = static_cast<float>(std::min(b, static_cast<double>(plotEnd)));
    const bool dense = (b - a) < 1.0;
    xb = std::max(xb, xa + kMinRectWidth);
    if (dense && haveLast && lastDense && xa <= lastX1 + 1.0f) {
      // A sub-pixel instance next to the previous dense rect: the run grows instead of a new rect starting.
      if (xb > lastX1) out.back().x1 = lastX1 = xb;
      ++out.back().count;
    } else if (!(haveLast && xa >= lastX0 && xb <= lastX1)) {  // not inside the previous rect (nested scope)
      if (produced >= maxRects) return false;
      out.push_back({row.index[k], xa, xb, 1});
      ++produced;
      lastX0 = xa;
      lastX1 = xb;
      haveLast = true;
      lastDense = dense;
    }
    if (dense) {
      // Skip to the first instance that begins in a later cell of kMinRectWidth pixels (a dense instance is
      // drawn that wide anyway): galloping, then binary search, so the cost is logarithmic in the number of
      // instances skipped.
      const double cell = static_cast<double>(kMinRectWidth);
      const double nextX = x0 + (std::floor((std::max(a, static_cast<double>(x0)) - x0) / cell) + 1.0) * cell;
      const double nextRel = timeline::xToRelTick(view, nextX, x0, width);
      const uint64_t nextTick = timeline::toAbsolute(limits, std::max(nextRel, 0.0));
      const auto beginOf = [&](size_t i) { return row.begin[i]; };
      size_t lo = k + 1, step = 1;
      while (lo + step < n && beginOf(lo + step) < nextTick) {
        lo += step;
        step *= 2;
      }
      size_t hi = std::min(lo + step, n);
      // beginOf(lo) is < nextTick or lo == k + 1; the answer lies in [lo, hi].
      while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (beginOf(mid) < nextTick) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      const size_t next = std::max(lo, k + 1);
      // The skipped instances begin in the same cell: the rect stands for them as well.
      if (!out.empty() && next > k + 1) out.back().count += static_cast<uint32_t>(next - k - 1);
      k = next;
    } else {
      ++k;
    }
  }
  return true;
}

}  // namespace tachyon::viewer::stats
