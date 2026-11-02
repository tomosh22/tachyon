#include "viewer/status/toast.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <optional>
#include <utility>

namespace tachyon::viewer {

ToastQueue::ToastQueue(ClockFn clock) : m_clock(std::move(clock)) {
  if (!m_clock) {
    m_clock = [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
  }
}

double ToastQueue::now() const {
  return m_clock();
}

uint64_t ToastQueue::push(ToastLevel level, std::string text) {
  Toast t;
  t.id = m_nextId++;
  t.level = level;
  t.text = std::move(text);
  t.createdAt = m_clock();
  m_toasts.push_back(std::move(t));
  const uint64_t id = m_toasts.back().id;
  while (m_toasts.size() > kMaxVisible) {
    m_heights.erase(m_toasts.front().id);
    m_toasts.erase(m_toasts.begin());
  }
  return id;
}

bool ToastQueue::expire() {
  const double t = m_clock();
  const size_t before = m_toasts.size();
  std::erase_if(m_toasts, [&](const Toast& toast) {
    const bool gone = t - toast.createdAt >= kLifetimeSeconds;
    if (gone) m_heights.erase(toast.id);
    return gone;
  });
  return m_toasts.size() != before;
}

void ToastQueue::dismiss(uint64_t id) {
  std::erase_if(m_toasts, [&](const Toast& toast) { return toast.id == id; });
  m_heights.erase(id);
}

void ToastQueue::draw() {
  expire();
  if (m_toasts.empty()) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float margin = ImGui::GetFontSize();
  float y = vp->WorkPos.y + vp->WorkSize.y - margin;
  const float x = vp->WorkPos.x + vp->WorkSize.x - margin;
  std::optional<uint64_t> clicked;
  // Newest at the bottom, older ones stacked above it.
  for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
    const Toast& t = *it;
    ImVec4 accent(0.45f, 0.7f, 1.0f, 1.0f);
    const char* tag = "Info";
    if (t.level == ToastLevel::Warning) {
      accent = ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
      tag = "Warning";
    } else if (t.level == ToastLevel::Error) {
      accent = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
      tag = "Error";
    }
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(ImGui::GetFontSize() * 28.0f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const std::string label = "##toast" + std::to_string(t.id);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin(label.c_str(), nullptr, kFlags)) {
      ImGui::TextColored(accent, "%s", tag);
      ImGui::SameLine();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 27.0f);
      ImGui::TextUnformatted(t.text.c_str());
      ImGui::PopTextWrapPos();
      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = t.id;
      m_heights[t.id] = ImGui::GetWindowHeight();
    }
    ImGui::End();
    const auto h = m_heights.find(t.id);
    y -=
        (h != m_heights.end() ? h->second : ImGui::GetFrameHeight() * 1.5f) + ImGui::GetStyle().ItemSpacing.y;
  }
  if (clicked) dismiss(*clicked);
}

}  // namespace tachyon::viewer
