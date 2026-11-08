#include "cli/doctor.h"

#include "control/transport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ostream>
#include <sstream>
#include <system_error>

#if defined(__linux__)
#include "capture/linux_perf/perf_system.h"

#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace tachyon::cli {

namespace fs = std::filesystem;

namespace {

constexpr int kCapSysPtrace = 19;
constexpr int kCapSysAdmin = 21;
constexpr int kCapPerfmon = 38;
constexpr int kCapIpcLock = 14;
constexpr int64_t kDefaultRingKib = 257 * 4;  // the agent's default ring: 256 data pages plus the header page
constexpr size_t kSunPathMax = 107;

bool readText(const fs::path& path, std::string& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return false;
  out = ss.str();
  return true;
}

std::string trim(std::string s) {
  const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

bool parseInt(const std::string& text, int64_t& out) {
  const std::string t = trim(text);
  if (t.empty()) return false;
  int64_t v = 0;
  const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
  if (ec != std::errc() || ptr != t.data() + t.size()) return false;
  out = v;
  return true;
}

// A numeric sysctl below procRoot/sys/kernel (or any path); nullopt-like via bool.
bool readNumber(const fs::path& path, int64_t& out) {
  std::string text;
  return readText(path, text) && parseInt(text, out);
}

bool hasBit(uint64_t caps, int bit) {
  return ((caps >> bit) & 1U) != 0;
}

// Number of cpus in a sysfs list such as "0-3,5". 0 when unreadable or malformed.
uint32_t countCpus(const std::string& listText) {
  uint32_t count = 0;
  size_t i = 0;
  const std::string text = trim(listText);
  while (i < text.size()) {
    uint64_t a = 0, b = 0;
    size_t start = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0)
      a = a * 10 + (text[i++] - '0');
    if (i == start) return 0;
    b = a;
    if (i < text.size() && text[i] == '-') {
      ++i;
      start = i;
      b = 0;
      while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0)
        b = b * 10 + (text[i++] - '0');
      if (i == start || b < a) return 0;
    }
    if (b > 1'000'000) return 0;
    count += static_cast<uint32_t>(b - a + 1);
    if (i < text.size()) {
      if (text[i] != ',') return 0;
      ++i;
    }
  }
  return count;
}

// "Max open files   1024   524288   files" in /proc/<pid>/limits: the soft limit, -1 for "unlimited", -2 when
// the row is missing or malformed.
int64_t readLimit(const fs::path& limitsFile, const std::string& row) {
  std::ifstream in(limitsFile);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(row, 0) != 0) continue;
    std::istringstream rest(line.substr(row.size()));
    std::string soft;
    rest >> soft;
    if (soft == "unlimited") return -1;
    int64_t v = 0;
    return parseInt(soft, v) ? v : -2;
  }
  return -2;
}

struct Status {
  bool known = false;
  uint64_t capEff = 0;
  int64_t euid = -1;
};

Status readStatus(const fs::path& procRoot) {
  Status st;
  std::ifstream in(procRoot / "self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("CapEff:", 0) == 0) {
      char* end = nullptr;
      const std::string v = trim(line.substr(7));
      st.capEff = std::strtoull(v.c_str(), &end, 16);
      st.known = end != v.c_str();
    } else if (line.rfind("Uid:", 0) == 0) {
      std::istringstream f(line.substr(4));
      std::string real, eff;
      f >> real >> eff;
      parseInt(eff, st.euid);
    }
  }
  return st;
}

struct Version {
  bool known = false;
  int major = 0, minor = 0;
  std::string text;
};

Version readKernelVersion(const fs::path& procRoot) {
  Version v;
  std::string text;
  if (!readText(procRoot / "sys/kernel/osrelease", text)) return v;
  v.text = trim(text);
  const char* p = v.text.c_str();
  char* end = nullptr;
  const long major = std::strtol(p, &end, 10);
  if (end == p || *end != '.') return v;
  p = end + 1;
  const long minor = std::strtol(p, &end, 10);
  if (end == p) return v;
  v.major = static_cast<int>(major);
  v.minor = static_cast<int>(minor);
  v.known = true;
  return v;
}

struct Context {
  Context(const DoctorEnv& e, std::vector<DoctorCheck>& o) : env(e), out(o) {}
  const DoctorEnv& env;
  std::vector<DoctorCheck>& out;
  Status self;
  FileCapabilities agentCaps;  // valid only when the agent file carries capabilities
  bool agentCapsKnown = false;
  bool processPerf =
      false;               // this process can use perf_event_open without the sysctl (CAP_PERFMON/SYS_ADMIN)
  bool agentPerf = false;  // the agent binary gets CAP_PERFMON/SYS_ADMIN effective (file caps or setuid root)
  bool processPtrace = false;
  bool agentPtrace = false;
  int64_t paranoid = 0;
  bool paranoidKnown = false;
  uint32_t cpus = 0;

  void add(const char* id, const char* title, DoctorStatus status, std::string detail,
           std::string hint = {}) {
    DoctorCheck c;
    c.id = id;
    c.title = title;
    c.status = status;
    c.detail = std::move(detail);
    c.hint = status == DoctorStatus::Pass ? std::string() : std::move(hint);
    out.push_back(std::move(c));
  }
  bool privileged() const { return processPerf || agentPerf; }
};

std::string capList(uint64_t caps) {
  std::string s;
  const struct {
    int bit;
    const char* name;
  } names[] = {
      {kCapPerfmon, "CAP_PERFMON"}, {kCapSysAdmin, "CAP_SYS_ADMIN"}, {kCapSysPtrace, "CAP_SYS_PTRACE"}};
  for (const auto& n : names) {
    if (!s.empty()) s += ", ";
    s += n.name;
    s += hasBit(caps, n.bit) ? "=yes" : "=no";
  }
  return s;
}

