// WP-73: `tachyon-cli doctor` against fake /proc and /sys trees, one test per check state, plus the JSON
// schema, the exit codes and a smoke run on the real machine.
#include "cli/doctor.h"
#include "../control/test_util.h"
#include "cli/cli.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <fstream>
#include <set>
#include <sstream>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;

constexpr uint64_t kPerfmon = 1ull << 38;
constexpr uint64_t kSysAdmin = 1ull << 21;
constexpr uint64_t kSysPtrace = 1ull << 19;

void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

std::string hex(uint64_t v) {
  std::ostringstream os;
  os << std::hex << v;
  std::string s = os.str();
  return std::string(16 - s.size(), '0') + s;
}

// A security.capability blob (revision 2 or 3) with the same bits permitted and inheritable-none.
std::string capBlob(int revision, bool effective, uint64_t permitted) {
  std::string b;
  const auto put = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  };
  put((revision << 24) | (effective ? 1u : 0u));
  put(static_cast<uint32_t>(permitted));
  put(0);
  put(static_cast<uint32_t>(permitted >> 32));
  put(0);
  if (revision == 3) put(0);
  return b;
}

// A machine that passes every check: privileged, paranoid -1, tracefs present, trial opens succeed.
struct Machine {
  TempDir dir;
  fs::path proc() const { return dir.path() / "proc"; }
  fs::path sys() const { return dir.path() / "sys"; }

  void status(uint64_t capEff, int uid = 1000) {
    write(proc() / "self/status", "Name:\tdoctor\nUid:\t" + std::to_string(uid) + "\t" + std::to_string(uid) +
                                      "\t0\t0\nCapEff:\t" + hex(capEff) + "\n");
  }
  void sysctl(const std::string& name, const std::string& value) {
    write(proc() / "sys/kernel" / name, value + "\n");
  }
  void limits(const std::string& nofile, const std::string& memlock) {
    write(proc() / "self/limits",
          "Limit                     Soft Limit           Hard Limit           Units\n"
          "Max open files            " +
              nofile + "                " + nofile +
              "                files\n"
              "Max locked memory         " +
              memlock + "              " + memlock + "              bytes\n");
  }
  void tracefs(bool wake = true) {
    const fs::path t = sys() / "kernel/tracing/events/sched";
    write(t / "sched_switch/format", "name: sched_switch\n");
    write(t / "sched_switch/id", "316\n");
    if (wake) {
      write(t / "sched_waking/format", "name: sched_waking\n");
      write(t / "sched_waking/id", "317\n");
    }
  }

  Machine() {
    status(kPerfmon | kSysPtrace, 0);
    sysctl("osrelease", "6.8.0-fake");
    sysctl("perf_event_paranoid", "-1");
    sysctl("perf_event_max_sample_rate", "100000");
    sysctl("perf_cpu_time_max_percent", "25");
    sysctl("perf_event_mlock_kb", "516");
    limits("1048576", "unlimited");
    write(sys() / "devices/system/cpu/online", "0-7\n");
    tracefs();
    fs::create_directories(dir.path() / "run");
    fs::create_directories(dir.path() / "debug/.build-id");
  }

  DoctorEnv env() const {
    DoctorEnv e;
    e.procRoot = proc();
    e.sysRoot = sys();
    e.requestedHz = 8000;
    e.socketAddress = (dir.path() / "run/tachyon-agent.sock").string();
    e.agent.found = true;
    e.agent.path = dir.path() / "tachyon-agent";
    e.agent.xattr = DoctorAgentFile::Xattr::Present;
    e.agent.xattrBlob = capBlob(3, true, kPerfmon | kSysPtrace);
    e.buildIdDirs = {dir.path() / "debug/.build-id"};
    e.isWritableDir = [](const fs::path& d) { return fs::is_directory(d); };
    e.trialSystemWide = [] { return 0; };
    e.trialPerProcess = [] { return 0; };
    return e;
  }
};

const DoctorCheck& find(const DoctorReport& r, const std::string& id) {
  for (const DoctorCheck& c : r.checks)
    if (c.id == id) return c;
  ADD_FAILURE() << "no check " << id;
  static const DoctorCheck none;
  return none;
}

DoctorStatus statusOf(const DoctorEnv& e, const std::string& id) {
  return find(runDoctorChecks(e), id).status;
}

TEST(Doctor, HealthyMachinePassesEveryCheck) {
  Machine m;
  const DoctorReport r = runDoctorChecks(m.env());
  for (const DoctorCheck& c : r.checks) {
    EXPECT_EQ(c.status, DoctorStatus::Pass) << c.id << ": " << c.detail;
    EXPECT_TRUE(c.hint.empty()) << c.id;
    EXPECT_FALSE(c.detail.empty()) << c.id;
  }
  EXPECT_EQ(r.warn + r.fail, 0);
  EXPECT_EQ(r.exitCode(), 0);
  EXPECT_EQ(r.checks.size(), 15u);
}

