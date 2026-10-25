// Helpers for the WP-53 tests: scope events on a HandModel and readable views of query results.
#pragma once

#include "aggregation_test_util.h"
#include "analysis/query.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tachyon::analysis::testutil {

// Name of a function key without the "game.exe!" module prefix (scopes are "scope:<id>").
inline std::string shortName(const QueryContext& ctx, const FunctionKey& key) {
  std::string n = functionKeyName(ctx, key);
  const std::string prefix = "game.exe!";
  if (n.compare(0, prefix.size(), prefix) == 0) n.erase(0, prefix.size());
  return n;
}

// Exclusive ticks per root-first path ("Main/A/scope:S/B") of every node of a top-down tree that has any.
inline std::map<std::string, uint64_t> pathsOf(const QueryContext& ctx, const NodeAgg& root) {
  std::map<std::string, uint64_t> out;
  std::vector<std::pair<const NodeAgg*, std::string>> work;
  work.emplace_back(&root, root.key == kRootKey ? std::string() : shortName(ctx, root.key));
  while (!work.empty()) {
    auto [node, path] = work.back();
    work.pop_back();
    if (node->exclusiveTicks != 0) out[path] += node->exclusiveTicks;
    for (const NodeAgg& c : node->children) {
      const std::string n = shortName(ctx, c.key);
      work.emplace_back(&c, path.empty() ? n : path + "/" + n);
    }
  }
  return out;
}

inline std::map<std::string, FunctionRow> rowsByName(const QueryContext& ctx,
                                                     const std::vector<FunctionRow>& rows) {
  std::map<std::string, FunctionRow> out;
  for (const FunctionRow& r : rows) out[shortName(ctx, r.key)] = r;
  return out;
}

// Appends scope events to a HandModel (strings interned in the model). Call sort() before finish().
class ScopeBuilder {
 public:
  explicit ScopeBuilder(HandModel& h) : m_h(h) {}
  void begin(uint64_t ts, const char* id, uint64_t caller, uint32_t tid = 100, const char* data = "",
             uint32_t color = 0xFFFFFFFFu) {
    trace::ScopeEventRec e{};
    e.ts = ts;
    e.tid = tid;
    e.kind = trace::kScopeBegin;
    e.id = m_h.m.strings.intern(id);
    e.data = m_h.m.strings.intern(data);
    e.color_rgba = color;
    e.caller_addr = caller;
    m_h.m.scopeEvents.push_back(e);
  }
  void end(uint64_t ts, uint32_t tid = 100) {
    trace::ScopeEventRec e{};
    e.ts = ts;
    e.tid = tid;
    e.kind = trace::kScopeEnd;
    m_h.m.scopeEvents.push_back(e);
  }
  void fiber(uint64_t ts, uint32_t thread, uint8_t kind, uint64_t fiberId, uint64_t other = 0) {
    model::FiberSwitch f;
    f.ts = ts;
    f.thread = ThreadIdx(thread);
    f.kind = kind;
    f.fiber_id = fiberId;
    f.other_fiber_id = other;
    m_h.m.fiberSwitches.push_back(f);
  }
  // Stable sort by ts (the model guarantees that order).
  void sort() {
    std::stable_sort(
        m_h.m.scopeEvents.begin(), m_h.m.scopeEvents.end(),
        [](const trace::ScopeEventRec& a, const trace::ScopeEventRec& b) { return a.ts < b.ts; });
    std::stable_sort(m_h.m.fiberSwitches.begin(), m_h.m.fiberSwitches.end(),
                     [](const model::FiberSwitch& a, const model::FiberSwitch& b) { return a.ts < b.ts; });
  }

 private:
  HandModel& m_h;
};

}  // namespace tachyon::analysis::testutil
