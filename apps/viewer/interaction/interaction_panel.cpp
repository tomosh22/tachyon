#include "viewer/interaction/interaction_panel.h"

#include "viewer/builtin_panels.h"
#include "viewer/document.h"
#include "viewer/job_queue.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace tachyon::viewer {

using interaction::StackLine;
using interaction::WaitEpisode;

namespace {

double clockHzOf(const model::TraceModel& m) {
  return m.header.clock_frequency_hz ? static_cast<double>(m.header.clock_frequency_hz) : 1e9;
}

}  // namespace

void ThreadInteractionPanel::setMaxWaits(size_t n) {
  m_maxWaits = std::clamp<size_t>(n, 1, 5000);
}

void ThreadInteractionPanel::onDocumentChanged(const Document*) {
  m_episode.reset();
  m_shownSelection.reset();
  m_blocking.clear();
  m_unblocking.clear();
  m_details = {};
  m_waits.reset(nullptr);  // an in-flight scan finishes on its own and is dropped as stale
  m_everRequested = false;
  m_shownSymbolGeneration = ~0ull;
}

void ThreadInteractionPanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                                SelectionField field) {
  if (field == SelectionField::PanelRequest && state.panelRequest.name == kName) {
    m_focusPending = true;
    ++m_focusRequests;
  }
  if (field == SelectionField::SelectedWait || field == SelectionField::All) context.requestRedraw();
}

void ThreadInteractionPanel::refresh(const PanelContext& context) {
  if (!context.document || !context.selection) {
    m_episode.reset();
    m_shownSelection.reset();
    m_blocking.clear();
    m_unblocking.clear();
    return;
  }
  const model::TraceModel& model = context.document->model();
  const std::optional<WaitSelection>& wanted = context.selection->state().selectedWait;
  if (wanted == m_shownSelection && context.documentGeneration == m_shownDocGeneration &&
      context.symbolGeneration == m_shownSymbolGeneration) {
    return;
  }
  m_shownSelection = wanted;
  m_shownDocGeneration = context.documentGeneration;
  m_shownSymbolGeneration = context.symbolGeneration;
  m_episode.reset();
  m_blocking.clear();
  m_unblocking.clear();
  m_details = {};
  if (!wanted) return;
  m_episode = interaction::episodeStartingAt(model, wanted->thread, wanted->switchOutTs);
  if (!m_episode) return;
  m_details = interaction::describeEpisode(model, *m_episode, clockHzOf(model));
  m_blocking = interaction::describeStack(model, context.symbols, m_episode->blockingStack);
  m_unblocking = interaction::describeStack(model, context.symbols, m_episode->unblockingStack);
}

void ThreadInteractionPanel::requestWaits(const PanelContext& context) {
  if (!context.document || !context.selection) return;
  const model::TraceModel& model = context.document->model();
  interaction::WaitQuery q;
  q.maxResults = m_maxWaits;
  q.t0 = model.header.start_ts;
  if (const std::optional<ViewFilter>& f = context.selection->state().timeFilter) {
    q.t0 = f->t0;
    q.t1 = f->t1;
    q.threads = f->threads;
  }
  if (m_everRequested && q == m_requestedQuery && context.documentGeneration == m_requestedDocGeneration)
    return;
  m_everRequested = true;
  m_requestedQuery = q;
  m_requestedDocGeneration = context.documentGeneration;
  const PanelContext redraw = context;  // only the redraw hook is used from the completion
  m_waits.request(context.document->sharedModel(), q, context.jobs, [redraw] { redraw.requestRedraw(); });
}

void ThreadInteractionPanel::selectWait(const PanelContext& context, const WaitEpisode& e) {
  if (!context.selection) return;
  SelectionBus& bus = *context.selection;
  bus.setSelectedThread(e.thread);
  bus.setSelectedWait(WaitSelection{e.thread, e.switchOutTs});
  const double hz = context.document ? clockHzOf(context.document->model()) : 1e9;
  const interaction::NavTarget t = interaction::wokenTarget(e, hz);
  bus.requestNavigation(t.thread, t.t0, t.t1);
}

bool ThreadInteractionPanel::goToWaker(const PanelContext& context) {
  if (!m_episode || !context.selection || !context.document) return false;
  const auto target = interaction::wakerTarget(*m_episode, clockHzOf(context.document->model()));
  if (!target) return false;
  context.selection->setSelectedThread(target->thread);
  context.selection->requestNavigation(target->thread, target->t0, target->t1);
  return true;
}

bool ThreadInteractionPanel::goToWoken(const PanelContext& context) {
  if (!m_episode || !context.selection || !context.document) return false;
  const interaction::NavTarget t = interaction::wokenTarget(*m_episode, clockHzOf(context.document->model()));
  context.selection->setSelectedThread(t.thread);
  context.selection->requestNavigation(t.thread, t.t0, t.t1);
  return true;
}

