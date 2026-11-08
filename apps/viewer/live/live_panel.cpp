#include "viewer/live/live_panel.h"

#include <imgui.h>

#include <cstdio>

namespace tachyon::viewer {

namespace {

constexpr ImVec4 kRed(1.0f, 0.4f, 0.4f, 1.0f);
constexpr ImVec4 kAmber(1.0f, 0.75f, 0.2f, 1.0f);

class LivePanel final : public IPanel {
 public:
  explicit LivePanel(LivePanelHost host) : m_host(std::move(host)) {}

  const char* name() const override { return kLivePanelName; }

  void draw(const PanelContext&, bool* open) override {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(name(), open)) {
      ImGui::End();
      return;
    }
    const LiveUiState s = m_host.state ? m_host.state() : LiveUiState{};
    if (s.phase == LiveUiState::Phase::None) {
      ImGui::TextDisabled("No live capture. Start one with Capture > Live...");
      ImGui::End();
      return;
    }
    switch (s.phase) {
      case LiveUiState::Phase::Waiting: ImGui::TextColored(kAmber, "LIVE - waiting for data"); break;
      case LiveUiState::Phase::Running:
        if (s.paused)
          ImGui::TextColored(kAmber, "LIVE (paused)");
        else
          ImGui::TextColored(kRed, "LIVE");
        break;
      case LiveUiState::Phase::Ended:
        ImGui::TextDisabled("ENDED - static snapshot of the last window");
        break;
      case LiveUiState::Phase::Failed: ImGui::TextColored(kRed, "LIVE VIEW STOPPED"); break;
      case LiveUiState::Phase::None: break;
    }
    if (!s.error.empty()) ImGui::TextColored(kRed, "%s", s.error.c_str());

    const bool running = s.phase == LiveUiState::Phase::Running || s.phase == LiveUiState::Phase::Waiting;
    ImGui::BeginDisabled(!running);
    if (ImGui::Button(s.paused ? "Resume" : "Pause") && m_host.setPaused) m_host.setPaused(!s.paused);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.followAvailable);
    if (ImGui::Button(s.following ? "Following" : "Follow") && m_host.setFollow) m_host.setFollow(true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          "Show the newest window and scroll with it. Panning or zooming the timeline turns it off.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.canSave);
    if (ImGui::Button("Save last 30 s") && m_host.saveLast) m_host.saveLast();
    ImGui::EndDisabled();
    if (!s.canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", s.saveHint.empty() ? "Not available" : s.saveHint.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.canStop);
    if (ImGui::Button("Stop") && m_host.stop) m_host.stop();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Stop the capture and open the final trace.");

    ImGui::Separator();
    ImGui::Text("Window: %.1f s   Threads: %u   Samples: %llu", s.windowSeconds, s.threads,
                static_cast<unsigned long long>(s.samples));
    if (s.latencyS)
      ImGui::Text("Latency: %.2f s", *s.latencyS);
    else
      ImGui::TextDisabled("Latency: n/a (remote agent)");
    ImGui::Text("Dropped (live view only): %llu   Gaps: %u   Resyncs: %u   Bad batches: %u",
                static_cast<unsigned long long>(s.droppedLive), s.gaps, s.resubscribes, s.corruptBatches);
    ImGui::TextDisabled("Feed: %s   snapshot %llu of %llu   %llu swaps   build %.0f ms", s.feedState.c_str(),
                        static_cast<unsigned long long>(s.shownGeneration),
                        static_cast<unsigned long long>(s.receivedGeneration),
                        static_cast<unsigned long long>(s.swaps), s.buildMs);
    ImGui::TextDisabled("Symbols: %u service(s) started, %u module(s) loaded", s.symbolEpochs,
                        s.symbolModulesLoaded);
    ImGui::End();
  }

 private:
  LivePanelHost m_host;
};

}  // namespace

std::string liveStatusText(const LiveUiState& s) {
  using Phase = LiveUiState::Phase;
  if (s.phase == Phase::None) return {};
  char buffer[192];
  const char* tag = "LIVE";
  if (s.phase == Phase::Ended) tag = "ENDED";
  if (s.phase == Phase::Failed) tag = "LIVE STOPPED";
  if (s.phase == Phase::Waiting) {
    std::snprintf(buffer, sizeof buffer, "LIVE waiting for data");
    return buffer;
  }
  int n =
      std::snprintf(buffer, sizeof buffer, "%s%s  %.1f s", tag, s.paused ? " (paused)" : "", s.windowSeconds);
  if (s.latencyS && n > 0 && static_cast<size_t>(n) < sizeof buffer)
    n += std::snprintf(buffer + n, sizeof buffer - static_cast<size_t>(n), "  latency %.2f s", *s.latencyS);
  if (n > 0 && static_cast<size_t>(n) < sizeof buffer)
    std::snprintf(buffer + n, sizeof buffer - static_cast<size_t>(n), "  dropped %llu  gaps %u",
                  static_cast<unsigned long long>(s.droppedLive), s.gaps);
  return buffer;
}

std::unique_ptr<IPanel> makeLivePanel(LivePanelHost host) {
  return std::make_unique<LivePanel>(std::move(host));
}

}  // namespace tachyon::viewer