void checkKernel(Context& c) {
  const Version v = readKernelVersion(c.env.procRoot);
  if (!v.known) {
    c.add("kernel_version", "Kernel version", DoctorStatus::Warn,
          "unknown: cannot read /proc/sys/kernel/osrelease",
          "Tachyon needs Linux 5.8 or newer (CAP_PERFMON); 4.3 or newer works with CAP_SYS_ADMIN");
    return;
  }
  const std::string detail = "Linux " + v.text;
  if (v.major < 4 || (v.major == 4 && v.minor < 3)) {
    c.add("kernel_version", "Kernel version", DoctorStatus::Fail,
          detail + ": too old (perf clock selection and context-switch records need Linux 4.3)",
          "upgrade to Linux 5.8 or newer");
  } else if (v.major < 5 || (v.major == 5 && v.minor < 8)) {
    c.add(
        "kernel_version", "Kernel version", DoctorStatus::Warn,
        detail +
            ": works, but CAP_PERFMON does not exist before 5.8 (the agent needs CAP_SYS_ADMIN or root) "
            "and build ids in mmap records need 5.12",
        "use `setcap cap_sys_admin,cap_sys_ptrace+ep tachyon-agent` or run the agent as root, or upgrade the "
        "kernel");
  } else {
    c.add("kernel_version", "Kernel version", DoctorStatus::Pass, detail);
  }
}

const char* paranoidMeaning(int64_t v) {
  if (v <= -1) return "every event for every user, no mlock limit";
  if (v == 0)
    return "system-wide cpu events and kernel profiling; raw tracepoints (scheduler events) need CAP_PERFMON";
  if (v == 1) return "own processes including kernel samples; no system-wide sampling without CAP_PERFMON";
  if (v == 2) return "own processes, user-space samples only; no system-wide sampling without CAP_PERFMON";
  return "nothing without CAP_PERFMON (distribution patch: perf_event_open is root-only)";
}

void checkParanoid(Context& c) {
  const char* title = "kernel.perf_event_paranoid";
  int64_t v = 0;
  if (!readNumber(c.env.procRoot / "sys/kernel/perf_event_paranoid", v)) {
    c.add(
        "perf_event_paranoid", title, DoctorStatus::Warn,
        "unknown: cannot read /proc/sys/kernel/perf_event_paranoid",
        "check with `sysctl kernel.perf_event_paranoid`; -1 allows everything, 2 (typical default) only own "
        "processes without kernel samples");
    return;
  }
  c.paranoid = v;
  c.paranoidKnown = true;
  const std::string detail = "value " + std::to_string(v) + " allows " + paranoidMeaning(v);
  if (c.privileged()) {
    c.add("perf_event_paranoid", title, DoctorStatus::Pass,
          detail + "; not limiting here because " + (c.processPerf ? "this process" : "the agent binary") +
              " holds CAP_PERFMON or CAP_SYS_ADMIN");
    return;
  }
  const std::string fix =
      "run `sudo sysctl kernel.perf_event_paranoid=-1` (persist in /etc/sysctl.d/), or give the agent "
      "CAP_PERFMON: `sudo setcap cap_perfmon,cap_sys_ptrace+ep tachyon-agent`";
  if (v <= -1) {
    c.add("perf_event_paranoid", title, DoctorStatus::Pass, detail);
  } else if (v == 0) {
    c.add("perf_event_paranoid", title, DoctorStatus::Warn,
          detail + ": scheduler tracepoints fall back to context-switch records (no wait reasons or stacks)",
          fix);
  } else if (v <= 2) {
    c.add("perf_event_paranoid", title, DoctorStatus::Warn,
          detail + ": system captures are impossible and per-process captures may lack kernel frames", fix);
  } else {
    c.add("perf_event_paranoid", title, DoctorStatus::Fail, detail, fix);
  }
}

void checkSampleRate(Context& c) {
  const char* title = "kernel.perf_event_max_sample_rate";
  int64_t v = 0;
  if (!readNumber(c.env.procRoot / "sys/kernel/perf_event_max_sample_rate", v)) {
    c.add("max_sample_rate", title, DoctorStatus::Warn,
          "unknown: cannot read /proc/sys/kernel/perf_event_max_sample_rate",
          "check with `sysctl kernel.perf_event_max_sample_rate`; it must be at least the requested --hz");
    return;
  }
  const std::string hz = std::to_string(c.env.requestedHz);
  if (v >= static_cast<int64_t>(c.env.requestedHz)) {
    c.add("max_sample_rate", title, DoctorStatus::Pass,
          "limit " + std::to_string(v) + " Hz, requested " + hz + " Hz");
  } else {
    c.add("max_sample_rate", title, DoctorStatus::Fail,
          "limit " + std::to_string(v) + " Hz is below the requested " + hz +
              " Hz: the kernel refuses the event (or throttles it)",
          "lower --hz to " + std::to_string(v) + " or run `sudo sysctl kernel.perf_event_max_sample_rate=" +
              std::to_string(std::max<int64_t>(c.env.requestedHz * 2, 1)) + "`");
  }
}

void checkCpuTimeMax(Context& c) {
  const char* title = "kernel.perf_cpu_time_max_percent";
  int64_t v = 0;
  if (!readNumber(c.env.procRoot / "sys/kernel/perf_cpu_time_max_percent", v)) {
    c.add("cpu_time_max_percent", title, DoctorStatus::Warn,
          "unknown: cannot read /proc/sys/kernel/perf_cpu_time_max_percent",
          "check with `sysctl kernel.perf_cpu_time_max_percent`");
    return;
  }
  if (v == 0 || v >= 100) {
    c.add("cpu_time_max_percent", title, DoctorStatus::Pass,
          "value " + std::to_string(v) + ": throttling by cpu time is off");
  } else if (v < 10) {
    c.add("cpu_time_max_percent", title, DoctorStatus::Warn,
          "value " + std::to_string(v) +
              " %: the kernel lowers the sample rate on its own once sampling costs more "
              "than this share of cpu time, which shows as throttle warnings",
          "run `sudo sysctl kernel.perf_cpu_time_max_percent=25` (default) or 0 to disable the limit");
  } else {
    c.add("cpu_time_max_percent", title, DoctorStatus::Pass, "value " + std::to_string(v) + " %");
  }
}

