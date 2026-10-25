// Panels that ship with the shell. WP-41..48 add their own panels through PanelRegistry.
#pragma once

#include "viewer/panel.h"

#include <memory>

namespace tachyon::viewer {

// Thread-safe: factory. Lists the open trace's summary and its threads; clicking a thread selects it.
std::unique_ptr<IPanel> makeTraceInfoPanel();

// Thread-safe: factory. WP-42: per-thread timeline (state bands, sample strip, flame chart); implemented in
// timeline/timeline_panel.cpp.
std::unique_ptr<IPanel> makeThreadsTimelinePanel();

// Thread-safe: factory. WP-46: wait / wake-up inspector ("Thread Interaction"); implemented in
// interaction/interaction_panel.cpp.
std::unique_ptr<IPanel> makeThreadInteractionPanel();

// Thread-safe: factory. WP-43: top-down call graph ("Call Graph"); implemented in
// callgraph/callgraph_panel.cpp.
std::unique_ptr<IPanel> makeCallGraphPanel();

}  // namespace tachyon::viewer
