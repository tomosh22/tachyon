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
// Main-thread only (WP-67d). For tables with a stretch name column (column 0) and fixed numeric columns
// that use ImGuiTableFlags_ScrollX: the inner width to pass to BeginTable so that the name column keeps at
// least max(12 em, min(18 em, 40% of the available width)); when the panel is too narrow the table scrolls
// horizontally instead of crushing the name. `fixedColumnsWidth` is what the other visible columns take
// (see measuredFixedColumnsWidth; pass 0 before the first measurement, then `defaultFixedColumnsEm`
// ems are assumed).
float nameColumnInnerWidth(float fixedColumnsWidth, float defaultFixedColumnsEm);
// Main-thread only. Inside a table (after the headers row): the width of the table except column 0 (the other
// columns, padding and spacing), for the next frame's nameColumnInnerWidth.
float measuredFixedColumnsWidth();
// Main-thread only. The smallest width the name column is kept at for `available` pixels of table.
float minNameColumnWidth(float available);

// Main-thread only. A small spinning arc at the cursor (advances the cursor).
void drawSpinner(float radius, ImU32 color);

}  // namespace tachyon::viewer::common
