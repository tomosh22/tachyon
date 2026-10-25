// Pure logic of the Butterfly panel (WP-44): the display lines for analysis::Butterfly, their percentages and
// order, and the navigation rule. No ImGui, no executor.
//
// Percentages are relative to the selected function's inclusive time (caller and callee lines: inclusive and
// exclusive of the line divided by self inclusive; the self line shows 100 % inclusive). Because callers are
// taken relative to the outermost occurrence of the function (docs/specs/analysis-query.md), the caller
// percentages sum to 100 %; the callee percentages sum to at most 100 % (the rest is the function's own
// exclusive time).
#pragma once

#include "analysis/query.h"
#include "symbols/function_key.h"
#include "viewer/common/function_names.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer::functions {

struct Line {
  symbols::FunctionKey key;
  uint64_t inclusiveTicks = 0, exclusiveTicks = 0;
  uint32_t instances = 0;
  double inclusivePercent = 0, exclusivePercent = 0;  // of the selected function's inclusive time
  common::NameParts name;
  bool navigable = true;  // false for the synthetic <root> caller
};

struct ButterflyView {
  Line self;
  std::vector<Line> callers;  // shown above the function, heaviest first
  std::vector<Line> callees;  // shown below, heaviest first
  // True when the query found no sample containing the function (zero instances).
  bool empty() const { return self.instances == 0 && self.inclusiveTicks == 0; }
};

using Namer = std::function<common::NameParts(const symbols::FunctionKey&)>;

// Thread-safe (pure; `namer` must be). Lines sorted by inclusive ticks descending, then key ascending.
ButterflyView buildButterflyView(const analysis::Butterfly& butterfly, const Namer& namer);

// Thread-safe: pure function. The function a click on `key` selects; nullopt for the synthetic <root> caller.
std::optional<symbols::FunctionKey> navigationTarget(const symbols::FunctionKey& key);

}  // namespace tachyon::viewer::functions
