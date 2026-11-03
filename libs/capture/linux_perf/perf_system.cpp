#include "capture/linux_perf/perf_system.h"

#include <linux/perf_event.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace tachyon::capture::perf {

namespace {

std::string errnoText(int err) {
  return std::string(std::strerror(err)) + " (errno " + std::to_string(err) + ")";
}

bool readFileText(const char* path, std::string& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

int64_t readSysctl(const char* path, int64_t fallback) {
  std::string text;
  if (!readFileText(path, text)) return fallback;
  char* end = nullptr;
  const long long v = std::strtoll(text.c_str(), &end, 10);
  return end == text.c_str() ? fallback : v;
}

bool hasCapability() {
  constexpr int kCapSysAdmin = 21;
  constexpr int kCapPerfmon = 38;
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("CapEff:", 0) == 0) {
      const uint64_t caps = std::strtoull(line.c_str() + 7, nullptr, 16);
      return ((caps >> kCapPerfmon) & 1U) != 0 || ((caps >> kCapSysAdmin) & 1U) != 0;
    }
  }
  return false;
}

class MmapRing final : public IRing {
 public:
  MmapRing(int fd, void* base, size_t mapBytes, const uint8_t* data, uint64_t dataSize, uint64_t* head,
           uint64_t* tail)
      : m_fd(fd), m_base(base), m_mapBytes(mapBytes), m_consumer(data, dataSize, head, tail) {}
  ~MmapRing() override { ::munmap(m_base, m_mapBytes); }
  RingStatus drain(std::vector<uint8_t>& out) override { return m_consumer.drain(out); }
  int pollFd() const override { return m_fd; }
  uint64_t overruns() const override { return m_consumer.overruns(); }

 private:
  int m_fd;
  void* m_base;
  size_t m_mapBytes;
  RingConsumer m_consumer;
};

class LinuxPerfSystem final : public IPerfSystem {
 public:
  OpenOutcome openEvent(const EventConfig& config, int pid, int cpu) override {
    OpenOutcome out;
    perf_event_attr attr{};
    attr.size = sizeof(attr);
    const bool tracepoint = config.kind == EventConfig::Kind::Tracepoint;
    if (tracepoint) {
      attr.type = PERF_TYPE_TRACEPOINT;
      attr.config = config.tracepointId;
      attr.sample_period = 1;  // every occurrence
    } else {
      attr.type = PERF_TYPE_SOFTWARE;
      attr.config = PERF_COUNT_SW_CPU_CLOCK;
      attr.freq = 1;
      attr.sample_freq = config.samplingHz;
    }
    attr.sample_type = config.sampleType;
    attr.disabled = config.disabled ? 1 : 0;
    attr.inherit = config.inherit ? 1 : 0;
    attr.exclude_kernel = config.excludeKernel ? 1 : 0;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = (config.excludeCallchainKernel || config.excludeKernel) ? 1 : 0;
    if (!tracepoint) {
      attr.mmap = 1;
      attr.mmap2 = 1;
      attr.comm = 1;
      attr.comm_exec = 1;
      attr.task = 1;
      attr.context_switch = config.contextSwitch ? 1 : 0;
    }
    attr.sample_id_all = 1;
    attr.use_clockid = 1;
    attr.clockid = CLOCK_MONOTONIC_RAW;
    attr.enable_on_exec = config.enableOnExec ? 1 : 0;
    if (config.wakeupBytes != 0) {
      attr.watermark = 1;
      attr.wakeup_watermark = config.wakeupBytes;
    }
    attr.build_id = (config.wantKernelBuildId && !tracepoint) ? 1 : 0;
    long fd = ::syscall(SYS_perf_event_open, &attr, pid, cpu, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0 && errno == EINVAL && attr.build_id != 0) {  // kernels before 5.12 reject the build_id bit
      attr.build_id = 0;
      fd = ::syscall(SYS_perf_event_open, &attr, pid, cpu, -1, PERF_FLAG_FD_CLOEXEC);
    }
    if (fd < 0) {
      out.err = errno;
      return out;
    }
    out.fd = static_cast<int>(fd);
    out.kernelBuildId = attr.build_id != 0;
    return out;
  }

