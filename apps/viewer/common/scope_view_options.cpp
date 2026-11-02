#include "viewer/common/scope_view_options.h"

#include "viewer/document.h"

namespace tachyon::viewer::common {

EffectiveScopeOptions effectiveScopeOptions(const model::TraceModel& model, const ViewOptions& options) {
  EffectiveScopeOptions out;
  out.blendScopes = options.blendScopes && traceHasScopes(model);
  out.groupByFiber = options.groupByFiber && traceHasFibers(model);
  return out;
}

EffectiveScopeOptions effectiveScopeOptions(const PanelContext& context) {
  if (!context.document || !context.selection) return {};
  return effectiveScopeOptions(context.document->model(), context.selection->state().viewOptions);
}

void applyScopeOptions(analysis::AggregationOptions& options, const EffectiveScopeOptions& scope) {
  options.blendScopes = scope.blendScopes;
  options.groupByFiber = scope.groupByFiber;
}

}  // namespace tachyon::viewer::common
