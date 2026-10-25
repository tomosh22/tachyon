#include "analysis/scope_pairing.h"

#include "analysis/query_cache.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace tachyon::analysis {

TidLookup::TidLookup(const model::TraceModel& m) {
  m_tids.reserve(m.tidToThread.size());
  for (const auto& [tid, idx] : m.tidToThread)
    if (idx.valid() && idx.get() < m.threads.size()) m_tids.emplace_back(tid, idx.get());
  std::sort(m_tids.begin(), m_tids.end());
}

uint32_t TidLookup::find(uint32_t tid) {
  if (m_lastIdx != kInvalidIdx && tid == m_lastTid) return m_lastIdx;
  if (tid == 0) return kInvalidIdx;
  const auto it = std::lower_bound(m_tids.begin(), m_tids.end(), std::pair<uint32_t, uint32_t>{tid, 0});
  if (it == m_tids.end() || it->first != tid) return kInvalidIdx;
  m_lastTid = tid;
  m_lastIdx = it->second;
  return m_lastIdx;
}

size_t PairedScopes::heapBytes() const {
  size_t bytes = sizeof(PairedScopes) + vectorBytes(byThread) + vectorBytes(orphanEndTs);
  for (const auto& v : byThread) bytes += vectorBytes(v);
  for (const auto& v : orphanEndTs) bytes += vectorBytes(v);
  return bytes;
}

Result<PairedScopes> pairScopes(const model::TraceModel& m, Cancel cancel) {
  const size_t threadCount = m.threads.size();
  PairedScopes out;
  out.byThread.resize(threadCount);
  out.orphanEndTs.resize(threadCount);

  TidLookup lookup(m);
  std::vector<std::vector<uint32_t>> open(threadCount);  // indices into byThread[t]
  const std::vector<trace::ScopeEventRec>& events = m.scopeEvents;
  for (size_t i = 0; i < events.size(); ++i) {
    if ((i & 0xFFFFu) == 0 && cancel.requested()) return makeError(ErrorCode::Cancelled, "query cancelled");
    trace::ScopeEventRec e;  // packed record: copy before use
    std::memcpy(&e, &events[i], sizeof e);
    const uint32_t t = lookup.find(e.tid);
    if (t == kInvalidIdx) {
      ++out.droppedEvents;
      continue;
    }
    std::vector<uint32_t>& stack = open[t];
    std::vector<ScopeInstance>& list = out.byThread[t];
    if (e.kind == trace::kScopeBegin) {
      ScopeInstance si;
      si.id = e.id;
      si.begin = e.ts;
      si.end = e.ts;
      si.depth = static_cast<uint32_t>(stack.size());
      si.callerAddr = e.caller_addr;
      si.data = e.data;
      si.color = e.color_rgba;
      si.unterminated = true;  // until its End arrives
      stack.push_back(static_cast<uint32_t>(list.size()));
      list.push_back(si);
    } else if (e.kind == trace::kScopeEnd) {
      if (stack.empty()) {
        out.orphanEndTs[t].push_back(e.ts);
        continue;
      }
      ScopeInstance& si = list[stack.back()];
      stack.pop_back();
      si.end = std::max(e.ts, si.begin);
      si.unterminated = false;
    }
  }
  const uint64_t traceEnd = m.header.end_ts;
  for (size_t t = 0; t < threadCount; ++t) {
    for (uint32_t idx : open[t]) {
      ScopeInstance& si = out.byThread[t][idx];
      si.end = std::max(traceEnd, si.begin);
      ++out.unterminatedCount;
    }
  }
  return out;
}

}  // namespace tachyon::analysis
