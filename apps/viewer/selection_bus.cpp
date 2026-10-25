#include "viewer/selection_bus.h"

#include <algorithm>
#include <utility>

namespace tachyon::viewer {

SelectionBus::SubscriptionId SelectionBus::subscribe(Callback callback) {
  if (!callback) return kNoSubscription;
  const SubscriptionId id = m_nextId++;
  m_subscribers.push_back({id, std::move(callback)});
  return id;
}

void SelectionBus::unsubscribe(SubscriptionId id) {
  std::erase_if(m_subscribers, [id](const Subscriber& s) { return s.id == id; });
}

void SelectionBus::setTimeFilter(std::optional<ViewFilter> filter) {
  if (m_state.timeFilter == filter) return;
  m_state.timeFilter = std::move(filter);
  publish(SelectionField::TimeFilter);
}

void SelectionBus::setSelectedThread(ThreadIdx thread) {
  if (m_state.selectedThread == thread) return;
  m_state.selectedThread = thread;
  publish(SelectionField::SelectedThread);
}

void SelectionBus::setSelectedFunction(std::optional<FunctionKey> function) {
  if (m_state.selectedFunction == function) return;
  m_state.selectedFunction = function;
  publish(SelectionField::SelectedFunction);
}

void SelectionBus::setHovered(const HoveredItem& item) {
  if (m_state.hovered == item) return;
  m_state.hovered = item;
  publish(SelectionField::Hovered);
}

void SelectionBus::clear() {
  m_state = SelectionState{};
  publish(SelectionField::All);
}

void SelectionBus::publish(SelectionField field) {
  std::vector<SubscriptionId> ids;
  ids.reserve(m_subscribers.size());
  for (const Subscriber& s : m_subscribers) ids.push_back(s.id);
  for (const SubscriptionId id : ids) {
    const auto it = std::find_if(m_subscribers.begin(), m_subscribers.end(),
                                 [id](const Subscriber& s) { return s.id == id; });
    if (it == m_subscribers.end()) continue;  // unsubscribed by an earlier callback in this dispatch
    const Callback callback = it->callback;   // copy: the vector may change while the callback runs
    callback(m_state, field);
  }
}

}  // namespace tachyon::viewer
