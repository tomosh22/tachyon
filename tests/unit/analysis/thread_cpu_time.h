// Cancellation-latency measurement for the cancellation tests.
//
// The requirement under test is a bound on the *work* a query does after its cancel flag is raised (50 ms,
// asserted at 2x). Wall-clock time also counts every moment the scheduler keeps the query thread off the cpu,
// which on a shared or loaded machine (ctest -j, other builds) is arbitrary and says nothing about the code
// under test. A query that is being cancelled never blocks, so the cpu time its thread spends between the
// flag store and its return is exactly the latency it would have on an idle machine.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace tachyon::analysis::testutil {

// CPU time consumed so far by the thread behind `handle`, in ns (0 when it cannot be read).
inline int64_t threadCpuNs(std::thread::native_handle_type handle) {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(handle, &created, &exited, &kernel, &user)) return 0;
  const auto ticks = [](const FILETIME& f) {
    return static_cast<int64_t>((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime);
  };
  return (ticks(kernel) + ticks(user)) * 100;  // 100 ns units
#else
  clockid_t clock;
  timespec ts;
  if (pthread_getcpuclockid(handle, &clock) != 0 || clock_gettime(clock, &ts) != 0) return 0;
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

// CPU time consumed so far by the calling thread, in ns.
inline int64_t currentThreadCpuNs() {
#ifdef _WIN32
  return threadCpuNs(GetCurrentThread());
#else
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

// The query thread's cpu time around the store of its cancel flag.
struct RaisePoint {
  int64_t cpuBefore = 0;
  int64_t cpuAfter = 0;
  // The thread's cpu time at the store. The two reads bracket it; if the raising thread was itself
  // descheduled between them the bracket is wide and the query ran on in it, so only the part after the
  // second read is certain to count; otherwise the (conservative) first read is used.
  int64_t cpuAtRaise() const { return cpuAfter - cpuBefore > 1'000'000 ? cpuAfter : cpuBefore; }
};

// Raises `flag` and records the cpu time of the `worker` thread around it. Call it before joining `worker`.
inline RaisePoint raiseFlag(std::thread& worker, std::atomic<bool>& flag) {
  RaisePoint p;
  p.cpuBefore = threadCpuNs(worker.native_handle());
  flag.store(true);
  p.cpuAfter = threadCpuNs(worker.native_handle());
  return p;
}

// Cpu milliseconds the worker spent from the flag store to `workerEndCpuNs` (its own currentThreadCpuNs() at
// the moment it returned).
inline double cpuLatencyMs(const RaisePoint& raised, int64_t workerEndCpuNs) {
  return static_cast<double>(std::max<int64_t>(0, workerEndCpuNs - raised.cpuAtRaise())) / 1e6;
}

}  // namespace tachyon::analysis::testutil
