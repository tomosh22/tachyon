// Panels that ship with the shell. WP-41..48 add their own panels through PanelRegistry.
#pragma once

#include "viewer/panel.h"

#include <memory>

namespace tachyon::viewer {

// Thread-safe: factory. Lists the open trace's summary and its threads; clicking a thread selects it.
std::unique_ptr<IPanel> makeTraceInfoPanel();

}  // namespace tachyon::viewer