TEST(Doctor, KernelVersionStates) {
  Machine m;
  m.sysctl("osrelease", "6.1.0");
  EXPECT_EQ(statusOf(m.env(), "kernel_version"), DoctorStatus::Pass);
  m.sysctl("osrelease", "5.8.0-1");
  EXPECT_EQ(statusOf(m.env(), "kernel_version"), DoctorStatus::Pass);
  m.sysctl("osrelease", "5.4.0-100-generic");
  const DoctorCheck warn = find(runDoctorChecks(m.env()), "kernel_version");
  EXPECT_EQ(warn.status, DoctorStatus::Warn);
  EXPECT_NE(warn.hint.find("cap_sys_admin"), std::string::npos);
  m.sysctl("osrelease", "4.2.1");
  EXPECT_EQ(statusOf(m.env(), "kernel_version"), DoctorStatus::Fail);
  m.sysctl("osrelease", "garbage");
  EXPECT_EQ(statusOf(m.env(), "kernel_version"), DoctorStatus::Warn);
  fs::remove(m.proc() / "sys/kernel/osrelease");
  EXPECT_EQ(statusOf(m.env(), "kernel_version"), DoctorStatus::Warn);
}

TEST(Doctor, ParanoidWithoutCapabilities) {
  Machine m;
  m.status(0);
  DoctorEnv e = m.env();
  e.agent.xattr = DoctorAgentFile::Xattr::Absent;
  const struct {
    const char* value;
    DoctorStatus expected;
  } cases[] = {{"-1", DoctorStatus::Pass}, {"0", DoctorStatus::Warn}, {"1", DoctorStatus::Warn},
               {"2", DoctorStatus::Warn},  {"3", DoctorStatus::Fail}, {"4", DoctorStatus::Fail}};
  for (const auto& c : cases) {
    m.sysctl("perf_event_paranoid", c.value);
    const DoctorCheck chk = find(runDoctorChecks(e), "perf_event_paranoid");
    EXPECT_EQ(chk.status, c.expected) << c.value << ": " << chk.detail;
    if (chk.status != DoctorStatus::Pass) EXPECT_NE(chk.hint.find("setcap"), std::string::npos) << c.value;
    EXPECT_NE(chk.detail.find(std::string("value ") + c.value), std::string::npos);
  }
  fs::remove(m.proc() / "sys/kernel/perf_event_paranoid");
  EXPECT_EQ(statusOf(e, "perf_event_paranoid"), DoctorStatus::Warn);
}

TEST(Doctor, ParanoidDoesNotLimitAPrivilegedProcessOrAgent) {
  Machine m;
  m.sysctl("perf_event_paranoid", "3");
  EXPECT_EQ(statusOf(m.env(), "perf_event_paranoid"), DoctorStatus::Pass);  // process holds CAP_PERFMON
  m.status(0);
  EXPECT_EQ(statusOf(m.env(), "perf_event_paranoid"), DoctorStatus::Pass);  // the agent binary does
  m.status(kSysAdmin);
  DoctorEnv e = m.env();
  e.agent.xattr = DoctorAgentFile::Xattr::Absent;
  EXPECT_EQ(statusOf(e, "perf_event_paranoid"), DoctorStatus::Pass);  // CAP_SYS_ADMIN counts (kernels < 5.8)
}

TEST(Doctor, SampleRateAgainstRequestedHz) {
  Machine m;
  m.sysctl("perf_event_max_sample_rate", "8000");
  EXPECT_EQ(statusOf(m.env(), "max_sample_rate"), DoctorStatus::Pass);
  m.sysctl("perf_event_max_sample_rate", "4000");
  const DoctorCheck fail = find(runDoctorChecks(m.env()), "max_sample_rate");
  EXPECT_EQ(fail.status, DoctorStatus::Fail);
  EXPECT_NE(fail.hint.find("--hz to 4000"), std::string::npos);
  DoctorEnv e = m.env();
  e.requestedHz = 1000;
  EXPECT_EQ(statusOf(e, "max_sample_rate"), DoctorStatus::Pass);
  fs::remove(m.proc() / "sys/kernel/perf_event_max_sample_rate");
  EXPECT_EQ(statusOf(m.env(), "max_sample_rate"), DoctorStatus::Warn);
}

