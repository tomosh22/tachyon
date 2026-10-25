// SelectionBus: the viewer's shared "what is selected" state; panels subscribe and are told what changed.
#pragma once

#include "core/strong_id.h"
#include "symbols/function_key.h"

#include <cstdint>
#include <functional>
#include <optional>
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

struct SelectionState {
  // Current time filter; nullopt means "whole trace".
  std::optional<ViewFilter> timeFilter;
  ThreadIdx selectedThread;
  std::optional<FunctionKey> selectedFunction;
  HoveredItem hovered;
};

enum class SelectionField : uint8_t { TimeFilter, SelectedThread, SelectedFunction, Hovered, All };

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
};

}  // namespace tachyon::viewer
