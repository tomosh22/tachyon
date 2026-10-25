// SelectionBus: the viewer's shared "what is selected" state; panels subscribe and are told what changed.
#pragma once

#include "analysis/instances.h"
#include "core/strong_id.h"
#include "symbols/function_key.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer {

// Placeholder for the canonical Filter from docs/specs/analysis-query.md (3.7). WP-33 provides the real type;
// the shape here is identical so the swap is a typedef change.
struct ViewFilter {
  uint64_t t0 = 0, t1 = 0;
  std::optional<std::vector<ThreadIdx>> threads;
  std::optional<CallstackIdx> subtreeRoot;
  std::optional<FunctionIdx> function;

  bool operator==(const ViewFilter&) const = default;
};

// The canonical FunctionKey of spec 3.7 (WP-20): {module, functionStartRva} for resolved sampled functions,
// {module, rva} for unresolved frames, {kScopeModule, StringIdx} for instrumentation scopes.
using FunctionKey = symbols::FunctionKey;

// What a hovered item refers to; panels fill it in, others show a cross-highlight.
struct HoveredItem {
  enum class Kind : uint8_t { None, Thread, Function, Callstack, Scope, Time };
  Kind kind = Kind::None;
  ThreadIdx thread;
  CallstackIdx callstack;
  std::optional<FunctionKey> function;
  uint64_t ts = 0;

  bool operator==(const HoveredItem&) const = default;
};

// A wait of one thread chosen for inspection (WP-46): the off-CPU episode that starts at `switchOutTs` on
// `thread`. Identified by value so it survives rebuilds of the interval vectors.
struct WaitSelection {
  ThreadIdx thread;
  uint64_t switchOutTs = 0;

  bool operator==(const WaitSelection&) const = default;
};

// Asks the timeline to show [t0, t1) (absolute ticks) on the row of `thread` (WP-46). `serial` is unique per
// request (0 = no request yet) so that repeating the same request is still an event.
struct NavigationRequest {
  uint64_t serial = 0;
  ThreadIdx thread;
  uint64_t t0 = 0, t1 = 0;

  bool operator==(const NavigationRequest&) const = default;
};

// Asks the shell to open and focus a panel by name (WP-46). `serial` as for NavigationRequest.
struct PanelRequest {
  uint64_t serial = 0;
  std::string name;

  bool operator==(const PanelRequest&) const = default;
};

// The instances of one function / scope that the timeline highlights (WP-47). `generation` identifies the
// instance list (0 = nothing highlighted; a new list gets a new generation, a change of `current` does not).
struct InstanceHighlight {
  uint64_t generation = 0;
  std::optional<FunctionKey> key;
  std::shared_ptr<const std::vector<analysis::Instance>> instances;  // sorted by begin
  std::optional<uint32_t> current;  // index into *instances of the instance last navigated to

  bool active() const { return generation != 0 && instances != nullptr; }
};

struct SelectionState {
  // Current time filter; nullopt means "whole trace".
  std::optional<ViewFilter> timeFilter;
  ThreadIdx selectedThread;
  std::optional<FunctionKey> selectedFunction;
  HoveredItem hovered;
  // WP-46 additions.
  std::optional<WaitSelection> selectedWait;
  NavigationRequest navigation;  // the last request; serial 0 after clear()
  PanelRequest panelRequest;     // the last request; serial 0 after clear()
  // WP-47.
  InstanceHighlight instanceHighlight;
};

// SelectedWait, Navigation and PanelRequest were added by WP-46 after All; All still means "everything".
enum class SelectionField : uint8_t {
  TimeFilter,
  SelectedThread,
  SelectedFunction,
  Hovered,
  All,
  SelectedWait,
  Navigation,
  PanelRequest,
  InstanceHighlight  // WP-47
};

// Main-thread only. Dispatch is synchronous, in subscription order. A callback may unsubscribe itself or any
// other subscriber: a subscriber removed before its turn in the current dispatch is not called. A callback
// may subscribe new subscribers: they first hear about the next publish. A callback may call a setter; that
// publishes re-entrantly (nested dispatch) and only when the value actually changed.
class SelectionBus {
 public:
  using SubscriptionId = uint64_t;
  using Callback = std::function<void(const SelectionState&, SelectionField)>;
  static constexpr SubscriptionId kNoSubscription = 0;

  // Main-thread only.
  SubscriptionId subscribe(Callback callback);
  // Main-thread only. Safe during dispatch; unknown ids are ignored.
  void unsubscribe(SubscriptionId id);
  // Main-thread only.
  size_t subscriberCount() const { return m_subscribers.size(); }

  // Main-thread only. Each setter publishes to subscribers only if the value changed.
  void setTimeFilter(std::optional<ViewFilter> filter);
  void setSelectedThread(ThreadIdx thread);
  void setSelectedFunction(std::optional<FunctionKey> function);
  void setHovered(const HoveredItem& item);
  // Main-thread only. Publishes only if the value changed.
  void setSelectedWait(std::optional<WaitSelection> wait);
  // Main-thread only. Always publishes (an event, not a value): assigns the next serial and returns it.
  uint64_t requestNavigation(ThreadIdx thread, uint64_t t0, uint64_t t1);
  // Main-thread only. Always publishes; assigns the next serial and returns it.
  uint64_t requestPanel(std::string name);
  // Main-thread only (WP-47). Highlights `instances` of `key` in the timeline. Always publishes (an event):
  // assigns and returns a new generation; `current` is reset. A null list clears the highlight (returns 0).
  uint64_t setInstanceHighlight(std::optional<FunctionKey> key,
                                std::shared_ptr<const std::vector<analysis::Instance>> instances);
  // Main-thread only (WP-47). Marks one instance of the active highlight as the current one (the generation
  // is unchanged). Publishes only if the value changed and a highlight is active.
  void setHighlightCurrent(std::optional<uint32_t> index);
  // Main-thread only. Resets everything (e.g. a new document) and publishes SelectionField::All once.
  void clear();

  // Main-thread only.
  const SelectionState& state() const { return m_state; }

 private:
  struct Subscriber {
    SubscriptionId id;
    Callback callback;
  };

  void publish(SelectionField field);

  SelectionState m_state;
  std::vector<Subscriber> m_subscribers;
  SubscriptionId m_nextId = 1;
  uint64_t m_nextSerial = 1;  // survives clear() so a new request never repeats an old serial
};

}  // namespace tachyon::viewer