TEST(Doctor, CpuTimeMaxPercentStates) {
  Machine m;
  for (const char* pass : {"0", "25", "100", "10"}) {
    m.sysctl("perf_cpu_time_max_percent", pass);
    EXPECT_EQ(statusOf(m.env(), "cpu_time_max_percent"), DoctorStatus::Pass) << pass;
  }
  m.sysctl("perf_cpu_time_max_percent", "5");
  EXPECT_EQ(statusOf(m.env(), "cpu_time_max_percent"), DoctorStatus::Warn);
  fs::remove(m.proc() / "sys/kernel/perf_cpu_time_max_percent");
  EXPECT_EQ(statusOf(m.env(), "cpu_time_max_percent"), DoctorStatus::Warn);
}

TEST(Doctor, MlockBudgetMattersOnlyWithoutPrivileges) {
  Machine m;  // privileged, paranoid -1
  EXPECT_EQ(statusOf(m.env(), "perf_event_mlock_kb"), DoctorStatus::Pass);
  m.status(0);
  m.sysctl("perf_event_paranoid", "2");
  DoctorEnv e = m.env();
  e.agent.xattr = DoctorAgentFile::Xattr::Absent;
  const DoctorCheck warn = find(runDoctorChecks(e), "perf_event_mlock_kb");
  EXPECT_EQ(warn.status, DoctorStatus::Warn);  // 8 cpus x 1028 KiB > 516 KiB
  EXPECT_NE(warn.hint.find("perf_event_mlock_kb=8224"), std::string::npos) << warn.hint;
  m.sysctl("perf_event_mlock_kb", "9000");
  EXPECT_EQ(statusOf(e, "perf_event_mlock_kb"), DoctorStatus::Pass);
  fs::remove(m.proc() / "sys/kernel/perf_event_mlock_kb");
  EXPECT_EQ(statusOf(e, "perf_event_mlock_kb"), DoctorStatus::Warn);
}

TEST(Doctor, ProcessCapabilities) {
  Machine m;
  EXPECT_EQ(statusOf(m.env(), "process_capabilities"), DoctorStatus::Pass);
  const DoctorCheck pass = find(runDoctorChecks(m.env()), "process_capabilities");
  EXPECT_NE(pass.detail.find("CAP_PERFMON=yes"), std::string::npos);
  EXPECT_NE(pass.detail.find("CAP_SYS_PTRACE=yes"), std::string::npos);
  EXPECT_NE(pass.detail.find("CAP_SYS_ADMIN=no"), std::string::npos);
  m.status(kSysAdmin);
  EXPECT_EQ(statusOf(m.env(), "process_capabilities"), DoctorStatus::Pass);
  m.status(0);
  EXPECT_EQ(statusOf(m.env(), "process_capabilities"), DoctorStatus::Pass);  // paranoid -1: not needed
  m.sysctl("perf_event_paranoid", "2");
  EXPECT_EQ(statusOf(m.env(), "process_capabilities"), DoctorStatus::Warn);
  fs::remove(m.proc() / "self/status");
  EXPECT_EQ(statusOf(m.env(), "process_capabilities"), DoctorStatus::Warn);
}

TEST(Doctor, AgentBinaryCapabilities) {
  Machine m;
  using X = DoctorAgentFile::Xattr;
  DoctorEnv e = m.env();
  EXPECT_EQ(statusOf(e, "agent_capabilities"), DoctorStatus::Pass);
  e.agent.xattrBlob = capBlob(2, true, kSysAdmin | kSysPtrace);  // older kernels
  EXPECT_EQ(statusOf(e, "agent_capabilities"), DoctorStatus::Pass);
  e.agent.xattrBlob = capBlob(3, true, kPerfmon);  // no ptrace
  const DoctorCheck noPtrace = find(runDoctorChecks(e), "agent_capabilities");
  EXPECT_EQ(noPtrace.status, DoctorStatus::Warn);
  EXPECT_NE(noPtrace.detail.find("CAP_SYS_PTRACE"), std::string::npos);
  e.agent.xattrBlob = capBlob(3, false, kPerfmon | kSysPtrace);  // +p instead of +ep
  const DoctorCheck noEffective = find(runDoctorChecks(e), "agent_capabilities");
  EXPECT_EQ(noEffective.status, DoctorStatus::Warn);
  EXPECT_NE(noEffective.detail.find("NOT set"), std::string::npos);
  e.agent.xattrBlob = capBlob(3, true, kSysPtrace);  // wrong capability
  EXPECT_EQ(statusOf(e, "agent_capabilities"), DoctorStatus::Warn);
  e.agent.xattrBlob = "garbage";
  EXPECT_EQ(statusOf(e, "agent_capabilities"), DoctorStatus::Warn);
  e.agent.xattr = X::Absent;
  const DoctorCheck absent = find(runDoctorChecks(e), "agent_capabilities");
  EXPECT_EQ(absent.status, DoctorStatus::Warn);
  EXPECT_NE(absent.hint.find("setcap cap_perfmon,cap_sys_ptrace+ep"), std::string::npos);
  e.agent.xattr = X::Unsupported;
  EXPECT_EQ(statusOf(e, "agent_capabilities"), DoctorStatus::Warn);
  e.agent.xattr = X::Error;
  EXPECT_EQ(statusOf(e, "agent_capabilities"), DoctorStatus::Warn);
  e.agent.xattr = X::Absent;
  e.agent.setuidRoot = true;
  EXPECT_EQ(statusOf(e, "agent_capabilities"), DoctorStatus::Pass);
  e.agent = {};
  const DoctorCheck missing = find(runDoctorChecks(e), "agent_capabilities");
  EXPECT_EQ(missing.status, DoctorStatus::Warn);
  EXPECT_NE(missing.hint.find("--agent"), std::string::npos);
}

