// The ETW real-time consumer of the live view (WP-65w): OpenTrace / ProcessTrace on a RUNNING session that
// was started with EVENT_TRACE_REAL_TIME_MODE next to its file mode (SessionConfig::realTime). A dedicated
// thread runs ProcessTrace; its callbacks copy the events into a LiveEtwSource and never wait.
//
// On non-Windows platforms start() returns Unsupported.
#pragma once

#include "capture/windows_etw/live_etw_source.h"
#include "core/result.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace tachyon::capture::etw {

// Thread-safe. The trace clock of a session started with Wnode.ClientContext = 1: QueryPerformanceCounter
// ticks (elsewhere: steady-clock nanoseconds, so tests have a clock).
uint64_t traceClockNow();
// Thread-safe. Ticks per second of traceClockNow().
uint64_t traceClockFrequency();

struct RealTimeConsumerStats {
  uint64_t events = 0;         // all events the callback saw
  uint64_t buffers = 0;        // ETW buffers processed
  double cpuSeconds = 0;       // cpu time of the ProcessTrace thread (GetThreadTimes: coarse, can read 0)
  double callbackSeconds = 0;  // wall time spent inside the event callbacks (the consumer's real cost)
  uint64_t perfFreq = 0;       // LogfileHeader.PerfFreq of the opened session (0 when unknown)
  uint32_t status = 0;         // ProcessTrace's return value once it returned
  bool finished = false;       // ProcessTrace returned
};

class RealTimeConsumer {
 public:
  // Thread-safe. Opens the running session `loggerName` for real-time consumption and starts the thread. The
  // source must outlive the consumer. NoPrivilege when not elevated, NotFound when the session does not
  // exist, Unsupported off Windows.
  static Result<std::unique_ptr<RealTimeConsumer>> start(const std::string& loggerName,
                                                         LiveEtwSource& source);
  // Thread-safe: calls stop(0).
  ~RealTimeConsumer();
  RealTimeConsumer(const RealTimeConsumer&) = delete;
  RealTimeConsumer& operator=(const RealTimeConsumer&) = delete;

  // Thread-safe. Call it after ControlTrace(FLUSH): waits until a buffer has reached the consumer (at most
  // `firstWait`: there may be nothing pending) and then no further buffer has for `quiet`, at most `maxWait`
  // in all. Events still sitting in the session's buffers are delivered to a real-time consumer by the
  // flush; a session that is stopped without one hands them over late or not at all.
  void waitQuiet(std::chrono::milliseconds quiet, std::chrono::milliseconds firstWait,
                 std::chrono::milliseconds maxWait);
  // Thread-safe, idempotent. Waits up to `drain` for ProcessTrace to return by itself (the session was
  // stopped, so it delivers the buffers that are left first), then closes the trace and joins the thread.
  void stop(std::chrono::milliseconds drain);
  // Thread-safe.
  RealTimeConsumerStats stats() const;

  struct Impl;  // internal: the ETW callbacks need to name it

 private:
  explicit RealTimeConsumer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

}  // namespace tachyon::capture::etw
