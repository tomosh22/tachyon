#include "analysis/scope_pairing.h"

#include "analysis/query_cache.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace tachyon::analysis {

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

  // tid -> thread by binary search over a flat sorted table, with a one-entry cache (events cluster).
  std::vector<std::pair<uint32_t, uint32_t>> tids;
  tids.reserve(m.tidToThread.size());
  for (const auto& [tid, idx] : m.tidToThread)
    if (idx.valid() && idx.get() < threadCount) tids.emplace_back(tid, idx.get());
  std::sort(tids.begin(), tids.end());

  std::vector<std::vector<uint32_t>> open(threadCount);  // indices into byThread[t]
  uint32_t lastTid = 0, lastIdx = kInvalidIdx;
  const std::vector<trace::ScopeEventRec>& events = m.scopeEvents;
  for (size_t i = 0; i < events.size(); ++i) {
    if ((i & 0xFFFFu) == 0 && cancel.requested()) return makeError(ErrorCode::Cancelled, "query cancelled");
    trace::ScopeEventRec e;  // packed record: copy before use
    std::memcpy(&e, &events[i], sizeof e);
    const uint32_t tid = e.tid;
    uint32_t t;
    if (lastIdx != kInvalidIdx && tid == lastTid) {
      t = lastIdx;
    } else {
      const auto it = std::lower_bound(tids.begin(), tids.end(), std::pair<uint32_t, uint32_t>{tid, 0});
      if (tid == 0 || it == tids.end() || it->first != tid) {
        ++out.droppedEvents;
        continue;
      }
      t = lastIdx = it->second;
      lastTid = tid;
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