TEST(Doctor, TracefsStates) {
  Machine m;
  EXPECT_EQ(statusOf(m.env(), "tracefs"), DoctorStatus::Pass);
  fs::remove(m.sys() / "kernel/tracing/events/sched/sched_waking/id");
  EXPECT_EQ(statusOf(m.env(), "tracefs"), DoctorStatus::Warn);  // wakers missing
  write(m.sys() / "kernel/tracing/events/sched/sched_wakeup/id", "318\n");
  EXPECT_EQ(statusOf(m.env(), "tracefs"), DoctorStatus::Pass);  // sched_wakeup is accepted
  fs::remove(m.sys() / "kernel/tracing/events/sched/sched_switch/id");
  EXPECT_EQ(statusOf(m.env(), "tracefs"), DoctorStatus::Warn);  // format visible, id not readable
  fs::remove_all(m.sys() / "kernel/tracing");
  const DoctorCheck none = find(runDoctorChecks(m.env()), "tracefs");
  EXPECT_EQ(none.status, DoctorStatus::Warn);
  EXPECT_NE(none.hint.find("mount -t tracefs"), std::string::npos);
  // The debugfs location is tried too.
  const fs::path dbg = m.sys() / "kernel/debug/tracing/events/sched";
  write(dbg / "sched_switch/format", "x");
  write(dbg / "sched_switch/id", "1\n");
  write(dbg / "sched_waking/id", "2\n");
  EXPECT_EQ(statusOf(m.env(), "tracefs"), DoctorStatus::Pass);
}

TEST(Doctor, TracefsWarnsWhenParanoidDeniesRawTracepoints) {
  Machine m;
  m.status(0);
  m.sysctl("perf_event_paranoid", "1");
  DoctorEnv e = m.env();
  e.agent.xattr = DoctorAgentFile::Xattr::Absent;
  EXPECT_EQ(statusOf(e, "tracefs"), DoctorStatus::Warn);
}

TEST(Doctor, TrialOpenStates) {
  Machine m;
  DoctorEnv e = m.env();
  e.trialSystemWide = [] { return EACCES; };
  // This process holds the capability, so the system-wide failure with a working per-process open is a
  // warning.
  DoctorReport r = runDoctorChecks(e);
  EXPECT_EQ(find(r, "perf_open_system").status, DoctorStatus::Warn);
  EXPECT_EQ(find(r, "perf_open_process").status, DoctorStatus::Pass);
  EXPECT_NE(find(r, "perf_open_system").detail.find("errno 13"), std::string::npos);

  e.trialPerProcess = [] { return EPERM; };
  r = runDoctorChecks(e);
  EXPECT_EQ(find(r, "perf_open_system").status,
            DoctorStatus::Fail);  // both fail: perf_event_open is unusable
  EXPECT_EQ(find(r, "perf_open_process").status, DoctorStatus::Fail);
  EXPECT_EQ(r.exitCode(), 1);
  EXPECT_NE(find(r, "perf_open_process").hint.find("seccomp"), std::string::npos);

  e.trialSystemWide = [] { return ENOSYS; };
  EXPECT_NE(find(runDoctorChecks(e), "perf_open_system").hint.find("seccomp"), std::string::npos);
  e.trialSystemWide = [] { return EINVAL; };
  EXPECT_NE(find(runDoctorChecks(e), "perf_open_system").hint.find("8000"), std::string::npos);
  e.trialSystemWide = [] { return EMFILE; };
  EXPECT_NE(find(runDoctorChecks(e), "perf_open_system").hint.find("ulimit"), std::string::npos);

  e.trialSystemWide = nullptr;
  e.trialPerProcess = nullptr;
  r = runDoctorChecks(e);
  EXPECT_EQ(find(r, "perf_open_system").status, DoctorStatus::Warn);
  EXPECT_EQ(find(r, "perf_open_process").status, DoctorStatus::Warn);
  EXPECT_NE(find(r, "perf_open_system").detail.find("unknown"), std::string::npos);
}

