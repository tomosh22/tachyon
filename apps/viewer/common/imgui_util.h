// ImGui drawing helpers shared by the analysis panels (extracted from the Call Graph by WP-44).
#pragma once

#include <imgui.h>

namespace tachyon::viewer::common {

// Main-thread only. Right-aligned text in the current table cell.
void textRight(const char* text);
// Main-thread only. A small spinning arc at the cursor (advances the cursor).
void drawSpinner(float radius, ImU32 color);

}  // namespace tachyon::viewer::common