void checkMlock(Context& c) {
  const char* title = "kernel.perf_event_mlock_kb";
  int64_t v = 0;
  if (!readNumber(c.env.procRoot / "sys/kernel/perf_event_mlock_kb", v)) {
    c.add("perf_event_mlock_kb", title, DoctorStatus::Warn,
          "unknown: cannot read /proc/sys/kernel/perf_event_mlock_kb",
          "check with `sysctl kernel.perf_event_mlock_kb`");
    return;
  }
  const int64_t need = kDefaultRingKib * std::max<uint32_t>(c.cpus, 1);
  const std::string detail = "budget " + std::to_string(v) + " KiB; a system capture maps about " +
                             std::to_string(need) + " KiB of ring buffers (" +
                             std::to_string(std::max<uint32_t>(c.cpus, 1)) + " cpus x 1 MiB)";
  if (c.privileged() || (c.paranoidKnown && c.paranoid < 0)) {
    c.add("perf_event_mlock_kb", title, DoctorStatus::Pass,
          detail + "; not limiting with CAP_PERFMON or paranoid -1");
  } else if (v >= need) {
    c.add("perf_event_mlock_kb", title, DoctorStatus::Pass, detail);
  } else {
    c.add("perf_event_mlock_kb", title, DoctorStatus::Warn,
          detail + ": smaller rings are needed (agent --perf-ring-pages) or the budget must grow",
          "run `sudo sysctl kernel.perf_event_mlock_kb=" + std::to_string(need) +
              "`, raise RLIMIT_MEMLOCK, or give the agent CAP_PERFMON");
  }
}

void checkProcessCaps(Context& c) {
  const char* title = "Capabilities of this process";
  if (!c.self.known) {
    c.add("process_capabilities", title, DoctorStatus::Warn, "unknown: cannot read /proc/self/status",
          "check with `capsh --print`");
    return;
  }
  const std::string detail = capList(c.self.capEff);
  if (c.processPerf) {
    c.add("process_capabilities", title, DoctorStatus::Pass, detail);
  } else if (c.paranoidKnown && c.paranoid < 0) {
    c.add("process_capabilities", title, DoctorStatus::Pass,
          detail + "; not needed with perf_event_paranoid -1");
  } else {
    c.add(
        "process_capabilities", title, DoctorStatus::Warn,
        detail + ": no CAP_PERFMON or CAP_SYS_ADMIN, so a capture started from this user's shell is limited",
        "run the agent as root, or `sudo setcap cap_perfmon,cap_sys_ptrace+ep tachyon-agent` (kernels before "
        "5.8: "
        "cap_sys_admin instead of cap_perfmon)");
  }
}

void checkAgentCaps(Context& c) {
  const char* title = "Capabilities of the agent binary";
  const DoctorAgentFile& a = c.env.agent;
  if (!a.found) {
    c.add("agent_capabilities", title, DoctorStatus::Warn,
          "tachyon-agent was not found next to tachyon-cli or on $PATH",
          "pass its location with `doctor --agent PATH`");
    return;
  }
  const std::string where = a.path.string();
  if (a.setuidRoot) {
    c.add("agent_capabilities", title, DoctorStatus::Pass,
          where + ": owned by root with the set-user-ID bit");
    return;
  }
  switch (a.xattr) {
    case DoctorAgentFile::Xattr::Present: {
      const FileCapabilities fc = parseFileCapabilities(a.xattrBlob);
      if (!fc.valid) {
        c.add("agent_capabilities", title, DoctorStatus::Warn,
              where + ": cannot decode security.capability (" + fc.error + ")",
              "reapply with `sudo setcap cap_perfmon,cap_sys_ptrace+ep " + where + "`");
        return;
      }
      const std::string detail = where + ": permitted " + capList(fc.permitted) + ", " +
                                 (fc.effective ? "effective flag set" : "effective flag NOT set");
      const bool perf =
          fc.effective && (hasBit(fc.permitted, kCapPerfmon) || hasBit(fc.permitted, kCapSysAdmin));
      if (!perf) {
        c.add("agent_capabilities", title, DoctorStatus::Warn,
              detail + (fc.effective ? ": no CAP_PERFMON or CAP_SYS_ADMIN" : ": use +ep, not +p"),
              "run `sudo setcap cap_perfmon,cap_sys_ptrace+ep " + where + "`");
      } else if (!hasBit(fc.permitted, kCapSysPtrace)) {
        c.add("agent_capabilities", title, DoctorStatus::Warn,
              detail + ": without CAP_SYS_PTRACE the agent cannot attach to processes of other users",
              "run `sudo setcap cap_perfmon,cap_sys_ptrace+ep " + where + "`");
      } else {
        c.add("agent_capabilities", title, DoctorStatus::Pass, detail);
      }
      return;
    }
    case DoctorAgentFile::Xattr::Absent:
      c.add("agent_capabilities", title, DoctorStatus::Warn,
            where + ": no file capabilities; the agent works only when started as root or with sudo",
            "run `sudo setcap cap_perfmon,cap_sys_ptrace+ep " + where + "` to run it as a normal user");
      return;
    case DoctorAgentFile::Xattr::Unsupported:
      c.add("agent_capabilities", title, DoctorStatus::Warn,
            where + ": the filesystem has no extended attributes, so file capabilities cannot be set there",
            "copy the agent to a local ext4/xfs/btrfs path before `setcap`, or run it as root");
      return;
    default:
      c.add("agent_capabilities", title, DoctorStatus::Warn,
            where + ": unknown: cannot read security.capability", "check with `getcap " + where + "`");
      return;
  }
}

