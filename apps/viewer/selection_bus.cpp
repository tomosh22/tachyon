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

void SelectionBus::setSelectedWait(std::optional<WaitSelection> wait) {
  if (m_state.selectedWait == wait) return;
  m_state.selectedWait = wait;
  publish(SelectionField::SelectedWait);
}

uint64_t SelectionBus::requestNavigation(ThreadIdx thread, uint64_t t0, uint64_t t1) {
  m_state.navigation = NavigationRequest{m_nextSerial++, thread, t0, t1};
  publish(SelectionField::Navigation);
  return m_state.navigation.serial;
}

uint64_t SelectionBus::requestPanel(std::string name) {
  m_state.panelRequest = PanelRequest{m_nextSerial++, std::move(name)};
  publish(SelectionField::PanelRequest);
  return m_state.panelRequest.serial;
}

uint64_t SelectionBus::setInstanceHighlight(
    std::optional<FunctionKey> key, std::shared_ptr<const std::vector<analysis::Instance>> instances) {
  InstanceHighlight next;
  if (instances) {
    next.generation = m_nextSerial++;
    next.key = key;
    next.instances = std::move(instances);
  } else if (m_state.instanceHighlight.generation == 0) {
    return 0;  // already clear
  }
  m_state.instanceHighlight = std::move(next);
  publish(SelectionField::InstanceHighlight);
  return m_state.instanceHighlight.generation;
}

void SelectionBus::setHighlightCurrent(std::optional<uint32_t> index) {
  InstanceHighlight& h = m_state.instanceHighlight;
  if (!h.active() || h.current == index) return;
  h.current = index;
  publish(SelectionField::InstanceHighlight);
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
