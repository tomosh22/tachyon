// Choosing how the perf backend records scheduler events (WP-71).
//
// Preferred path ("tracepoint"): sched:sched_switch and sched:sched_waking (sched_wakeup on kernels without
// it) as PERF_TYPE_TRACEPOINT events with RAW|CALLCHAIN|TIME|TID|CPU, redirected into the ring of the cpu's
// sampling event. They give the kernel's task state (wait reasons), the blocking stack of the task that
// switches out and the waker's stack. The tracepoint ids and payload layouts are read from tracefs.
//
// Fallback ("context_switch"): attr.context_switch on the sampling events, PERF_RECORD_SWITCH /
// SWITCH_CPU_WIDE side-band records. Works without tracefs, but carries neither wait reasons, nor stacks, nor
// wakeup events; the capture says so in a warning.
#pragma once

#include "capture/linux_perf/sched_format.h"
#include "capture/linux_perf/spool.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::capture::perf {

struct SchedPlan {
  SchedInfo info;
  uint32_t switchId = 0;  // tracepoint ids (tracepoint mode)
  uint32_t wakeId = 0;
  std::vector<std::string> warnings;  // for the capture: what was lost and why

  bool tracepoints() const { return info.mode == "tracepoint"; }
  bool contextSwitch() const { return info.mode == "context_switch"; }
  // Tracepoint events opened next to each cpu-clock event.
  uint32_t tracepointEvents() const {
    return tracepoints() ? (switchId != 0 ? 1u : 0u) + (wakeId != 0 ? 1u : 0u) : 0u;
  }
};

// Thread-safe: reads files. `wantSwitch` / `wantWake` come from the start options (cswitch / readythread).
// Uses the tracepoint path when `tracefsRoot` (events/sched/... below it) holds usable formats for every
// wanted event; otherwise the fallback with the reason in info.reason and a warning. Wanting nothing gives
// mode "none". A missing wakeup tracepoint alone keeps the switch tracepoint and adds a warning.
SchedPlan planSched(const std::filesystem::path& tracefsRoot, bool wantSwitch, bool wantWake);

// Thread-safe: pure function. The context_switch plan (or "none" when switches are not wanted), used when the
// tracepoint events could not be opened. `reason` goes into info.reason and the warning.
SchedPlan fallbackSchedPlan(bool wantSwitch, bool wantWake, const std::string& reason);

}  // namespace tachyon::capture::perf
