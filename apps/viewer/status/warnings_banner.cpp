#include "viewer/status/warnings_banner.h"
#include "viewer/theme.h"

#include "viewer/paths.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace tachyon::viewer {

namespace {
constexpr size_t kMaxShownLines = 4;
}

std::string WarningsBanner::keyOf(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(path, ec);
  return pathToUtf8(ec ? path : abs);
}

void WarningsBanner::recollect(const Document& document) {
  std::vector<std::string> agent;
  if (const auto it = m_agentWarnings.find(m_key); it != m_agentWarnings.end()) agent = it->second;
  // WP-65b: a Live snapshot is partial by nature and its dropped-event count is on the status bar.
  if (!document.isLive())
    m_warnings = collectCaptureWarnings(document.model(), agent);
  else
    m_warnings.clear();
  for (const CaptureWarning& n : m_notices) m_warnings.push_back(n);
}

void WarningsBanner::update(const Document* document, uint64_t documentGeneration) {
  if (m_haveDocument && m_generation == documentGeneration && (document != nullptr)) {
    if (m_agentDirty) {
      m_agentDirty = false;
      const std::vector<CaptureWarning> before = m_warnings;
      recollect(*document);
      // Something the user has not seen shows the banner again, even if it was dismissed.
      for (const CaptureWarning& w : m_warnings) {
        const bool known = std::any_of(before.begin(), before.end(),
                                       [&](const CaptureWarning& b) { return b.text == w.text; });
        if (!known) {
          m_dismissed = false;
          m_dismissedDocuments.erase(m_key);
        }
      }
    }
    return;
  }
  m_agentDirty = false;
  if (!(document && document->isLive() && m_haveDocument && keyOf(document->path()) == m_key))
    m_notices.clear();  // (the notices of a live capture survive its snapshots)
  if (!document) {
    m_haveDocument = false;
    m_generation = documentGeneration;
    m_warnings.clear();
    m_dismissed = false;
    m_key.clear();
    return;
  }
  m_haveDocument = true;
  m_generation = documentGeneration;
  m_key = keyOf(document->path());
  recollect(*document);
  m_dismissed = m_dismissedDocuments.count(m_key) != 0;
}

void WarningsBanner::addAgentWarnings(const std::filesystem::path& path, std::vector<std::string> warnings) {
  if (warnings.empty()) return;
  const std::string key = keyOf(path);
  m_agentWarnings[key] = std::move(warnings);
  if (m_haveDocument && key == m_key) m_agentDirty = true;
}

void WarningsBanner::addNotice(CaptureWarningKind kind, std::string text) {
  for (CaptureWarning& w : m_notices) {
    if (w.kind == kind) w.text = text;
  }
  for (CaptureWarning& w : m_warnings) {
    if (w.kind == kind) {
      w.text = std::move(text);
      m_dismissed = false;
      return;
    }
  }
  m_notices.push_back({kind, text});
  m_warnings.push_back({kind, std::move(text)});
  m_dismissed = false;
}

void WarningsBanner::dismiss() {
  m_dismissed = true;
  if (!m_key.empty()) m_dismissedDocuments.insert(m_key);
}

void WarningsBanner::draw() {
  if (!visible()) {
    m_lastHeight = 0.0f;
    return;
  }
  const size_t shown = std::min(m_warnings.size(), kMaxShownLines);
  const bool more = m_warnings.size() > shown;
  const ThemePalette& pal = currentPalette();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float buttonW = ImGui::CalcTextSize("Dismiss").x + style.FramePadding.x * 2.0f;
  const float wrapPos = ImGui::GetMainViewport()->Size.x - buttonW - style.WindowPadding.x * 3.0f;
  const float labelW = ImGui::CalcTextSize("Warning").x + style.ItemSpacing.x;
  // Height of the wrapped lines, so that nothing is clipped.
  float height = style.WindowPadding.y * 2.0f;
  for (size_t i = 0; i < shown; ++i) {
    const float textH = ImGui::CalcTextSize(m_warnings[i].text.c_str(), nullptr, false,
                                            std::max(wrapPos - labelW - style.WindowPadding.x, 40.0f))
                            .y;
    height += std::max(textH, ImGui::GetTextLineHeight()) + style.ItemSpacing.y;
  }
  if (more) height += ImGui::GetTextLineHeightWithSpacing();
  m_lastHeight = height;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking;
  // Pale amber with dark amber text in light mode, muted amber in dark mode.
  const bool light = pal.scheme == ColorScheme::Light;
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(light ? makeRgba(255, 243, 210)
                                                                                : makeRgba(72, 54, 18)));
  ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(light ? makeRgba(230, 190, 90)
                                                                              : makeRgba(120, 92, 30)));
  if (ImGui::BeginViewportSideBar("##CaptureWarnings", ImGui::GetMainViewport(), ImGuiDir_Up, height,
                                  flags)) {
    ImGui::PushTextWrapPos(wrapPos);
    for (size_t i = 0; i < shown; ++i) {
      ImGui::TextColored(
          ImGui::ColorConvertU32ToFloat4(light ? makeRgba(133, 77, 0) : makeRgba(255, 217, 115)), "Warning");
      ImGui::SameLine();
      ImGui::TextUnformatted(m_warnings[i].text.c_str());
    }
    ImGui::PopTextWrapPos();
    if (more) {
      ImGui::TextDisabled("... and %zu more", m_warnings.size() - shown);
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        for (size_t i = shown; i < m_warnings.size(); ++i) ImGui::TextUnformatted(m_warnings[i].text.c_str());
        ImGui::EndTooltip();
      }
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - buttonW - ImGui::GetStyle().WindowPadding.x,
                               ImGui::GetStyle().WindowPadding.y));
    if (ImGui::Button("Dismiss")) dismiss();
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
}

}  // namespace tachyon::viewer
