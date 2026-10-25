// The "Thread Interaction" panel (WP-46): for the wait chosen on the timeline (SelectionState::selectedWait)
// it shows the blocking stack (left) and the unblocking stack (right), the waker, the wait reason, the wait
// duration and the ready latency, with "Go to waker" / "Go to woken" buttons; below it lists the N longest
// waits of the current filter range with their wakers.
#pragma once

#include "viewer/interaction/interaction_model.h"
#include "viewer/interaction/longest_waits_runner.h"
#include "viewer/panel.h"

#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer {

class ThreadInteractionPanel final : public IPanel {
 public:
  static constexpr const char* kName = interaction::kThreadInteractionPanelName;

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // ---- actions (buttons and tests); main-thread only ----
  // Selects `episode` as the inspected wait: selected thread, selected wait, navigation to the woken thread.
  void selectWait(const PanelContext& context, const interaction::WaitEpisode& episode);
  // Select the waker thread and show the wake-up position; false without a selected wait or waker thread.
  bool goToWaker(const PanelContext& context);
  // Select the woken thread and show the whole wait; false without a selected wait.
  bool goToWoken(const PanelContext& context);

  // ---- state (tests); main-thread only ----
  const std::optional<interaction::WaitEpisode>& episode() const { return m_episode; }
  const interaction::EpisodeDetails& details() const { return m_details; }
  const std::vector<interaction::StackLine>& blockingStack() const { return m_blocking; }
  const std::vector<interaction::StackLine>& unblockingStack() const { return m_unblocking; }
  const interaction::LongestWaitsRunner& waits() const { return m_waits; }
  size_t maxWaits() const { return m_maxWaits; }
  void setMaxWaits(size_t n);
  // Number of times the panel was asked to take the focus (PanelRequest for this panel).
  uint64_t focusRequests() const { return m_focusRequests; }

 private:
  void refresh(const PanelContext& context);
  void requestWaits(const PanelContext& context);
  void drawDetails(const PanelContext& context);
  void drawStacks(const PanelContext& context);
  void drawWaits(const PanelContext& context);

  std::optional<interaction::WaitEpisode> m_episode;
  std::optional<WaitSelection> m_shownSelection;  // what m_episode was resolved for
  uint64_t m_shownDocGeneration = 0;
  uint64_t m_shownSymbolGeneration = ~0ull;
  interaction::EpisodeDetails m_details;
  std::vector<interaction::StackLine> m_blocking, m_unblocking;

  interaction::LongestWaitsRunner m_waits;
  interaction::WaitQuery m_requestedQuery;
  uint64_t m_requestedDocGeneration = ~0ull;
  bool m_everRequested = false;
  size_t m_maxWaits = 50;

  bool m_focusPending = false;
  uint64_t m_focusRequests = 0;
};

}  // namespace tachyon::viewer