void checkTracefs(Context& c) {
  const char* title = "tracefs and scheduler tracepoints";
  const fs::path candidates[] = {c.env.sysRoot / "kernel/tracing", c.env.sysRoot / "kernel/debug/tracing"};
  fs::path root;
  std::error_code ec;
  for (const fs::path& cand : candidates) {
    if (fs::exists(cand / "events/sched/sched_switch/format", ec)) {
      root = cand;
      break;
    }
  }
  const std::string mountHint =
      "mount it with `sudo mount -t tracefs nodev /sys/kernel/tracing`; for non-root access `sudo chgrp -R "
      "tracing "
      "/sys/kernel/tracing && sudo chmod -R g+rX /sys/kernel/tracing` and add the user to that group. "
      "Without it "
      "the capture falls back to context-switch records (no wait reasons, blocking stacks or wakers)";
  if (root.empty()) {
    c.add("tracefs", title, DoctorStatus::Warn,
          "no readable tracefs with events/sched/sched_switch under /sys/kernel/tracing or "
          "/sys/kernel/debug/tracing",
          mountHint);
    return;
  }
  const auto readable = [&](const fs::path& p) {
    std::string t;
    int64_t id = 0;
    return readText(p, t) && parseInt(t, id) && id > 0;
  };
  const bool sw = readable(root / "events/sched/sched_switch/id");
  const bool wake =
      readable(root / "events/sched/sched_waking/id") || readable(root / "events/sched/sched_wakeup/id");
  if (!sw) {
    c.add("tracefs", title, DoctorStatus::Warn, root.string() + ": sched_switch/id is not readable",
          mountHint);
  } else if (!wake) {
    c.add("tracefs", title, DoctorStatus::Warn,
          root.string() +
              ": sched_switch is readable, sched_waking/sched_wakeup is not: wakers will be missing",
          mountHint);
  } else if (!c.privileged() && c.paranoidKnown && c.paranoid >= 0) {
    c.add("tracefs", title, DoctorStatus::Warn,
          root.string() + ": sched_switch and sched_waking ids are readable, but perf_event_paranoid " +
              std::to_string(c.paranoid) + " denies raw tracepoints to unprivileged users",
          "give the agent CAP_PERFMON or set kernel.perf_event_paranoid=-1");
  } else {
    c.add("tracefs", title, DoctorStatus::Pass,
          root.string() + ": sched_switch and sched_waking ids readable");
  }
}

std::string errnoHint(int err, bool systemWide, const Context& c) {
  switch (err) {
    case EACCES:
    case EPERM:
      return systemWide
                 ? "grant CAP_PERFMON (`sudo setcap cap_perfmon,cap_sys_ptrace+ep tachyon-agent`), run as "
                   "root, "
                   "or set kernel.perf_event_paranoid=-1 (system-wide sampling needs it or a capability)"
                 : "set kernel.perf_event_paranoid to 2 or lower, or grant CAP_PERFMON; in a container "
                   "also check seccomp (`--cap-add=PERFMON --security-opt seccomp=unconfined`)";
    case ENOSYS:
    case ENOENT:
    case ENODEV:
    case EOPNOTSUPP:
      return "the kernel or hypervisor has no software cpu-clock event, or seccomp filters perf_event_open "
             "(containers: `--security-opt seccomp=unconfined`)";
    case EINVAL:
      return "the kernel rejected the event: check kernel.perf_event_max_sample_rate (" +
             std::to_string(c.env.requestedHz) + " Hz requested) and the kernel version";
    case EMFILE:
    case ENFILE: return "raise the open-file limit (`ulimit -n`)";
    default: return "see `man perf_event_open` for errno " + std::to_string(err);
  }
}

void checkTrial(Context& c) {
  const bool haveSys = static_cast<bool>(c.env.trialSystemWide);
  const bool havePid = static_cast<bool>(c.env.trialPerProcess);
  const int sysErr = haveSys ? c.env.trialSystemWide() : 0;
  const int pidErr = havePid ? c.env.trialPerProcess() : 0;
  const auto describe = [](int e) {
    return std::generic_category().message(e) + " (errno " + std::to_string(e) + ")";
  };

  // System-wide.
  const char* titleSys = "perf_event_open, system-wide";
  if (!haveSys) {
    c.add("perf_open_system", titleSys, DoctorStatus::Warn,
          "unknown: the trial cannot be made on this platform", "");
  } else if (sysErr == 0) {
    c.add("perf_open_system", titleSys, DoctorStatus::Pass, "cpu-clock with callchains opens on cpu 0");
  } else if (c.agentPerf && !c.processPerf) {
    c.add("perf_open_system", titleSys, DoctorStatus::Warn,
          "fails for this process: " + describe(sysErr) +
              "; the agent binary has capabilities, so the agent itself "
              "may still succeed",
          "run the doctor as root (`sudo tachyon-cli doctor`) to test with the agent's privileges");
  } else if (pidErr != 0 && havePid) {
    c.add("perf_open_system", titleSys, DoctorStatus::Fail, "fails: " + describe(sysErr),
          errnoHint(sysErr, true, c));
  } else {
    c.add("perf_open_system", titleSys, DoctorStatus::Warn,
          "fails: " + describe(sysErr) +
              "; system captures need privileges, launch and attach captures of your own "
              "processes may still work",
          errnoHint(sysErr, true, c));
  }

  // Per process.
  const char* titlePid = "perf_event_open, per process";
  if (!havePid) {
    c.add("perf_open_process", titlePid, DoctorStatus::Warn,
          "unknown: the trial cannot be made on this platform", "");
  } else if (pidErr == 0) {
    c.add("perf_open_process", titlePid, DoctorStatus::Pass,
          "cpu-clock with callchains opens for this process");
  } else if (c.agentPerf && !c.processPerf) {
    c.add("perf_open_process", titlePid, DoctorStatus::Warn,
          "fails for this process: " + describe(pidErr) + "; the agent binary has capabilities", "");
  } else if (sysErr != 0 && haveSys) {
    c.add("perf_open_process", titlePid, DoctorStatus::Fail, "fails: " + describe(pidErr),
          errnoHint(pidErr, false, c));
  } else {
    c.add("perf_open_process", titlePid, DoctorStatus::Warn, "fails: " + describe(pidErr),
          errnoHint(pidErr, false, c));
  }
}

