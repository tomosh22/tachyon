// The kernel-facing layer of the perf backend (WP-70): perf_event_open, ring mmaps, ioctls and sysctls behind
// an interface, so the backend's session logic (plans, launch handshake, drain, conversion) is unit-tested
// with a fake that needs no privileges.
#pragma once

#include "capture/linux_perf/perf_abi.h"
#include "capture/linux_perf/ring_buffer.h"
#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::capture::perf {

// The perf_event_attr the backend asks for. Everything not listed is fixed (PERF_TYPE_SOFTWARE /
// PERF_COUNT_SW_CPU_CLOCK, freq mode, mmap + mmap2 + comm + comm_exec + task, sample_id_all, use_clockid with
// CLOCK_MONOTONIC_RAW, exclude_hv).
struct EventConfig {
  // CpuClock: the sampling event, with the mmap / comm / task side-band records.
  // Tracepoint: PERF_TYPE_TRACEPOINT with config = tracepointId, period 1. It carries only the sample_id
  // trailer: no mmap / comm / task records, which the cpu-clock event of the same ring already delivers (a
  // second copy would be a duplicate). Everything else below applies to both kinds.
  enum class Kind : uint8_t { CpuClock, Tracepoint };
  Kind kind = Kind::CpuClock;
  uint64_t tracepointId = 0;
  bool contextSwitch = false;  // CpuClock: also deliver PERF_RECORD_SWITCH / SWITCH_CPU_WIDE (WP-71 fallback)
  uint32_t samplingHz = 8000;
  uint64_t sampleType = kBackendSampleType;
  bool excludeKernel = false;  // no samples while in kernel mode (needed for unprivileged per-pid use)
  bool excludeCallchainKernel = false;  // user frames only in the callchain (kernel_stacks == false)
  bool inherit = false;                 // children / new threads of the target join the event
  bool disabled = true;                 // start disabled; enable explicitly (or on exec)
  bool enableOnExec = false;
  bool wantKernelBuildId = true;  // ask for build-ids in MMAP2 records (Linux 5.12+); retried without
  uint32_t wakeupBytes = 0;       // wake the poller when this many bytes are pending (0: default)
};

struct SysctlInfo {
  int paranoid = 2;
  int64_t maxSampleRate = 0;
  int cpuTimeMaxPercent = 0;
  int64_t mlockKb = 0;             // kernel.perf_event_mlock_kb
  bool hasPerfCapability = false;  // CAP_PERFMON or CAP_SYS_ADMIN in the effective set
};

struct OpenOutcome {
  int fd = -1;
  int err = 0;                 // errno when fd < 0
  bool kernelBuildId = false;  // the event delivers build-ids in MMAP2
};

// One mapped ring buffer of an event.
class IRing {
 public:
  virtual ~IRing() = default;
  // Not thread-safe. Appends the unread bytes to `out`.
  virtual RingStatus drain(std::vector<uint8_t>& out) = 0;
  // Thread-safe. A file descriptor to poll for POLLIN, or -1 when the ring cannot be polled (fakes).
  virtual int pollFd() const = 0;
  virtual uint64_t overruns() const = 0;
};

struct RingOutcome {
  std::unique_ptr<IRing> ring;
  int err = 0;
};

class IPerfSystem {
 public:
  virtual ~IPerfSystem() = default;
  // Thread-safe. perf_event_open(attr, pid, cpu, -1, FD_CLOEXEC). pid -1 + cpu >= 0: system-wide on that cpu;
  // pid >= 0 + cpu -1: that thread on any cpu.
  virtual OpenOutcome openEvent(const EventConfig& config, int pid, int cpu) = 0;
  // Thread-safe. mmaps 1 + dataPages pages of the event's fd.
  virtual RingOutcome mapRing(int fd, uint32_t dataPages) = 0;
  // Thread-safe. PERF_EVENT_IOC_ENABLE / DISABLE.
  virtual bool setEnabled(int fd, bool enabled) = 0;
  // Thread-safe. PERF_EVENT_IOC_SET_OUTPUT: the records of `fd` go into the ring mapped for `targetFd`
  // (both events must be bound to the same cpu).
  virtual bool setOutput(int fd, int targetFd) = 0;
  virtual void closeEvent(int fd) = 0;
  virtual std::vector<uint32_t> onlineCpus() = 0;
  virtual SysctlInfo sysctls() = 0;
  // Thread-safe. CLOCK_MONOTONIC_RAW in nanoseconds: the clock the events are stamped with.
  virtual uint64_t nowNs() = 0;
  // Thread-safe. Root of the proc filesystem (a fake directory in tests).
  virtual std::filesystem::path procRoot() { return "/proc"; }
  // Thread-safe. The tracefs directory that holds events/<system>/<event>/{format,id}: for the real layer the
  // first of /sys/kernel/tracing and /sys/kernel/debug/tracing that has events/sched/sched_switch; empty when
  // there is none (no tracefs, or not readable). A fake directory in tests; the default is "no tracefs".
  virtual std::filesystem::path tracefsRoot() { return {}; }
};

// Thread-safe. The real implementation over the perf_event_open syscall.
std::shared_ptr<IPerfSystem> makeLinuxPerfSystem();

// Thread-safe: pure function. The errno of a failed perf_event_open (or ring mmap when `mapping`) turned
// into an Error whose message says what to change: kernel.perf_event_paranoid, CAP_PERFMON,
// kernel.perf_event_max_sample_rate, perf_event_mlock_kb, the fd limit. NoPrivilege for EACCES / EPERM,
// NotFound for ESRCH, Unsupported for ENOENT / ENODEV / EOPNOTSUPP / ENOSYS, InvalidArgument for a sampling
// rate above the kernel limit, Internal otherwise.
Error describeOpenError(int err, const EventConfig& config, const SysctlInfo& sysctls, int pid, int cpu,
                        bool mapping = false);

// Thread-safe: pure function. The advice appended to throttle warnings.
std::string throttleAdvice(const SysctlInfo& sysctls, uint32_t samplingHz);

// Thread-safe. Parses a sysfs cpu list such as "0-3,5,8-9". Empty on malformed input.
std::vector<uint32_t> parseCpuList(const std::string& text);

}  // namespace tachyon::capture::perf
