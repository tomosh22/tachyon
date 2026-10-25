// Theme and DPI handling for the ImGui style.
#pragma once

#include "viewer/settings.h"

namespace tachyon::viewer {

// Main-thread only. Resets the current ImGui style, applies the theme colours and scales sizes and fonts by
// `scale` (display DPI scale times the user's UI scale). Requires a live ImGui context.
void applyTheme(ThemeKind theme, float scale);

// Thread-safe: pure function. Combined scale, clamped to the supported range.
float effectiveUiScale(float dpiScale, float userScale);

}  // namespace tachyon::viewer