TEST(Doctor, TrialFailureOfAnUnprivilegedDoctorWithAPrivilegedAgentIsOnlyAWarning) {
  Machine m;
  m.status(0);
  DoctorEnv e = m.env();  // the agent binary carries CAP_PERFMON
  e.trialSystemWide = [] { return EACCES; };
  e.trialPerProcess = [] { return EACCES; };
  const DoctorReport r = runDoctorChecks(e);
  EXPECT_EQ(find(r, "perf_open_system").status, DoctorStatus::Warn);
  EXPECT_EQ(find(r, "perf_open_process").status, DoctorStatus::Warn);
  EXPECT_EQ(r.exitCode(), 0);
}

TEST(Doctor, UnprivilegedUserCanStillSampleOwnProcesses) {
  Machine m;
  m.status(0);
  m.sysctl("perf_event_paranoid", "2");
  DoctorEnv e = m.env();
  e.agent.xattr = DoctorAgentFile::Xattr::Absent;
  e.trialSystemWide = [] { return EACCES; };
  const DoctorReport r = runDoctorChecks(e);
  EXPECT_EQ(find(r, "perf_open_system").status, DoctorStatus::Warn);
  EXPECT_EQ(find(r, "perf_open_process").status, DoctorStatus::Pass);
  EXPECT_EQ(r.exitCode(), 0);
}

TEST(Doctor, FileDescriptorLimit) {
  Machine m;  // 8 cpus: comfortable is 280
  m.limits("1024", "unlimited");
  EXPECT_EQ(statusOf(m.env(), "rlimit_nofile"), DoctorStatus::Pass);
  m.limits("100", "unlimited");
  const DoctorCheck warn = find(runDoctorChecks(m.env()), "rlimit_nofile");
  EXPECT_EQ(warn.status, DoctorStatus::Warn);
  EXPECT_NE(warn.hint.find("ulimit -n 280"), std::string::npos);
  m.limits("30", "unlimited");
  EXPECT_EQ(statusOf(m.env(), "rlimit_nofile"), DoctorStatus::Fail);
  m.limits("unlimited", "unlimited");
  EXPECT_EQ(statusOf(m.env(), "rlimit_nofile"), DoctorStatus::Pass);
  fs::remove(m.proc() / "self/limits");
  EXPECT_EQ(statusOf(m.env(), "rlimit_nofile"), DoctorStatus::Warn);
}

TEST(Doctor, MemlockLimit) {
  Machine m;
  m.limits("1024", "65536");
  EXPECT_EQ(statusOf(m.env(), "rlimit_memlock"), DoctorStatus::Pass);  // privileged: exempt
  m.status(0);
  m.sysctl("perf_event_paranoid", "2");
  DoctorEnv e = m.env();
  e.agent.xattr = DoctorAgentFile::Xattr::Absent;
  EXPECT_EQ(statusOf(e, "rlimit_memlock"), DoctorStatus::Warn);
  m.limits("1024", std::to_string(16 * 1024 * 1024));
  EXPECT_EQ(statusOf(e, "rlimit_memlock"), DoctorStatus::Pass);
  m.limits("1024", "unlimited");
  EXPECT_EQ(statusOf(e, "rlimit_memlock"), DoctorStatus::Pass);
  fs::remove(m.proc() / "self/limits");
  EXPECT_EQ(statusOf(e, "rlimit_memlock"), DoctorStatus::Warn);
}

TEST(Doctor, PtraceScope) {
  Machine m;
  EXPECT_EQ(statusOf(m.env(), "ptrace_scope"), DoctorStatus::Pass);  // no Yama directory
  m.sysctl("yama/ptrace_scope", "1");
  EXPECT_EQ(statusOf(m.env(), "ptrace_scope"), DoctorStatus::Pass);
  m.sysctl("yama/ptrace_scope", "2");
  EXPECT_EQ(statusOf(m.env(), "ptrace_scope"), DoctorStatus::Pass);  // CAP_SYS_PTRACE present
  m.status(kPerfmon);
  DoctorEnv e = m.env();
  e.agent.xattrBlob = capBlob(3, true, kPerfmon);
  EXPECT_EQ(statusOf(e, "ptrace_scope"), DoctorStatus::Warn);
  m.sysctl("yama/ptrace_scope", "3");
  const DoctorCheck three = find(runDoctorChecks(e), "ptrace_scope");
  EXPECT_EQ(three.status, DoctorStatus::Warn);
  EXPECT_NE(three.detail.find("blocked"), std::string::npos);
  m.sysctl("yama/ptrace_scope", "0");
  EXPECT_EQ(statusOf(e, "ptrace_scope"), DoctorStatus::Pass);
  fs::remove(m.proc() / "sys/kernel/yama/ptrace_scope");
  EXPECT_EQ(statusOf(e, "ptrace_scope"), DoctorStatus::Warn);
}

