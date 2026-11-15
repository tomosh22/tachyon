// WP-67c: the default docked layout of the viewer (Threads Timeline on top; Call Graph, Function List and
// Butterfly below) and the rules that map every built-in panel to one of its nodes.
#pragma once

#include <imgui.h>

#include <string>
#include <vector>

namespace tachyon::viewer {

// The nodes of the default layout a panel can be docked into.
enum class DockSlot { None, Timeline, CallGraph, FunctionList, Butterfly };

// Thread-safe (pure). The slot of a built-in panel by window title; None for unknown (plugin/test) panels.
DockSlot dockSlotFor(const std::string& panelName);

// Thread-safe (pure). The id DockSpaceOverViewport(0, viewport, ...) gives its dockspace in the main
// viewport. Equal to what earlier versions of the viewer saved in imgui.ini, so saved layouts keep matching.
ImGuiID mainDockspaceId(const ImGuiViewport* viewport);

// Main-thread only (needs the ImGui context, before this frame's panels are submitted). True when something
// of a layout was loaded from imgui.ini: the dockspace has a node already, or one of `panelNames` has saved
// window settings. A saved layout, even one with floating windows only, is never replaced by the default.
bool hasSavedLayout(ImGuiID dockspaceId, const std::vector<std::string>& panelNames);

// Main-thread only. Replaces whatever is in the dockspace with the default layout, sized to `size`, and docks
// every panel of `panelNames` into its slot (also panels that are closed: they open docked). Must run before
// the frame's DockSpaceOverViewport call. Returns false and changes nothing when `size` is too small.
bool buildDefaultDockLayout(ImGuiID dockspaceId, ImVec2 size, const std::vector<std::string>& panelNames);

// Main-thread only. For a panel opened for the first time into a layout that was loaded from imgui.ini (so
// the builder did not dock it): the dock node of its slot's anchor window, or 0 when there is none. Use with
// SetNextWindowDockID(id, ImGuiCond_FirstUseEver).
ImGuiID dockTargetFor(const std::string& panelName);

}  // namespace tachyon::viewer
