#include "viewer/theme.h"

#include <imgui.h>

#include <algorithm>

namespace tachyon::viewer {

float effectiveUiScale(float dpiScale, float userScale) {
  return std::clamp(dpiScale * userScale, Settings::kMinUiScale, Settings::kMaxUiScale);
}

void applyTheme(ThemeKind theme, float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  if (theme == ThemeKind::Light) {
    ImGui::StyleColorsLight(&style);
  } else {
    ImGui::StyleColorsDark(&style);
  }
  style.WindowRounding = 3.0f;
  style.FrameRounding = 2.0f;
  style.TabRounding = 2.0f;
  style.ScrollbarRounding = 2.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.ItemSpacing = ImVec2(8.0f, 5.0f);
  // Multi-viewport is not used; keep docked windows opaque so they match the theme.
  style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  style.ScaleAllSizes(scale);
  ImGui::GetIO().FontGlobalScale = scale;
}

}  // namespace tachyon::viewer
