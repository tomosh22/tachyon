// The scope-related analysis options every panel shares (WP-54): "Blend instrumentation scopes into sampled
// stacks" and "Group by fiber". The user's wishes live in SelectionState::viewOptions (the blend wish starts
// from Settings::blendScopes); what a panel queries with is the EFFECTIVE value, which also depends on the
// trace: blending needs scope events, fiber grouping needs fiber events. A trace without instrumentation
// therefore queries exactly as before WP-54.
#pragma once

#include "analysis/query.h"
#include "model/model.h"
#include "viewer/panel.h"
#include "viewer/selection_bus.h"

namespace tachyon::viewer::common {

struct EffectiveScopeOptions {
  bool blendScopes = false;
  bool groupByFiber = false;

  bool operator==(const EffectiveScopeOptions&) const = default;
};

// Thread-safe: pure function on the immutable model. The trace has Begin/End events.
inline bool traceHasScopes(const model::TraceModel& model) {
  return !model.scopeEvents.empty();
}
// Thread-safe: pure function. The trace has fiber events.
inline bool traceHasFibers(const model::TraceModel& model) {
  return !model.fiberSwitches.empty();
}

// Thread-safe: pure function. The wishes of `options` limited to what the trace can show.
EffectiveScopeOptions effectiveScopeOptions(const model::TraceModel& model, const ViewOptions& options);
// Main-thread only (reads the bus). Same for a panel context; no document or no bus gives all-off.
EffectiveScopeOptions effectiveScopeOptions(const PanelContext& context);
// Thread-safe: pure function. Copies the two flags into `options`.
void applyScopeOptions(analysis::AggregationOptions& options, const EffectiveScopeOptions& scope);

}  // namespace tachyon::viewer::common
