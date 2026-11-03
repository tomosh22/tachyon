#include "viewer/status/warnings_banner.h"

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
  m_warnings = collectCaptureWarnings(document.model(), agent);
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
  m_notices.clear();
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
  const float lineH = ImGui::GetTextLineHeightWithSpacing();
  const float height =
      lineH * static_cast<float>(shown + (more ? 1 : 0)) + ImGui::GetStyle().WindowPadding.y * 2.0f;
  m_lastHeight = height;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking;
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.35f, 0.24f, 0.05f, 1.0f));
  if (ImGui::BeginViewportSideBar("##CaptureWarnings", ImGui::GetMainViewport(), ImGuiDir_Up, height,
                                  flags)) {
    const float buttonW = ImGui::CalcTextSize("Dismiss").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - buttonW - ImGui::GetStyle().WindowPadding.x * 3.0f);
    for (size_t i = 0; i < shown; ++i) {
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "Warning");
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
  ImGui::PopStyleColor();
}

}  // namespace tachyon::viewer