void ThreadInteractionPanel::draw(const PanelContext& context, bool* open) {
  if (m_focusPending) {
    ImGui::SetNextWindowFocus();
    m_focusPending = false;
  }
  ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(name(), open)) {
    ImGui::End();
    return;
  }
  if (!context.document || !context.selection) {
    ImGui::TextDisabled("No trace open.");
    ImGui::End();
    return;
  }
  refresh(context);
  requestWaits(context);
  drawDetails(context);
  ImGui::Separator();
  drawStacks(context);
  ImGui::Separator();
  drawWaits(context);
  ImGui::End();
}

void ThreadInteractionPanel::drawDetails(const PanelContext& context) {
  if (!m_episode) {
    if (m_shownSelection) {
      ImGui::TextDisabled("The selected wait is not in this trace.");
    } else {
      ImGui::TextDisabled("Click a Waiting or Ready interval in the timeline, or a row below.");
    }
    return;
  }
  const WaitEpisode& e = *m_episode;
  const interaction::EpisodeDetails& d = m_details;
  ImGui::Text("Thread: %s", d.woken.c_str());
  ImGui::Text("Woken by: %s", d.waker.c_str());
  ImGui::Text("Wait reason: %s (%s)", d.waitReason.c_str(), d.waitCategory.c_str());
  ImGui::Text("Wait duration: %s   Ready latency: %s   Switched out at: %s", d.waitDuration.c_str(),
              d.readyLatency.c_str(), d.switchOutTime.c_str());
  const double hz = clockHzOf(context.document->model());
  const bool canGoToWaker = interaction::wakerTarget(e, hz).has_value();
  ImGui::BeginDisabled(!canGoToWaker);
  if (ImGui::Button("Go to waker")) goToWaker(context);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Go to woken")) goToWoken(context);
  ImGui::SameLine();
  if (ImGui::Button("Clear")) context.selection->setSelectedWait(std::nullopt);
}

void ThreadInteractionPanel::drawStacks(const PanelContext& context) {
  const float height = std::max(ImGui::GetTextLineHeightWithSpacing() * 10.0f, 120.0f);
  if (!ImGui::BeginTable("##stacks", 2,
                         ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                         ImVec2(0, height))) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Blocking stack (why it stopped)");
  ImGui::TableSetupColumn("Unblocking stack (who woke it)");
  ImGui::TableHeadersRow();
  const size_t rows = std::max(m_blocking.size(), m_unblocking.size());
  const auto cell = [&](const std::vector<StackLine>& lines, size_t i, int column) {
    ImGui::PushID(static_cast<int>(i * 2 + static_cast<size_t>(column)));
    if (i < lines.size()) {
      const StackLine& l = lines[i];
      char label[512];
      std::snprintf(label, sizeof label, "%2zu  %s", i, l.text.c_str());
      const bool selected = l.key && context.selection->state().selectedFunction == l.key;
      if (ImGui::Selectable(label, selected) && l.key) context.selection->setSelectedFunction(l.key);
    } else if (i == 0) {
      ImGui::TextDisabled(m_episode ? "(no stack)" : "");
    }
    ImGui::PopID();
  };
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(std::max<size_t>(rows, 1)));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      cell(m_blocking, static_cast<size_t>(i), 0);
      ImGui::TableNextColumn();
      cell(m_unblocking, static_cast<size_t>(i), 1);
    }
  }
  ImGui::EndTable();
}

void ThreadInteractionPanel::drawWaits(const PanelContext& context) {
  const model::TraceModel& model = context.document->model();
  const double hz = clockHzOf(model);
  ImGui::Text("Longest waits in the filter range%s", m_waits.pending() ? " (updating...)" : "");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
  int n = static_cast<int>(m_maxWaits);
  if (ImGui::InputInt("N", &n, 10, 50)) setMaxWaits(static_cast<size_t>(std::max(n, 1)));
  ImGui::SameLine();
  ImGui::TextDisabled("%llu waits matched", static_cast<unsigned long long>(m_waits.totalWaits()));
  if (!ImGui::BeginTable("##waits", 6,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                             ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Thread");
  ImGui::TableSetupColumn("Woken by");
  ImGui::TableSetupColumn("Reason");
  ImGui::TableSetupColumn("Wait");
  ImGui::TableSetupColumn("Ready latency");
  ImGui::TableSetupColumn("At");
  ImGui::TableHeadersRow();
  const std::vector<WaitEpisode>& rows = m_waits.rows();
  const std::optional<WaitSelection>& chosen = context.selection->state().selectedWait;
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const WaitEpisode& e = rows[static_cast<size_t>(i)];
      const interaction::EpisodeDetails d = interaction::describeEpisode(model, e, hz);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      const bool selected = chosen && chosen->thread == e.thread && chosen->switchOutTs == e.switchOutTs;
      if (ImGui::Selectable(d.woken.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
        selectWait(context, e);
      ImGui::PopID();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.waker.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.waitReason.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.waitDuration.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.readyLatency.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.switchOutTime.c_str());
    }
  }
  ImGui::EndTable();
}

std::unique_ptr<IPanel> makeThreadInteractionPanel() {
  return std::make_unique<ThreadInteractionPanel>();
}

}  // namespace tachyon::viewer
