#include "viewer/functions/butterfly_model.h"

#include <algorithm>

namespace tachyon::viewer::functions {

namespace {

Line makeLine(const analysis::FunctionRow& row, uint64_t base, const Namer& namer) {
  Line l;
  l.key = row.key;
  l.inclusiveTicks = row.inclusiveTicks;
  l.exclusiveTicks = row.exclusiveTicks;
  l.instances = row.instances;
  if (base > 0) {
    l.inclusivePercent = 100.0 * static_cast<double>(row.inclusiveTicks) / static_cast<double>(base);
    l.exclusivePercent = 100.0 * static_cast<double>(row.exclusiveTicks) / static_cast<double>(base);
  }
  l.name = namer(row.key);
  l.navigable = navigationTarget(row.key).has_value();
  return l;
}

std::vector<Line> makeLines(const std::vector<analysis::FunctionRow>& rows, uint64_t base,
                            const Namer& namer) {
  std::vector<Line> lines;
  lines.reserve(rows.size());
  for (const analysis::FunctionRow& r : rows) lines.push_back(makeLine(r, base, namer));
  std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
    return a.inclusiveTicks != b.inclusiveTicks ? a.inclusiveTicks > b.inclusiveTicks : a.key < b.key;
  });
  return lines;
}

}  // namespace

ButterflyView buildButterflyView(const analysis::Butterfly& butterfly, const Namer& namer) {
  ButterflyView v;
  const uint64_t base = butterfly.self.inclusiveTicks;
  v.self = makeLine(butterfly.self, base, namer);
  v.callers = makeLines(butterfly.callers, base, namer);
  v.callees = makeLines(butterfly.callees, base, namer);
  return v;
}

std::optional<symbols::FunctionKey> navigationTarget(const symbols::FunctionKey& key) {
  if (key == analysis::kRootKey) return std::nullopt;
  return key;
}

}  // namespace tachyon::viewer::functions