void checkNofile(Context& c) {
  const char* title = "RLIMIT_NOFILE";
  const int64_t limit = readLimit(c.env.procRoot / "self/limits", "Max open files");
  if (limit == -2) {
    c.add("rlimit_nofile", title, DoctorStatus::Warn, "unknown: cannot read /proc/self/limits",
          "check with `ulimit -n`");
    return;
  }
  if (limit == -1) {
    c.add("rlimit_nofile", title, DoctorStatus::Pass, "unlimited");
    return;
  }
  const int64_t cpus = std::max<uint32_t>(c.cpus, 1);
  const int64_t comfortable =
      cpus * 3 + 256;  // cpu-clock plus two tracepoint events per cpu, plus the agent's own
  const std::string detail = "soft limit " + std::to_string(limit) + ", " + std::to_string(cpus) +
                             " cpus (a system capture holds up to 3 descriptors per cpu)";
  if (limit >= comfortable) {
    c.add("rlimit_nofile", title, DoctorStatus::Pass, detail);
  } else if (limit >= cpus + 32) {
    c.add("rlimit_nofile", title, DoctorStatus::Warn,
          detail + "; attach and launch captures without CAP_PERFMON use threads x cpus descriptors",
          "run `ulimit -n " + std::to_string(comfortable) + "` (or more) before starting the agent");
  } else {
    c.add("rlimit_nofile", title, DoctorStatus::Fail, detail + ": not enough for one ring per cpu",
          "run `ulimit -n " + std::to_string(comfortable) + "` (or more) before starting the agent");
  }
}

void checkMemlock(Context& c) {
  const char* title = "RLIMIT_MEMLOCK";
  const int64_t limit = readLimit(c.env.procRoot / "self/limits", "Max locked memory");
  if (limit == -2) {
    c.add("rlimit_memlock", title, DoctorStatus::Warn, "unknown: cannot read /proc/self/limits",
          "check with `ulimit -l`");
    return;
  }
  if (limit == -1) {
    c.add("rlimit_memlock", title, DoctorStatus::Pass, "unlimited");
    return;
  }
  const int64_t needKib = kDefaultRingKib * std::max<uint32_t>(c.cpus, 1);
  const std::string detail = "soft limit " + std::to_string(limit / 1024) + " KiB, ring buffers need about " +
                             std::to_string(needKib) + " KiB";
  const bool ipcLock = hasBit(c.self.capEff, kCapIpcLock) || c.privileged();
  if (ipcLock || limit / 1024 >= needKib) {
    c.add("rlimit_memlock", title, DoctorStatus::Pass,
          detail + (ipcLock ? "; exempt with CAP_PERFMON/CAP_IPC_LOCK" : ""));
  } else {
    c.add("rlimit_memlock", title, DoctorStatus::Warn,
          detail +
              ": together with kernel.perf_event_mlock_kb it bounds the ring memory of an unprivileged user",
          "run `ulimit -l unlimited` (or raise `memlock` in /etc/security/limits.conf), or give the agent "
          "CAP_PERFMON");
  }
}

void checkPtraceScope(Context& c) {
  const char* title = "kernel.yama.ptrace_scope";
  const fs::path file = c.env.procRoot / "sys/kernel/yama/ptrace_scope";
  std::error_code ec;
  if (!fs::exists(c.env.procRoot / "sys/kernel/yama", ec)) {
    c.add("ptrace_scope", title, DoctorStatus::Pass,
          "the Yama security module is not active: no ptrace restriction");
    return;
  }
  int64_t v = 0;
  if (!readNumber(file, v)) {
    c.add("ptrace_scope", title, DoctorStatus::Warn, "unknown: cannot read " + file.string(),
          "check with `sysctl kernel.yama.ptrace_scope`");
    return;
  }
  const bool ptrace = c.processPtrace || c.agentPtrace;
  const std::string detail = "value " + std::to_string(v) + ", launch captures are not affected";
  if (v <= 1 || ptrace) {
    c.add("ptrace_scope", title, DoctorStatus::Pass,
          detail + (ptrace ? "; CAP_SYS_PTRACE is present: attach works for any process the agent may signal"
                    : v == 1 ? "; attach works only for descendants of the agent (processes it launched) "
                               "unless the agent has CAP_SYS_PTRACE"
                             : ""));
  } else {
    c.add("ptrace_scope", title, DoctorStatus::Warn,
          detail + (v >= 3 ? "; attach to running processes is blocked" : "; attach needs CAP_SYS_PTRACE"),
          v >= 3 ? "ptrace_scope 3 cannot be lowered without a reboot: use launch captures or reboot with a "
                   "lower value"
                 : "grant the agent CAP_SYS_PTRACE (`sudo setcap cap_perfmon,cap_sys_ptrace+ep "
                   "tachyon-agent`) or run "
                   "`sudo sysctl kernel.yama.ptrace_scope=1`");
  }
}

