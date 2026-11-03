// Helpers shared by the exporters (internal to libs/export).
#pragma once

#include "analysis/query.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tachyon::exporters::detail {

// Thread-safe: pure function. Ticks of a clock running at `hz` to nanoseconds, rounded to nearest. Exact
// integer arithmetic while hz <= 1.8e10 (every real trace clock); a double estimate beyond that.
inline uint64_t ticksToNanos(uint64_t ticks, uint64_t hz) {
  if (hz == 1'000'000'000ull) return ticks;
  if (hz > 18'000'000'000ull)
    return static_cast<uint64_t>(std::llround(static_cast<double>(ticks) * 1e9 / static_cast<double>(hz)));
  const uint64_t q = ticks / hz, r = ticks % hz;
  return q * 1'000'000'000ull + (r * 1'000'000'000ull + hz / 2) / hz;
}

// Thread-safe: pure function. Signed difference ts - origin in nanoseconds.
inline int64_t signedNanos(uint64_t ts, uint64_t origin, uint64_t hz) {
  return ts >= origin ? static_cast<int64_t>(ticksToNanos(ts - origin, hz))
                      : -static_cast<int64_t>(ticksToNanos(origin - ts, hz));
}

// Thread-safe (const access). The selected threads, ascending and without duplicates. Validates the indices
// and that the thread states are built.
inline Result<std::vector<ThreadIdx>> selectThreads(const model::TraceModel& m,
                                                    const analysis::Filter& filter) {
  if (filter.subtreeRoot || filter.function)
    return makeError(ErrorCode::Unsupported,
                     "the exporters do not support Filter::subtreeRoot / Filter::function");
  if (m.header.clock_frequency_hz == 0)
    return makeError(ErrorCode::InvalidArgument, "trace clock frequency is 0");
  if (m.statesByThread.size() != m.threads.size() || m.samplesByThread.size() != m.threads.size())
    return makeError(ErrorCode::InvalidArgument, "thread states are not built (call buildThreadStates)");
  std::vector<ThreadIdx> out;
  if (filter.threads) {
    out = *filter.threads;
    for (ThreadIdx t : out)
      if (!t.valid() || t.get() >= m.threads.size())
        return makeError(ErrorCode::InvalidArgument, "thread index out of range");
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  } else {
    out.reserve(m.threads.size());
    for (uint32_t i = 0; i < m.threads.size(); ++i) out.emplace_back(i);
  }
  return out;
}

}  // namespace tachyon::exporters::detail
