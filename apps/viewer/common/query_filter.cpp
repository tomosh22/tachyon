#include "viewer/common/query_filter.h"

namespace tachyon::viewer::common {

analysis::Filter toQueryFilter(const std::optional<ViewFilter>& filter) {
  analysis::Filter f;  // [0, UINT64_MAX), all threads
  if (!filter) return f;
  f.t0 = filter->t0;
  f.t1 = filter->t1;
  f.threads = filter->threads;
  f.subtreeRoot = filter->subtreeRoot;
  f.subtreePath = filter->subtreePath;
  return f;
}

void applySubtreeOptions(const std::optional<ViewFilter>& filter, bool& expandInline,
                         analysis::KernelFrames& kernelFrames, bool& blendScopes, bool& groupByFiber) {
  if (!filter || !filter->subtreePath || !filter->subtreeOptions) return;
  expandInline = filter->subtreeOptions->expandInline;
  kernelFrames = filter->subtreeOptions->kernelFrames;
  blendScopes = filter->subtreeOptions->blendScopes;
  groupByFiber = filter->subtreeOptions->groupByFiber;
}

bool hasSubtreeRestriction(const std::optional<ViewFilter>& filter) {
  return filter && (filter->subtreeRoot || filter->subtreePath);
}

void clearSubtree(analysis::Filter& filter) {
  filter.subtreeRoot.reset();
  filter.subtreePath.reset();
}

void clearSubtree(ViewFilter& filter) {
  filter.subtreeRoot.reset();
  filter.subtreePath.reset();
  filter.subtreeOptions.reset();
  filter.subtreeLabel.clear();
}

std::string subtreeText(const ViewFilter& filter) {
  if (filter.subtreePath) {
    if (!filter.subtreeLabel.empty()) return filter.subtreeLabel;
    return "path of " + std::to_string(filter.subtreePath->size()) + " frames";
  }
  if (filter.subtreeRoot) return "node #" + std::to_string(filter.subtreeRoot->get());
  return {};
}

double ticksToMs(uint64_t ticks, double clockHz) {
  return clockHz > 0.0 ? static_cast<double>(ticks) * 1000.0 / clockHz : 0.0;
}

}  // namespace tachyon::viewer::common
