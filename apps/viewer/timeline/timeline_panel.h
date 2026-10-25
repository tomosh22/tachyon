// The "Threads Timeline" panel (WP-42): an IPanel hosting a TimelineWidget.
#pragma once

#include "viewer/panel.h"
#include "viewer/timeline/timeline_widget.h"

namespace tachyon::viewer {

class ThreadsTimelinePanel final : public IPanel {
 public:
  static constexpr const char* kName = "Threads Timeline";

  // Main-thread only.
  const char* name() const override { return kName; }
  void draw(const PanelContext& context, bool* open) override;
  void onSelectionChanged(const PanelContext& context, const SelectionState& state,
                          SelectionField field) override;
  void onDocumentChanged(const Document* document) override;

  // Main-thread only.
  timeline::TimelineWidget& widget() { return m_widget; }
  const timeline::TimelineWidget& widget() const { return m_widget; }

 private:
  timeline::TimelineWidget m_widget;
};

}  // namespace tachyon::viewer
