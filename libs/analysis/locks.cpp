// Lock summary (WP-53): per lock address contention from the paired lock-wait scopes.
#include "analysis/locks.h"
#include "analysis/detail/query_support.h"
#include "analysis/fibers.h"
#include "analysis/query_cache.h"
#include "analysis/scope_pairing.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace tachyon::analysis {

using namespace detail;

bool parseLockAddress(std::string_view text, uint64_t& address) {
  size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;
  uint64_t v = 0;
  size_t digits = 0;
  for (; i < text.size(); ++i, ++digits) {
    const char c = text[i];
    unsigned d;
    if (c >= '0' && c <= '9') {
      d = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<unsigned>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<unsigned>(c - 'A') + 10;
    } else {
      break;
    }
    if (digits >= 16) return false;  // more than 64 bits
    v = (v << 4) | d;
  }
  if (digits == 0) return false;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\0')) ++i;
  if (i != text.size()) return false;  // trailing garbage
  address = v;
  return true;
}

namespace {

size_t summaryBytes(const LockSummary& s) {
  size_t bytes = sizeof(LockSummary) + 2 * kAllocOverhead + 32 + vectorBytes(s.locks);
  for (const LockRow& r : s.locks) bytes += vectorBytes(r.topThreads);
  return bytes;
}

struct Acc {
  LockRow row;
  std::unordered_map<uint32_t, LockWaiter> waiters;
};

}  // namespace

Result<std::shared_ptr<const LockSummary>> lockSummaryShared(const QueryContext& ctx, const Filter& filter,
                                                             Cancel cancel) {
  auto rf = resolveFilterBasic(ctx, filter);
  if (!rf) return rf.error();
  QueryKey key;
  key.kind = QueryKind::LockSummary;
  key.filter.t0 = rf->canonical.t0;
  key.filter.t1 = rf->canonical.t1;
  key.filter.threads = rf->canonical.threads;
  key.options = rf->options;
  return cachedOrCompute<LockSummary>(
      ctx, key,
      [&]() -> Result<LockSummary> {
        LockSummary out;
        if (rf->t0 >= rf->t1) return out;
        auto paired = pairedScopesFor(ctx, cancel);
        if (!paired) return paired.error();
        const PairedScopes& ps = **paired;
        const model::TraceModel& m = ctx.model();
        std::unordered_map<uint64_t, Acc> locks;
        for (ThreadIdx t : rf->threads) {
          if (cancel.requested()) return cancelledError();
          if (t.get() >= ps.byThread.size()) continue;
          const std::vector<ScopeInstance>& list = ps.byThread[t.get()];
          auto it = std::lower_bound(list.begin(), list.end(), rf->t0,
                                     [](const ScopeInstance& s, uint64_t ts) { return s.begin < ts; });
          size_t scanned = 0;
          for (; it != list.end() && it->begin < rf->t1; ++it) {
            if ((++scanned & 0xFFFFu) == 0 && cancel.requested()) return cancelledError();
            if (it->color != kLockColorSentinel) continue;
            uint64_t address;
            if (!parseLockAddress(m.strings.get(it->data), address)) {
              ++out.unparsedWaits;
              continue;
            }
            Acc& a = locks[address];
            a.row.address = address;
            const uint64_t wait = it->end - it->begin;
            ++a.row.contentionCount;
            a.row.totalWaitTicks += wait;
            a.row.maxWaitTicks = std::max(a.row.maxWaitTicks, wait);
            if (it->unterminated) ++a.row.unterminated;
            if (m.strings.get(it->id) != kLockReservedId &&
                (!a.row.name.valid() || it->id.get() < a.row.name.get()))
              a.row.name = it->id;
            LockWaiter& w = a.waiters[t.get()];
            w.thread = t;
            ++w.waits;
            w.waitTicks += wait;
          }
        }
        out.locks.reserve(locks.size());
        for (auto& [address, a] : locks) {
          LockRow row = std::move(a.row);
          row.topThreads.reserve(a.waiters.size());
          for (auto& [tid, w] : a.waiters) row.topThreads.push_back(w);
          std::sort(row.topThreads.begin(), row.topThreads.end(),
                    [](const LockWaiter& x, const LockWaiter& y) {
                      if (x.waitTicks != y.waitTicks) return x.waitTicks > y.waitTicks;
                      if (x.waits != y.waits) return x.waits > y.waits;
                      return x.thread < y.thread;
                    });
          if (row.topThreads.size() > kMaxLockWaiters) row.topThreads.resize(kMaxLockWaiters);
          row.topThreads.shrink_to_fit();
          out.locks.push_back(std::move(row));
        }
        std::sort(out.locks.begin(), out.locks.end(), [](const LockRow& x, const LockRow& y) {
          if (x.totalWaitTicks != y.totalWaitTicks) return x.totalWaitTicks > y.totalWaitTicks;
          return x.address < y.address;
        });
        return out;
      },
      summaryBytes);
}

Result<LockSummary> lockSummary(const QueryContext& ctx, const Filter& filter, Cancel cancel) {
  auto r = lockSummaryShared(ctx, filter, cancel);
  if (!r) return r.error();
  return **r;
}

}  // namespace tachyon::analysis