  RingOutcome mapRing(int fd, uint32_t dataPages) override {
    RingOutcome out;
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    const size_t bytes = (static_cast<size_t>(dataPages) + 1) * page;
    void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
      out.err = errno;
      return out;
    }
    auto* mp = static_cast<perf_event_mmap_page*>(base);
    const uint64_t dataOffset = mp->data_offset != 0 ? mp->data_offset : page;
    const uint64_t dataSize = mp->data_size != 0 ? mp->data_size : static_cast<uint64_t>(dataPages) * page;
    out.ring = std::make_unique<MmapRing>(fd, base, bytes, static_cast<const uint8_t*>(base) + dataOffset,
                                          dataSize, reinterpret_cast<uint64_t*>(&mp->data_head),
                                          reinterpret_cast<uint64_t*>(&mp->data_tail));
    return out;
  }

  bool setEnabled(int fd, bool enabled) override {
    return ::ioctl(fd, enabled ? PERF_EVENT_IOC_ENABLE : PERF_EVENT_IOC_DISABLE, 0) == 0;
  }

  bool setOutput(int fd, int targetFd) override {
    return ::ioctl(fd, PERF_EVENT_IOC_SET_OUTPUT, targetFd) == 0;
  }

  void closeEvent(int fd) override { ::close(fd); }

  std::vector<uint32_t> onlineCpus() override {
    std::string text;
    std::vector<uint32_t> cpus;
    if (readFileText("/sys/devices/system/cpu/online", text)) cpus = parseCpuList(text);
    if (cpus.empty()) {
      const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
      for (long i = 0; i < (n > 0 ? n : 1); ++i) cpus.push_back(static_cast<uint32_t>(i));
    }
    return cpus;
  }

  SysctlInfo sysctls() override {
    SysctlInfo s;
    s.paranoid = static_cast<int>(readSysctl("/proc/sys/kernel/perf_event_paranoid", 2));
    s.maxSampleRate = readSysctl("/proc/sys/kernel/perf_event_max_sample_rate", 0);
    s.cpuTimeMaxPercent = static_cast<int>(readSysctl("/proc/sys/kernel/perf_cpu_time_max_percent", 0));
    s.mlockKb = readSysctl("/proc/sys/kernel/perf_event_mlock_kb", 0);
    s.hasPerfCapability = hasCapability();
    return s;
  }

  std::filesystem::path tracefsRoot() override {
    for (const char* candidate : {"/sys/kernel/tracing", "/sys/kernel/debug/tracing"}) {
      std::error_code ec;
      if (std::filesystem::exists(std::filesystem::path(candidate) / "events/sched/sched_switch/format", ec))
        return candidate;
    }
    return {};
  }

  uint64_t nowNs() override {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
  }
};

}  // namespace

std::shared_ptr<IPerfSystem> makeLinuxPerfSystem() {
  return std::make_shared<LinuxPerfSystem>();
}

std::vector<uint32_t> parseCpuList(const std::string& text) {
  std::vector<uint32_t> out;
  size_t i = 0;
  auto number = [&](uint32_t& v) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
    uint64_t acc = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      acc = acc * 10 + static_cast<uint64_t>(text[i] - '0');
      if (acc > 1'000'000) return false;
      ++i;
    }
    v = static_cast<uint32_t>(acc);
    return true;
  };
  while (i < text.size()) {
    if (text[i] == '\n' || text[i] == ' ') {
      ++i;
      continue;
    }
    uint32_t lo = 0;
    if (!number(lo)) return {};
    uint32_t hi = lo;
    if (i < text.size() && text[i] == '-') {
      ++i;
      if (!number(hi) || hi < lo) return {};
    }
    for (uint32_t c = lo; c <= hi; ++c) out.push_back(c);
    if (i < text.size() && text[i] == ',') ++i;
  }
  return out;
}