TEST(Doctor, SocketDirectory) {
  Machine m;
  DoctorEnv e = m.env();
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Pass);
  e.socketAddress = (m.dir.path() / "missing/tachyon-agent.sock").string();
  const DoctorCheck missing = find(runDoctorChecks(e), "socket_dir");
  EXPECT_EQ(missing.status, DoctorStatus::Fail);
  EXPECT_NE(missing.hint.find("TACHYON_AGENT_PIPE"), std::string::npos);
  e = m.env();
  e.isWritableDir = [](const fs::path&) { return false; };
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Fail);
  e = m.env();
  e.socketAddress = (m.dir.path() / "run" / std::string(120, 'x')).string();
  const DoctorCheck longPath = find(runDoctorChecks(e), "socket_dir");
  EXPECT_EQ(longPath.status, DoctorStatus::Fail);
  EXPECT_NE(longPath.detail.find("longer than"), std::string::npos);
  e.socketAddress.clear();
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Warn);
}

TEST(Doctor, SymbolSources) {
  Machine m;
  DoctorEnv e = m.env();
  EXPECT_EQ(statusOf(e, "symbols"), DoctorStatus::Pass);  // .build-id directory
  e.buildIdDirs = {m.dir.path() / "nope"};
  const DoctorCheck warn = find(runDoctorChecks(e), "symbols");
  EXPECT_EQ(warn.status, DoctorStatus::Warn);
  EXPECT_NE(warn.hint.find("DEBUGINFOD_URLS"), std::string::npos);
  e.debuginfodUrls = "https://debuginfod.example.org";
  EXPECT_EQ(statusOf(e, "symbols"), DoctorStatus::Pass);
}

TEST(Doctor, FileCapabilityParsing) {
  const FileCapabilities v3 = parseFileCapabilities(capBlob(3, true, kPerfmon | kSysPtrace));
  ASSERT_TRUE(v3.valid);
  EXPECT_EQ(v3.revision, 3);
  EXPECT_TRUE(v3.effective);
  EXPECT_EQ(v3.permitted, kPerfmon | kSysPtrace);
  const FileCapabilities v2 = parseFileCapabilities(capBlob(2, false, kSysAdmin));
  ASSERT_TRUE(v2.valid);
  EXPECT_FALSE(v2.effective);
  EXPECT_EQ(v2.permitted, kSysAdmin);
  // Revision 1: 32-bit sets only.
  std::string v1;
  for (uint32_t w : {0x01000001u, static_cast<uint32_t>(kSysAdmin), 0u})
    for (int i = 0; i < 4; ++i) v1.push_back(static_cast<char>((w >> (8 * i)) & 0xFF));
  const FileCapabilities c1 = parseFileCapabilities(v1);
  ASSERT_TRUE(c1.valid);
  EXPECT_EQ(c1.revision, 1);
  EXPECT_EQ(c1.permitted, kSysAdmin);
  EXPECT_FALSE(parseFileCapabilities("").valid);
  EXPECT_FALSE(parseFileCapabilities("abc").valid);
  EXPECT_FALSE(parseFileCapabilities(capBlob(2, true, kPerfmon).substr(0, 19)).valid);
  std::string bad = capBlob(2, true, kPerfmon);
  bad[3] = 7;  // unknown revision
  EXPECT_FALSE(parseFileCapabilities(bad).valid);
  EXPECT_FALSE(parseFileCapabilities(bad).error.empty());
}

