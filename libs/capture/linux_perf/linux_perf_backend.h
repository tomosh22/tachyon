// LinuxPerfBackend: ICaptureBackend over perf_event_open (WP-70). Linux only (the library is not built
// elsewhere).
//
// WHAT IS RECORDED. One software event per ring: PERF_TYPE_SOFTWARE / PERF_COUNT_SW_CPU_CLOCK in frequency
// mode (freq=1, sample_freq=samplingHz), sample_type IP|TID|TIME|CALLCHAIN|CPU, use_clockid with
// CLOCK_MONOTONIC_RAW (the trace clock), mmap + mmap2 + comm (+ comm_exec) + task + sample_id_all, build-id
// in MMAP2 when the kernel can deliver it. exclude_callchain_kernel is set when `kernelStacks` is off.
//
// PLANS (how the events are attached; chosen at start()):
//   system-wide  one event per online cpu with pid = -1 (needs CAP_PERFMON, CAP_SYS_ADMIN or
//                kernel.perf_event_paranoid <= 0). Used for System mode, and for Launch / Attach whenever it
//                is allowed. Every process is sampled and the converter keeps the target tree. This is the
//                most robust plan: one ring per cpu is only ever written by that cpu, nothing depends on
//                racing against thread creation, and fork / exec / mmap of the target are all in the stream.
//   per-thread   for each thread of the target one event on EVERY cpu, with inherit = 1 (threads and
//   processes
//                the target creates later get a copy of the event on the same cpu and write into its ring).
//                Used for Launch / Attach when system-wide sampling is not permitted (an unprivileged
//                `perf record -p` equivalent), with exclude_kernel set when kernel.perf_event_paranoid >= 2.
//                Why per cpu: the kernel refuses to mmap an inherited event that is not bound to a cpu
//                (EINVAL: every child would write into one ring from any cpu). So there is one ring per cpu,
//                owned by the first task's event of that cpu; the events of the other threads of an attach
//                redirect into it with PERF_EVENT_IOC_SET_OUTPUT. A ring is thus only written by its cpu.
//                Cost: threads x cpus file descriptors for an attach (a launch has one task: cpus
//                descriptors). Limits: threads created between listing /proc/<pid>/task and opening their
//                events are picked up by up to four re-scans, not after that unless a thread of the target
//                creates them (inherit); a thread that exits mid-open is skipped.
//
// SPOOL AND CONVERSION. A drain thread copies the rings to a spool directory (<outPath>.perfspool, see
// spool.h) while the capture runs; stop() / flush() convert it to the .tachyon file (perf_converter.h).
// CaptureResult.rawPath is the spool directory (deleted when keepRawSpool is off).
//
// LAUNCH. The child is forked first and parks on a pipe before exec; the events are opened and enabled
// (enable_on_exec for the per-thread plan), the /proc rundown is written, and only then is the child
// released, so the first instruction of the target, its exec and the dynamic loader are inside the capture
// (launch.h). With startImmediately == false the child stays parked (launchPending()) and the next start()
// call begins recording and releases it; that call's other fields are ignored. stop() on a parked launch
// kills the child and reports NotFound. A released target is never killed by stop().
//
// CIRCULAR (circularMb > 0). The data segments rotate and the oldest are deleted to stay within the budget;
// meta.bin keeps the module / thread history. flush(name, seconds) seals the current segment, converts the
// last `seconds` (0: everything retained) to <dir of outPath>/<name>.tachyon without stopping the capture
// (there is no gap, unlike the ETW restart) and discards the consumed segments, so the next flush starts
// after this one.
//
// SCHEDULER EVENTS (WP-71, sched_setup.h). Preferred: sched:sched_switch and sched:sched_waking tracepoint
// events next to every sampling event, writing into the same ring; they give wait reasons, the blocking stack
// of the task that switches out and the waker's stack. Without usable tracepoints (no tracefs, not
// readable, the open is refused) the sampling events ask for PERF_RECORD_SWITCH instead: switches without
// wait reasons, stacks or wakeups, with a warning. CaptureOptions.stackCSwitch / stackReady turn the two
// kinds of events on and off. The tracepoints are used in the system-wide plan only: a task-bound
// sched_switch event misses the switches into the task and a task-bound sched_waking event the wakeups by
// other tasks, so the per-thread plan records the task-bound PERF_RECORD_SWITCH out / in records.
//
// LOST / THROTTLE. PERF_RECORD_LOST counts go to CaptureResult.eventsLost. PERF_RECORD_THROTTLE periods are
// reported as warnings in the trace's CaptureInfo (and the log) with the sysctl guidance of throttleAdvice();
// they are not added to eventsLost because the kernel does not say how many samples a throttle cost.
#pragma once

#include "capture/capture_backend.h"
#include "capture/linux_perf/drain.h"
#include "capture/linux_perf/launch.h"
#include "capture/linux_perf/perf_converter.h"
#include "capture/linux_perf/perf_system.h"
#include "capture/linux_perf/retention.h"
#include "capture/linux_perf/sched_setup.h"
#include "capture/windows_etw/session_lifecycle.h"
#include "capture/windows_etw/target_watch.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tachyon::capture {

