// ImGui drawing helpers shared by the analysis panels (extracted from the Call Graph by WP-44).
#pragma once

#include <imgui.h>

namespace tachyon::viewer::common {

// Main-thread only. Right-aligned text in the current table cell.
void textRight(const char* text);
// Main-thread only (WP-54). The icon of an instrumentation scope in the analysis tables: a small filled
// diamond in the scope colour, drawn at the cursor; advances the cursor like text of width `size`.
void drawScopeIcon(float size);
// The colour of scope rows and icons in the analysis tables (distinct from sampled functions).
ImU32 scopeTextColor();
// Main-thread only. A small spinning arc at the cursor (advances the cursor).
void drawSpinner(float radius, ImU32 color);

}  // namespace tachyon::viewer::common