void checkSocketDir(Context& c) {
  const char* title = "Agent socket directory";
  if (c.env.socketAddress.empty()) {
    c.add("socket_dir", title, DoctorStatus::Warn, "unknown: no socket address", "");
    return;
  }
  // A relative --socket names a file in the current directory (the agent resolves it the same way).
  std::error_code absEc;
  fs::path socketPath = fs::absolute(fs::path(c.env.socketAddress), absEc);
  if (absEc) socketPath = c.env.socketAddress;
  const fs::path dir = socketPath.parent_path();
  const std::string fix =
      "set $TACHYON_AGENT_PIPE (or --socket) to a socket path in a directory you can write, e.g. "
      "/run/user/<uid>/tachyon-agent.sock";
  if (c.env.socketAddress.size() > kSunPathMax) {
    c.add("socket_dir", title, DoctorStatus::Fail,
          c.env.socketAddress + ": longer than " + std::to_string(kSunPathMax) +
              " characters, the limit of a Unix socket path",
          fix);
    return;
  }
  const bool writable = c.env.isWritableDir ? c.env.isWritableDir(dir) : false;
  if (!writable) {
    c.add("socket_dir", title, DoctorStatus::Fail,
          dir.string() + " does not exist or is not writable: the agent cannot create " + socketPath.string(),
          fix);
    return;
  }
  // The socket itself is 0600, but a directory others can write to lets them replace or pre-create it, and
  // one they can list shows when the agent runs. It should be private to this user.
  uint32_t mode = 0, owner = 0;
  if (c.env.statDir && c.env.statDir(dir, mode, owner)) {
    const bool wrongOwner = owner != c.env.uid;
    if (wrongOwner || (mode & 077) != 0) {
      char text[16];
      std::snprintf(text, sizeof text, "%04o", static_cast<unsigned>(mode & 07777));
      std::string what = dir.string() + " has mode " + text + " and is owned by uid " + std::to_string(owner);
      what +=
          wrongOwner ? ", not by you (uid " + std::to_string(c.env.uid) + ")" : ": other users can access it";
      c.add("socket_dir", title, DoctorStatus::Warn,
            what + "; the socket is " + socketPath.string() + " (directory writable)",
            "use a private directory: `mkdir -m 700 -p $XDG_RUNTIME_DIR/tachyon` and pass --socket "
            "$XDG_RUNTIME_DIR/tachyon/agent.sock, or `chmod 700 " +
                dir.string() + "` if you own it");
      return;
    }
  }
  c.add("socket_dir", title, DoctorStatus::Pass, socketPath.string() + " (directory writable)");
}

void checkSymbols(Context& c) {
  const char* title = "Symbol sources (debuginfod, .build-id)";
  std::error_code ec;
  std::vector<std::string> found;
  for (const fs::path& d : c.env.buildIdDirs)
    if (fs::is_directory(d, ec)) found.push_back(d.string());
  std::string detail;
  if (!c.env.debuginfodUrls.empty()) detail = "DEBUGINFOD_URLS=" + c.env.debuginfodUrls;
  for (const std::string& f : found) detail += (detail.empty() ? "" : "; ") + f;
  if (!c.env.debuginfodUrls.empty() || !found.empty()) {
    c.add("symbols", title, DoctorStatus::Pass, detail);
  } else {
    c.add("symbols", title, DoctorStatus::Warn,
          "no debuginfod URL and no .build-id directory: system libraries and the kernel will show without "
          "names",
          "install the -dbgsym/-debuginfo packages (they populate /usr/lib/debug/.build-id) or set "
          "$DEBUGINFOD_URLS (e.g. https://debuginfod.elfutils.org/); your own binaries need debug info or a "
          "symbol path");
  }
}

}  // namespace

const char* doctorStatusName(DoctorStatus status) {
  switch (status) {
    case DoctorStatus::Pass: return "PASS";
    case DoctorStatus::Warn: return "WARN";
    case DoctorStatus::Fail: return "FAIL";
  }
  return "FAIL";
}

FileCapabilities parseFileCapabilities(const std::string& blob) {
  FileCapabilities fc;
  const auto u32 = [&](size_t off) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(blob[off + static_cast<size_t>(i)]);
    return v;
  };
  if (blob.size() < 4) {
    fc.error = "value is " + std::to_string(blob.size()) + " bytes";
    return fc;
  }
  const uint32_t magic = u32(0);
  const uint32_t revision = magic & 0xFF000000u;
  fc.effective = (magic & 1u) != 0;
  if (revision == 0x01000000u && blob.size() == 12) {
    fc.revision = 1;
    fc.permitted = u32(4);
    fc.inheritable = u32(8);
  } else if ((revision == 0x02000000u && blob.size() == 20) ||
             (revision == 0x03000000u && blob.size() == 24)) {
    fc.revision = revision == 0x02000000u ? 2 : 3;
    fc.permitted = u32(4) | (static_cast<uint64_t>(u32(12)) << 32);
    fc.inheritable = u32(8) | (static_cast<uint64_t>(u32(16)) << 32);
  } else {
    fc.error = "unknown layout (revision " + std::to_string(revision >> 24) + ", " +
               std::to_string(blob.size()) + " bytes)";
    return fc;
  }
  fc.valid = true;
  return fc;
}