struct LinuxPerfBackendConfig {
  std::shared_ptr<perf::IPerfSystem> system;  // null: the real perf_event_open layer
  bool keepRawSpool = true;                   // keep the spool directory next to the output (rawPath)
  // How long a flush with a long window keeps raising the retention horizon for later flushes (retention.h).
  uint64_t horizonMemoryNs = perf::kDefaultHorizonMemoryNs;
  bool forcePerThread = false;  // never try the system-wide plan (tests, unprivileged emulation)
  uint32_t ringPages = 256;     // data pages per ring, a power of two (1 MiB with 4 KiB pages)
  std::string agentVersion = "tachyon-agent";
  std::chrono::milliseconds drainInterval{10};
  // Where the scheduler tracepoint formats are read (events/sched/...). Empty: IPerfSystem::tracefsRoot().
  std::filesystem::path tracefsRoot;
  // Never use the sched tracepoints; record context switches from PERF_RECORD_SWITCH (tests, kernels where
  // the tracepoints are known to misbehave).
  bool forceContextSwitchFallback = false;
};

// What the last start / stop observed; for tests, logs and reports.
struct LinuxPerfDiagnostics {
  std::string plan;       // "system-wide" | "per-thread"
  std::string schedPath;  // "tracepoint" | "context_switch" | "none"
  uint32_t rings = 0;
  uint32_t ringPages = 0;  // pages actually mapped (after any reduction)
  bool kernelBuildId = false;
  bool excludeKernel = false;
  perf::SysctlInfo sysctls;
  perf::DrainStats drain;
  perf::ConvertStats convert;
  std::vector<std::string> warnings;
  uint32_t rootPid = 0;
  std::vector<uint32_t> targetPids;
  uint32_t flushCount = 0;
  std::filesystem::path spoolDir;
};

// Thread-safe: all members may be called from any thread. start / stop / flush are serialised through the
// session phase logic (a second call while one is in flight gets Busy). The progress callback is invoked
// without internal locks held, from the thread calling stop() / flush().
class LinuxPerfBackend final : public ICaptureBackend, public ITargetWatch {
 public:
  explicit LinuxPerfBackend(LinuxPerfBackendConfig config = {});
  ~LinuxPerfBackend() override;

  // Probes perf_event_open (software cpu-clock, system-wide on the first online cpu). NoPrivilege (with the
  // paranoid / capability advice) when the kernel refuses.
  Result<void> checkPrivileges() override;
  Result<void> start(const CaptureOptions& options) override;
  Result<CaptureResult> stop() override;
  // Circular mode only. `seconds` == 0 keeps everything the retained segments hold.
  Result<CaptureResult> flush(std::string name, uint32_t seconds) override;
  CaptureState state() const override;
  void setProgressCallback(std::function<void(CaptureState, float)> callback) override;

  // Thread-safe. A copy of the diagnostics.
  LinuxPerfDiagnostics diagnostics() const;

  // ITargetWatch. Thread-safe.
  bool launchPending() const override;
  bool targetExited() const override;
  uint32_t rootPid() const override;

 private:
  struct Slot {
    int fd = -1;
    int cpu = -1;
    std::unique_ptr<perf::IRing> ring;  // null for events whose output is redirected into another ring
  };
  struct Session;

  void notify(CaptureState state, float progress);
  Result<void> beginRecording(const CaptureOptions& options, std::unique_ptr<perf::LaunchedProcess> child);
  Result<void> openPlan(Session& session, const CaptureOptions& options, bool systemWide);
  // Opens the events of the chosen plan (system-wide, else per-thread) for the session's sched plan.
  Result<void> openEvents(Session& session, const CaptureOptions& options);
  // Tracepoint sched events next to the cpu-clock event `ownerFd` of (pid, cpu): their records go into its
  // ring.
  Result<void> openSchedEvents(Session& session, const perf::EventConfig& base, int pid, int cpu,
                               int ownerFd);
  Result<void> openSlot(Session& session, const perf::EventConfig& cfg, int pid, int cpu);
  // Per-thread plan: opens `tid` on every cpu; the first task's event of a cpu owns that cpu's ring, the
  // others write into it (PERF_EVENT_IOC_SET_OUTPUT).
  // Makes sure threads x cpus descriptors (plus slack) fit RLIMIT_NOFILE: raises the soft limit up to the
  // hard one, else fails early with the number needed.
  Result<void> ensureDescriptors(size_t threads, size_t cpus);
  Result<void> openTask(Session& session, const perf::EventConfig& cfg, int tid);
  void teardown(Session& session);
  perf::ConvertOptions convertOptionsFor(const CaptureOptions& options, const Session& session) const;
  void reapFinished();

  mutable std::mutex m_mutex;
  LinuxPerfBackendConfig m_config;
  std::shared_ptr<perf::IPerfSystem> m_system;
  CaptureState m_state = CaptureState::Idle;
  etw::SessionLifecycle m_lifecycle;
  CaptureOptions m_options;
  std::unique_ptr<Session> m_session;
  std::unique_ptr<perf::LaunchedProcess> m_child;              // launched root (parked or running)
  std::vector<std::unique_ptr<perf::LaunchedProcess>> m_reap;  // released targets, reaped when they exit
  bool m_pending = false;
  std::chrono::steady_clock::time_point m_startTime;
  std::function<void(CaptureState, float)> m_callback;
  LinuxPerfDiagnostics m_diag;
  perf::RetentionHorizon m_horizon;  // retention horizon of the flushes of this capture (m_mutex)
};

}  // namespace tachyon::capture
