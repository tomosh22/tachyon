#include "viewer/common/imgui_util.h"

#include <cmath>

namespace tachyon::viewer::common {

void textRight(const char* text) {
  const float w = ImGui::CalcTextSize(text).x;
  const float avail = ImGui::GetContentRegionAvail().x;
  if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
  ImGui::TextUnformatted(text);
}

ImU32 scopeTextColor() {
  return IM_COL32(120, 200, 255, 255);
}

void drawScopeIcon(float size) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight();
  const float cx = pos.x + size * 0.5f, cy = pos.y + h * 0.5f, r = size * 0.4f;
  ImGui::GetWindowDrawList()->AddQuadFilled(ImVec2(cx, cy - r), ImVec2(cx + r, cy), ImVec2(cx, cy + r),
                                            ImVec2(cx - r, cy), scopeTextColor());
  ImGui::Dummy(ImVec2(size, h));
  ImGui::SameLine(0.0f, 2.0f);
}

void drawSpinner(float radius, ImU32 color) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 centre(pos.x + radius, pos.y + radius + 1.0f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float start = static_cast<float>(ImGui::GetTime()) * 6.0f;
  draw->PathClear();
  constexpr int kSegments = 24;
  for (int i = 0; i < kSegments; ++i) {
    const float a = start + (static_cast<float>(i) / kSegments) * 4.5f;
    draw->PathLineTo(ImVec2(centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius));
  }
  draw->PathStroke(color, 0, 2.0f);
  ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f + 2.0f));
}

}  // namespace tachyon::viewer::common