DoctorReport runDoctorChecks(const DoctorEnv& env) {
  DoctorReport report;
  Context c(env, report.checks);
  c.self = readStatus(env.procRoot);
  c.processPerf = c.self.known && (hasBit(c.self.capEff, kCapPerfmon) || hasBit(c.self.capEff, kCapSysAdmin));
  c.processPtrace = c.self.known && hasBit(c.self.capEff, kCapSysPtrace);
  if (env.agent.found) {
    if (env.agent.setuidRoot) {
      c.agentPerf = c.agentPtrace = true;
    } else if (env.agent.xattr == DoctorAgentFile::Xattr::Present) {
      const FileCapabilities fc = parseFileCapabilities(env.agent.xattrBlob);
      if (fc.valid && fc.effective) {
        c.agentPerf = hasBit(fc.permitted, kCapPerfmon) || hasBit(fc.permitted, kCapSysAdmin);
        c.agentPtrace = hasBit(fc.permitted, kCapSysPtrace);
      }
    }
  }
  std::string cpuList;
  if (readText(env.sysRoot / "devices/system/cpu/online", cpuList)) c.cpus = countCpus(cpuList);
  if (c.cpus == 0) c.cpus = 1;  // unreadable: the checks scale by one cpu
  {
    int64_t p = 0;
    if (readNumber(env.procRoot / "sys/kernel/perf_event_paranoid", p)) {
      c.paranoid = p;
      c.paranoidKnown = true;
    }
  }

  checkKernel(c);
  checkProcessCaps(c);
  checkAgentCaps(c);
  checkParanoid(c);
  checkSampleRate(c);
  checkCpuTimeMax(c);
  checkMlock(c);
  checkTracefs(c);
  checkTrial(c);
  checkNofile(c);
  checkMemlock(c);
  checkPtraceScope(c);
  checkSocketDir(c);
  checkSymbols(c);

  for (const DoctorCheck& chk : report.checks) {
    switch (chk.status) {
      case DoctorStatus::Pass: ++report.pass; break;
      case DoctorStatus::Warn: ++report.warn; break;
      case DoctorStatus::Fail: ++report.fail; break;
    }
  }
  return report;
}

std::string formatDoctorText(const DoctorReport& report, const DoctorEnv& env) {
  return formatDoctorText(report, env.requestedHz);
}

std::string formatDoctorText(const DoctorReport& report, uint32_t requestedHz) {
  std::ostringstream os;
  os << "tachyon-cli doctor: " << (report.platform == "windows" ? "Windows" : "Linux")
     << " capture prerequisites (sampling " << requestedHz << " Hz)\n\n";
  for (const DoctorCheck& c : report.checks) {
    os << "[" << doctorStatusName(c.status) << "] " << c.title << "\n       " << c.detail << "\n";
    if (!c.hint.empty()) os << "       fix: " << c.hint << "\n";
  }
  os << "\n" << report.pass << " passed, " << report.warn << " warnings, " << report.fail << " failed";
  os << (report.fail > 0   ? ": captures will not work until the FAIL items are fixed\n"
         : report.warn > 0 ? ": captures work, with the limits described above\n"
                           : ": this machine is ready\n");
  return os.str();
}

std::string formatDoctorJson(const DoctorReport& report, const DoctorEnv& env) {
  return formatDoctorJson(report, env.requestedHz);
}

std::string formatDoctorJson(const DoctorReport& report, uint32_t requestedHz) {
  nlohmann::ordered_json j;
  j["schema"] = 1;
  j["platform"] = report.platform;
  j["ok"] = report.fail == 0;
  j["exit_code"] = report.exitCode();
  j["requested_hz"] = requestedHz;
  j["summary"] = {{"pass", report.pass}, {"warn", report.warn}, {"fail", report.fail}};
  nlohmann::ordered_json checks = nlohmann::ordered_json::array();
  for (const DoctorCheck& c : report.checks) {
    nlohmann::ordered_json o;
    o["id"] = c.id;
    o["title"] = c.title;
    o["status"] = doctorStatusName(c.status);
    o["detail"] = c.detail;
    o["hint"] = c.hint;
    checks.push_back(std::move(o));
  }
  j["checks"] = std::move(checks);
  return j.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

int runDoctorWithEnv(const DoctorEnv& env, bool json, std::ostream& out) {
  const DoctorReport report = runDoctorChecks(env);
  if (json) {
    out << formatDoctorJson(report, env) << "\n";
  } else {
    out << formatDoctorText(report, env);
  }
  return report.exitCode();
}

int runDoctorWithEnv(const WindowsDoctorEnv& env, bool json, std::ostream& out) {
  const DoctorReport report = runWindowsDoctorChecks(env);
  if (json)
    out << formatDoctorJson(report, env.requestedHz) << "\n";
  else
    out << formatDoctorText(report, env.requestedHz);
  return report.exitCode();
}

namespace {

struct DoctorArgs {
  uint32_t hz = 8000;
  std::string agent, address;
};

constexpr int kDoctorUsage = 3;

// Returns 0, or kDoctorUsage after printing the problem.
int parseDoctorArgs(const std::vector<std::string>& args, std::ostream& err, DoctorArgs& d) {
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const auto value = [&](std::string& dst) {
      if (i + 1 >= args.size()) return false;
      dst = args[++i];
      return true;
    };
    std::string v;
    if (a == "--json") {
      continue;
    } else if (a == "--socket" || a == "--pipe") {
      if (!value(d.address)) {
        err << "tachyon-cli: missing value for " << a << "\n";
        return kDoctorUsage;
      }
    } else if (a == "--timeout" || a == "--connect-timeout-ms") {
      if (!value(v)) {
        err << "tachyon-cli: missing value for " << a << "\n";
        return kDoctorUsage;
      }
    } else if (a == "--hz" || a == "--sampling-hz") {
      int64_t n = 0;
      if (!value(v) || !parseInt(v, n) || n <= 0 || n > 1'000'000) {
        err << "tachyon-cli: --hz expects a positive number\n";
        return kDoctorUsage;
      }
      d.hz = static_cast<uint32_t>(n);
    } else if (a == "--agent") {
      if (!value(d.agent)) {
        err << "tachyon-cli: missing value for --agent\n";
        return kDoctorUsage;
      }
    } else {
      err << "tachyon-cli: unknown argument for doctor: " << a << "\n";
      return kDoctorUsage;
    }
  }
  if (d.address.empty()) d.address = control::defaultAgentAddress();
  return 0;
}

}  // namespace