TEST(Doctor, JsonSchemaIsStable) {
  Machine m;
  m.sysctl("perf_event_paranoid", "3");
  m.status(0);
  DoctorEnv e = m.env();
  e.agent.xattr = DoctorAgentFile::Xattr::Absent;
  std::ostringstream out;
  const int code = runDoctorWithEnv(e, true, out);
  EXPECT_EQ(code, 1);  // paranoid 3 without privileges
  const nlohmann::json j = nlohmann::json::parse(out.str());
  EXPECT_EQ(out.str().back(), '\n');
  EXPECT_EQ(out.str().find('\n'), out.str().size() - 1);  // one line
  std::set<std::string> keys;
  for (auto it = j.begin(); it != j.end(); ++it) keys.insert(it.key());
  EXPECT_EQ(keys, (std::set<std::string>{"schema", "platform", "ok", "exit_code", "requested_hz", "summary",
                                         "checks"}));
  EXPECT_EQ(j["schema"], 1);
  EXPECT_EQ(j["platform"], "linux");
  EXPECT_EQ(j["ok"], false);
  EXPECT_EQ(j["exit_code"], 1);
  EXPECT_EQ(j["requested_hz"], 8000);
  ASSERT_TRUE(j["summary"].is_object());
  EXPECT_EQ(j["summary"].size(), 3u);
  int counted[3] = {0, 0, 0};
  std::vector<std::string> ids;
  for (const auto& c : j["checks"]) {
    std::set<std::string> ck;
    for (auto it = c.begin(); it != c.end(); ++it) ck.insert(it.key());
    EXPECT_EQ(ck, (std::set<std::string>{"id", "title", "status", "detail", "hint"}));
    for (const char* k : {"id", "title", "status", "detail", "hint"}) EXPECT_TRUE(c[k].is_string()) << k;
    const std::string st = c["status"];
    EXPECT_TRUE(st == "PASS" || st == "WARN" || st == "FAIL") << st;
    ++counted[st == "PASS" ? 0 : st == "WARN" ? 1 : 2];
    ids.push_back(c["id"]);
  }
  EXPECT_EQ(j["summary"]["pass"], counted[0]);
  EXPECT_EQ(j["summary"]["warn"], counted[1]);
  EXPECT_EQ(j["summary"]["fail"], counted[2]);
  EXPECT_EQ(ids, (std::vector<std::string>{"kernel_version", "process_capabilities", "agent_capabilities",
                                           "perf_event_paranoid", "max_sample_rate", "cpu_time_max_percent",
                                           "perf_event_mlock_kb", "tracefs", "perf_open_system",
                                           "perf_open_process", "rlimit_nofile", "rlimit_memlock",
                                           "ptrace_scope", "socket_dir", "symbols"}));
}

TEST(Doctor, ExitCodeFollowsFailNotWarn) {
  Machine m;
  std::ostringstream out;
  EXPECT_EQ(runDoctorWithEnv(m.env(), false, out), 0);
  EXPECT_NE(out.str().find("[PASS] Kernel version"), std::string::npos) << out.str();
  EXPECT_NE(out.str().find("this machine is ready"), std::string::npos);

  m.sysctl("perf_cpu_time_max_percent", "1");  // WARN only
  std::ostringstream warn;
  EXPECT_EQ(runDoctorWithEnv(m.env(), false, warn), 0);
  EXPECT_NE(warn.str().find("[WARN]"), std::string::npos);
  EXPECT_NE(warn.str().find("fix: "), std::string::npos);

  m.sysctl("perf_event_max_sample_rate", "100");  // FAIL
  std::ostringstream fail;
  EXPECT_EQ(runDoctorWithEnv(m.env(), false, fail), 1);
  EXPECT_NE(fail.str().find("[FAIL]"), std::string::npos);
}

#if !defined(__linux__)
TEST(Doctor, OtherPlatformsPrintANoteAndSucceed) {
  std::ostringstream out, err;
  EXPECT_EQ(cli::run({"doctor"}, out, err), kExitOk);
  EXPECT_NE(out.str().find("not implemented"), std::string::npos);
  std::ostringstream json;
  EXPECT_EQ(cli::run({"doctor", "--json"}, json, err), kExitOk);
  EXPECT_EQ(nlohmann::json::parse(json.str())["ok"], true);
}
#endif

#if defined(__linux__)
TEST(Doctor, CliUsageErrors) {
  std::ostringstream out, err;
  EXPECT_EQ(cli::run({"doctor", "--hz", "abc"}, out, err), kExitUsage);
  EXPECT_EQ(cli::run({"doctor", "--bogus"}, out, err), kExitUsage);
  EXPECT_EQ(cli::run({"doctor", "--agent"}, out, err), kExitUsage);
  EXPECT_EQ(cli::run({"doctor", "--hz", "0"}, out, err), kExitUsage);
}

