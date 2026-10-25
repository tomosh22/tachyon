#include "viewer/common/query_filter.h"

namespace tachyon::viewer::common {

analysis::Filter toQueryFilter(const std::optional<ViewFilter>& filter) {
  analysis::Filter f;  // [0, UINT64_MAX), all threads
  if (!filter) return f;
  f.t0 = filter->t0;
  f.t1 = filter->t1;
  f.threads = filter->threads;
  f.subtreeRoot = filter->subtreeRoot;
  return f;
}

double ticksToMs(uint64_t ticks, double clockHz) {
  return clockHz > 0.0 ? static_cast<double>(ticks) * 1000.0 / clockHz : 0.0;
}

}  // namespace tachyon::viewer::common