#if defined(__linux__)

DoctorEnv makeSystemDoctorEnv(uint32_t requestedHz, const std::string& socketAddress,
                              const std::string& agentOverride) {
  DoctorEnv env;
  env.requestedHz = requestedHz;
  env.socketAddress = socketAddress;

  // The agent binary: --agent, else next to this executable, else on $PATH.
  std::error_code ec;
  fs::path agent;
  if (!agentOverride.empty()) {
    agent = agentOverride;
  } else {
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && fs::is_regular_file(self.parent_path() / "tachyon-agent", ec)) {
      agent = self.parent_path() / "tachyon-agent";
    } else if (const char* path = std::getenv("PATH")) {
      std::istringstream dirs(path);
      std::string dir;
      while (std::getline(dirs, dir, ':')) {
        if (!dir.empty() && fs::is_regular_file(fs::path(dir) / "tachyon-agent", ec)) {
          agent = fs::path(dir) / "tachyon-agent";
          break;
        }
      }
    }
  }
  if (!agent.empty() && fs::is_regular_file(agent, ec)) {
    env.agent.found = true;
    env.agent.path = agent;
    struct stat st {};
    if (::stat(agent.c_str(), &st) == 0) env.agent.setuidRoot = st.st_uid == 0 && (st.st_mode & S_ISUID) != 0;
    std::string buf(64, '\0');
    const ssize_t n = ::getxattr(agent.c_str(), "security.capability", buf.data(), buf.size());
    if (n >= 0) {
      buf.resize(static_cast<size_t>(n));
      env.agent.xattr = DoctorAgentFile::Xattr::Present;
      env.agent.xattrBlob = std::move(buf);
    } else if (errno == ENODATA) {
      env.agent.xattr = DoctorAgentFile::Xattr::Absent;
    } else if (errno == ENOTSUP) {
      env.agent.xattr = DoctorAgentFile::Xattr::Unsupported;
    } else {
      env.agent.xattr = DoctorAgentFile::Xattr::Error;
    }
  }

  if (const char* urls = std::getenv("DEBUGINFOD_URLS")) env.debuginfodUrls = trim(urls);
  env.buildIdDirs.push_back("/usr/lib/debug/.build-id");
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    env.buildIdDirs.push_back(fs::path(home) / ".debug/.build-id");

  env.isWritableDir = [](const fs::path& dir) {
    std::error_code e;
    return fs::is_directory(dir, e) && ::access(dir.c_str(), W_OK | X_OK) == 0;
  };
  env.uid = static_cast<uint32_t>(::geteuid());
  env.statDir = [](const fs::path& dir, uint32_t& mode, uint32_t& owner) {
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) return false;
    mode = static_cast<uint32_t>(st.st_mode & 07777);
    owner = static_cast<uint32_t>(st.st_uid);
    return true;
  };

  // Trial events mirror the backend's: cpu-clock, callchain sampling, the backend's sample type.
  const auto system = capture::perf::makeLinuxPerfSystem();
  const uint32_t hz = std::min<uint32_t>(requestedHz, 100);  // the rate is checked separately
  env.trialSystemWide = [system, hz] {
    capture::perf::EventConfig cfg;
    cfg.samplingHz = hz;
    cfg.disabled = true;
    const std::vector<uint32_t> cpus = system->onlineCpus();
    const auto o = system->openEvent(cfg, -1, cpus.empty() ? 0 : static_cast<int>(cpus.front()));
    if (o.fd < 0) return o.err;
    system->closeEvent(o.fd);
    return 0;
  };
  env.trialPerProcess = [system, hz] {
    capture::perf::EventConfig cfg;
    cfg.samplingHz = hz;
    cfg.disabled = true;
    const capture::perf::SysctlInfo sys = system->sysctls();
    cfg.excludeKernel =
        sys.paranoid >= 2 && !sys.hasPerfCapability;  // as the backend does for per-task events
    cfg.inherit = true;
    const auto o = system->openEvent(cfg, 0, -1);  // pid 0: the calling thread
    if (o.fd < 0) return o.err;
    system->closeEvent(o.fd);
    return 0;
  };
  return env;
}

int runDoctor(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err) {
  DoctorArgs d;
  if (const int rc = parseDoctorArgs(args, err, d); rc != 0) return rc;
  return runDoctorWithEnv(makeSystemDoctorEnv(d.hz, d.address, d.agent), json, out);
}

#else

DoctorEnv makeSystemDoctorEnv(uint32_t requestedHz, const std::string& socketAddress, const std::string&) {
  DoctorEnv env;
  env.requestedHz = requestedHz;
  env.socketAddress = socketAddress;
  return env;
}

int runDoctor(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err) {
  DoctorArgs d;
  if (const int rc = parseDoctorArgs(args, err, d); rc != 0) return rc;
#if defined(_WIN32)
  return runDoctorWithEnv(makeSystemWindowsDoctorEnv(d.hz, d.address, d.agent), json, out);
#else
  const char* note = "doctor is only implemented on Linux and Windows";
  if (json) {
    nlohmann::ordered_json j;
    j["schema"] = 1;
    j["platform"] = "unknown";
    j["ok"] = true;
    j["exit_code"] = 0;
    j["note"] = note;
    out << j.dump() << "\n";
  } else {
    out << note << "\n";
  }
  return 0;
#endif
}

#endif

}  // namespace tachyon::cli