// Smoke test on the machine running the tests: no crash, valid JSON, an exit code that matches the report.
TEST(Doctor, RealMachineSmoke) {
  std::ostringstream out, err;
  const int code = cli::run({"doctor", "--json", "--hz", "1000"}, out, err);
  ASSERT_TRUE(code == 0 || code == 1) << code;
  const nlohmann::json j = nlohmann::json::parse(out.str());
  EXPECT_EQ(j["schema"], 1);
  EXPECT_EQ(j["exit_code"], code);
  EXPECT_EQ(j["ok"], code == 0);
  EXPECT_EQ(j["checks"].size(), 15u);
  EXPECT_EQ(find(runDoctorChecks(makeSystemDoctorEnv(1000, "/tmp/x.sock", "")), "kernel_version").status,
            DoctorStatus::Pass);
  std::ostringstream text, terr;
  EXPECT_EQ(cli::run({"doctor"}, text, terr), code);
  EXPECT_NE(text.str().find("tachyon-cli doctor"), std::string::npos);
}
#endif

TEST(Doctor, RelativeSocketIsResolvedAgainstTheCurrentDirectory) {
  Machine m;
  DoctorEnv e = m.env();
  // A bare file name is a socket in the current directory, not a socket in an unwritable "" directory.
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  e.socketAddress = "tachyon-agent.sock";
  std::vector<fs::path> asked;
  e.isWritableDir = [&](const fs::path& d) {
    asked.push_back(d);
    return true;
  };
  const DoctorCheck bare = find(runDoctorChecks(e), "socket_dir");
  EXPECT_EQ(bare.status, DoctorStatus::Pass) << bare.detail;
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], cwd);
  EXPECT_NE(bare.detail.find((cwd / "tachyon-agent.sock").string()), std::string::npos) << bare.detail;
  asked.clear();
  e.socketAddress = "sub/dir/tachyon-agent.sock";
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Pass);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], cwd / "sub/dir");
}

TEST(Doctor, SocketDirectoryShouldBePrivateToTheUser) {
  Machine m;
  DoctorEnv e = m.env();
  e.uid = 1000;
  uint32_t mode = 0700, owner = 1000;
  e.statDir = [&](const fs::path&, uint32_t& m_, uint32_t& o_) {
    m_ = mode;
    o_ = owner;
    return true;
  };
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Pass) << "0700 and ours";
  mode = 0755;
  DoctorCheck open = find(runDoctorChecks(e), "socket_dir");
  EXPECT_EQ(open.status, DoctorStatus::Warn);
  EXPECT_NE(open.detail.find("0755"), std::string::npos) << open.detail;
  EXPECT_NE(open.hint.find("chmod 700"), std::string::npos) << open.hint;
  mode = 01777;  // /tmp
  owner = 0;
  open = find(runDoctorChecks(e), "socket_dir");
  EXPECT_EQ(open.status, DoctorStatus::Warn);
  EXPECT_NE(open.detail.find("not by you"), std::string::npos) << open.detail;
  mode = 0700;  // private but somebody else's
  owner = 33;
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Warn);
  owner = 1000;
  mode = 0750;
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Warn) << "group access counts";
  // A warning never makes doctor fail.
  EXPECT_EQ(runDoctorChecks(e).exitCode(), 0);
  // An unreadable directory does not hide the check's other results.
  e.statDir = [](const fs::path&, uint32_t&, uint32_t&) { return false; };
  EXPECT_EQ(statusOf(e, "socket_dir"), DoctorStatus::Pass);
}

TEST(Doctor, PtraceScopeOneWordingIsAccurate) {
  Machine m;
  m.sysctl("yama/ptrace_scope", "1");
  m.status(kPerfmon);
  DoctorEnv e = m.env();
  e.agent.xattrBlob = capBlob(3, true, kPerfmon);  // no CAP_SYS_PTRACE
  const DoctorCheck noCap = find(runDoctorChecks(e), "ptrace_scope");
  EXPECT_EQ(noCap.status, DoctorStatus::Pass);
  EXPECT_NE(noCap.detail.find("only for descendants"), std::string::npos) << noCap.detail;
  EXPECT_NE(noCap.detail.find("CAP_SYS_PTRACE"), std::string::npos);
  EXPECT_EQ(noCap.detail.find("other processes of the same user"), std::string::npos) << noCap.detail;
  m.status(kPerfmon | kSysPtrace);
  e.agent.xattrBlob = capBlob(3, true, kPerfmon | kSysPtrace);
  const DoctorCheck withCap = find(runDoctorChecks(e), "ptrace_scope");
  EXPECT_EQ(withCap.status, DoctorStatus::Pass);
  EXPECT_NE(withCap.detail.find("CAP_SYS_PTRACE is present"), std::string::npos) << withCap.detail;
  EXPECT_EQ(withCap.detail.find("only for descendants"), std::string::npos) << withCap.detail;
}

}  // namespace
}  // namespace tachyon::cli