Error describeOpenError(int err, const EventConfig& config, const SysctlInfo& sys, int pid, int cpu,
                        bool mapping) {
  std::string where = pid >= 0 && cpu < 0 ? "thread " + std::to_string(pid) : "cpu " + std::to_string(cpu);
  const std::string what = (mapping ? "mmap of the perf ring buffer for " : "perf_event_open for ") + where +
                           " failed: " + errnoText(err);
  const std::string paranoid = "kernel.perf_event_paranoid is " + std::to_string(sys.paranoid);
  if (mapping) {
    if (err == EPERM || err == ENOMEM || err == EAGAIN) {
      return makeError(
          ErrorCode::Internal,
          what + "; the locked-memory budget is exhausted (kernel.perf_event_mlock_kb is " +
              std::to_string(sys.mlockKb) +
              " KiB per user without CAP_IPC_LOCK/CAP_PERFMON, RLIMIT_MEMLOCK also applies): raise "
              "it with `sysctl kernel.perf_event_mlock_kb=...` or run with CAP_PERFMON");
    }
    return makeError(ErrorCode::Internal, what);
  }
  switch (err) {
    case EACCES:
    case EPERM:
      return makeError(
          ErrorCode::NoPrivilege,
          what + "; " + paranoid +
              (cpu >= 0 ? ". System-wide sampling needs CAP_PERFMON (or CAP_SYS_ADMIN, or root) or "
                          "`sysctl kernel.perf_event_paranoid=-1`"
                        : ". Sampling a process you own needs kernel.perf_event_paranoid <= 2 (kernel "
                          "stacks: <= 1); "
                          "CAP_PERFMON lifts the restriction") +
              ". In a container, perf_event_open may also be blocked by seccomp (docker: --cap-add=PERFMON "
              "--security-opt seccomp=unconfined)");
    case ESRCH: return makeError(ErrorCode::NotFound, what + "; the target process has exited");
    case ENOENT:
    case ENODEV:
    case EOPNOTSUPP:
    case ENOSYS:
      return makeError(ErrorCode::Unsupported,
                       what +
                           "; this kernel or virtual machine does not provide the software cpu-clock event "
                           "(is CONFIG_PERF_EVENTS enabled, and perf_event_open not filtered by seccomp?)");
    case EINVAL:
      if (sys.maxSampleRate > 0 && config.samplingHz > sys.maxSampleRate) {
        return makeError(
            ErrorCode::InvalidArgument,
            what + "; sampling_hz " + std::to_string(config.samplingHz) +
                " exceeds kernel.perf_event_max_sample_rate (" + std::to_string(sys.maxSampleRate) +
                "): lower sampling_hz or raise it with `sysctl kernel.perf_event_max_sample_rate=" +
                std::to_string(config.samplingHz * 2) +
                "` (the kernel also lowers it by itself when perf_cpu_time_max_percent, now " +
                std::to_string(sys.cpuTimeMaxPercent) + ", is exceeded)");
      }
      return makeError(
          ErrorCode::InvalidArgument,
          what + "; the kernel rejected the event attributes (kernel too old for this configuration?)");
    case EMFILE:
    case ENFILE:
      return makeError(
          ErrorCode::Internal,
          what + "; out of file descriptors: one is used per cpu (or per thread); raise `ulimit -n`");
    case E2BIG: return makeError(ErrorCode::Unsupported, what + "; perf_event_attr size mismatch");
    case ENOSPC:
      return makeError(ErrorCode::Internal, what + "; hardware or software counter resources are exhausted");
    default: return makeError(ErrorCode::Internal, what);
  }
}

std::string throttleAdvice(const SysctlInfo& sys, uint32_t samplingHz) {
  return "the kernel throttled sampling (event rate above kernel.perf_event_max_sample_rate, now " +
         std::to_string(sys.maxSampleRate) +
         ", or sampling used more than kernel.perf_cpu_time_max_percent, now " +
         std::to_string(sys.cpuTimeMaxPercent) +
         ", of the cpu time); samples are missing while throttled. Lower sampling_hz (" +
         std::to_string(samplingHz) +
         " requested) or raise the limits with `sysctl kernel.perf_event_max_sample_rate=N` and "
         "`sysctl kernel.perf_cpu_time_max_percent=P`";
}

}  // namespace tachyon::capture::perf
