// WP-70: /proc parsing and rundown, error messages, cpu lists, environment merging and the stop-on-start
// launch handshake (real fork/exec of /bin/sh style helpers; no perf_event_open needed).
#include "capture/linux_perf/launch.h"
#include "capture/linux_perf/perf_system.h"
#include "capture/linux_perf/proc_snapshot.h"
#include "linux_perf_test_util.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <set>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;

using ptest::TempDir;
using ptest::waitFor;

std::string selfExe() {
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

std::string exeOf(uint32_t pid) {
  char buf[4096];
  const std::string link = "/proc/" + std::to_string(pid) + "/exe";
  const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

// --- /proc parsing -----------------------------------------------------------------------------------------

TEST(ProcParse, MapsLines) {
  MapsLine m;
  ASSERT_TRUE(
      parseMapsLine("7f1234560000-7f1234588000 r-xp 00001000 fd:01 1234567                    "
                    "/usr/lib/x86_64-linux-gnu/libc.so.6",
                    m));
  EXPECT_EQ(m.start, 0x7f1234560000ull);
  EXPECT_EQ(m.end, 0x7f1234588000ull);
  EXPECT_TRUE(m.read);
  EXPECT_FALSE(m.write);
  EXPECT_TRUE(m.exec);
  EXPECT_FALSE(m.shared);
  EXPECT_EQ(m.offset, 0x1000u);
  EXPECT_EQ(m.devMajor, 0xfdu);
  EXPECT_EQ(m.devMinor, 1u);
  EXPECT_EQ(m.inode, 1234567u);
  EXPECT_EQ(m.path, "/usr/lib/x86_64-linux-gnu/libc.so.6");

  ASSERT_TRUE(
      parseMapsLine("55d0c0a00000-55d0c0a01000 r-xp 00000000 08:02 99 /opt/my dir/prog (deleted)", m));
  EXPECT_EQ(m.path, "/opt/my dir/prog (deleted)") << "spaces in the path are kept";

  ASSERT_TRUE(
      parseMapsLine("7ffd5f0c0000-7ffd5f0e1000 rw-p 00000000 00:00 0                          [stack]", m));
  EXPECT_EQ(m.path, "[stack]");
  EXPECT_FALSE(m.exec);

  ASSERT_TRUE(parseMapsLine("7f0000000000-7f0000001000 rwxp 00000000 00:00 0 ", m));
  EXPECT_TRUE(m.path.empty()) << "anonymous";

  ASSERT_TRUE(parseMapsLine("400000-401000 r--s 00000000 00:2a 5 /dev/shm/x", m));
  EXPECT_TRUE(m.shared);
}

TEST(ProcParse, MapsLineRejectsGarbage) {
  MapsLine m;
  EXPECT_FALSE(parseMapsLine("", m));
  EXPECT_FALSE(parseMapsLine("not a maps line", m));
  EXPECT_FALSE(parseMapsLine("zz-10 r-xp 0 00:00 0", m));
  EXPECT_FALSE(parseMapsLine("1000-500 r-xp 0 00:00 0", m)) << "end before start";
  EXPECT_FALSE(parseMapsLine("1000-2000 r-x 0 00:00 0", m)) << "short permission field";
  EXPECT_FALSE(parseMapsLine("1000-2000 r-xp 0 0000 0", m)) << "device without a colon";
  EXPECT_FALSE(parseMapsLine("1000-2000 r-xp 0 00:00", m)) << "missing inode";
}

TEST(ProcParse, StatLineWithAwkwardComm) {
  ProcStat st;
  ASSERT_TRUE(parseProcStat("1234 (my (weird) name) S 99 1234 1234 0 -1 4194560", st));
  EXPECT_EQ(st.comm, "my (weird) name");
  EXPECT_EQ(st.state, 'S');
  EXPECT_EQ(st.ppid, 99u);
  EXPECT_FALSE(parseProcStat("garbage", st));
  EXPECT_FALSE(parseProcStat("1 (a) S", st));
  EXPECT_FALSE(parseProcStat("1 (a) S x", st));
}

TEST(ProcParse, CpuLists) {
  EXPECT_EQ(parseCpuList("0-3\n"), (std::vector<uint32_t>{0, 1, 2, 3}));
  EXPECT_EQ(parseCpuList("0,2-3,8"), (std::vector<uint32_t>{0, 2, 3, 8}));
  EXPECT_EQ(parseCpuList("5"), std::vector<uint32_t>{5});
  EXPECT_TRUE(parseCpuList("").empty());
  EXPECT_TRUE(parseCpuList("a-b").empty());
  EXPECT_TRUE(parseCpuList("3-1").empty());
  EXPECT_TRUE(parseCpuList("0-99999999").empty()) << "absurd values do not allocate gigabytes";
}

// --- rundown over a fake /proc ----------------------------------------------------------------------------

TEST(Rundown, ProducesForkCommAndExecutableMmap2Records) {
  TempDir dir;
  ptest::writeFakeProcess(dir.path(), 100, 1, "init", {100}, {});
  ptest::writeFakeProcess(
      dir.path(), 200, 100, "app", {200, 201, 202},
      {"400000-401000 r--p 00000000 08:01 55 /opt/app", "401000-405000 r-xp 00001000 08:01 55 /opt/app",
       "7f0000000000-7f0000001000 r-xp 00000000 00:00 0",
       "7f1000000000-7f1000010000 r-xp 00000000 08:01 77 /lib/libx.so (deleted)",
       "7ffd00000000-7ffd00001000 r-xp 00000000 00:00 0 [vdso]"});
  ptest::writeFakeProcess(dir.path(), 300, 200, "child", {300},
                          {"500000-501000 r-xp 00000000 08:01 88 /opt/child"});
  ptest::writeFakeProcess(dir.path(), 400, 1, "other", {400}, {"600000-601000 r-xp 0 08:01 99 /opt/other"});
  std::ofstream(dir.path() / "notapid");  // ignored

  RundownOptions ro;
  ro.procRoot = dir.path();
  ro.rootPids = {200};
  ro.includeDescendants = true;
  ro.ts = 5000;
  RundownResult res = buildRundown(ro);
  EXPECT_EQ(res.pids, (std::vector<uint32_t>{200, 300})) << "the root and its descendant, nothing else";
  EXPECT_EQ(res.threads, 4u);
  EXPECT_EQ(res.mappings, 3u) << "only executable file mappings: the anonymous and [vdso] ones are skipped";

  std::vector<std::string> comms;
  std::vector<std::string> maps;
  std::vector<uint32_t> forks;
  size_t consumed = 0;
  EXPECT_EQ(forEachRecord(std::span<const uint8_t>(res.records), consumed,
                          [&](std::span<const uint8_t> rec) {
                            ParsedRecord r;
                            ASSERT_EQ(parseRecord(rec, ro.layout, r), ParseStatus::Ok);
                            EXPECT_EQ(r.time, 5000u) << "rundown records carry the session start time";
                            if (r.type() == kRecordComm)
                              comms.push_back(std::to_string(r.tid) + ":" + std::string(r.name));
                            if (r.type() == kRecordMmap2)
                              maps.push_back(std::to_string(r.pid) + ":" + std::string(r.name) + "@" +
                                             std::to_string(r.mapAddr));
                            if (r.type() == kRecordFork) forks.push_back(r.tid);
                          }),
            ParseStatus::Ok);
  EXPECT_EQ(comms, (std::vector<std::string>{"200:app", "201:app", "202:app", "300:child"}));
  EXPECT_EQ(forks, (std::vector<uint32_t>{200, 201, 202, 300}));
  ASSERT_EQ(maps.size(), 3u);
  EXPECT_EQ(maps[0], "200:/opt/app@" + std::to_string(0x401000));
  EXPECT_EQ(maps[1], "200:/lib/libx.so (deleted)@" + std::to_string(0x7f1000000000ull));
}

TEST(Rundown, SystemModeCoversEveryProcessAndSurvivesVanishedOnes) {
  TempDir dir;
  ptest::writeFakeProcess(dir.path(), 10, 1, "a", {10}, {});
  ptest::writeFakeProcess(dir.path(), 11, 1, "b", {11}, {});
  fs::create_directories(dir.path() / "12");  // no stat: vanished mid-read
  RundownOptions ro;
  ro.procRoot = dir.path();
  ro.ts = 1;
  EXPECT_EQ(buildRundown(ro).pids, (std::vector<uint32_t>{10, 11}));
  ro.rootPids = {99999};
  EXPECT_TRUE(buildRundown(ro).pids.empty());
}

TEST(Rundown, ProcessAliveTreatsZombiesAsDeadUnlessThreadsRemain) {
  TempDir dir;
  ptest::writeFakeProcess(dir.path(), 10, 1, "live", {10}, {}, 'S');
  ptest::writeFakeProcess(dir.path(), 11, 1, "zombie", {11}, {}, 'Z');
  ptest::writeFakeProcess(dir.path(), 12, 1, "zombie-leader", {12, 13}, {}, 'Z');
  EXPECT_TRUE(processAlive(dir.path(), 10));
  EXPECT_FALSE(processAlive(dir.path(), 11));
  EXPECT_TRUE(processAlive(dir.path(), 12)) << "a zombie leader whose threads still run is a live process";
  EXPECT_FALSE(processAlive(dir.path(), 999));
}

TEST(Rundown, RealProcOfThisProcessIsReadable) {
  RundownOptions ro;
  ro.rootPids = {static_cast<uint32_t>(::getpid())};
  ro.ts = 1;
  RundownResult res = buildRundown(ro);
  ASSERT_EQ(res.pids.size(), 1u);
  EXPECT_GE(res.threads, 1u);
  EXPECT_GE(res.mappings, 1u) << "the test binary has an executable mapping";
}

// --- error messages ---------------------------------------------------------------------------------------

TEST(PerfErrors, MessagesNameTheKnobsToTurn) {
  EventConfig cfg;
  cfg.samplingHz = 50000;
  SysctlInfo sys;
  sys.paranoid = 4;
  sys.maxSampleRate = 25000;
  sys.cpuTimeMaxPercent = 25;
  sys.mlockKb = 516;

  Error denied = describeOpenError(EACCES, cfg, sys, -1, 3);
  EXPECT_EQ(denied.code, ErrorCode::NoPrivilege);
  EXPECT_NE(denied.message.find("kernel.perf_event_paranoid is 4"), std::string::npos) << denied.message;
  EXPECT_NE(denied.message.find("CAP_PERFMON"), std::string::npos);
  EXPECT_NE(denied.message.find("seccomp"), std::string::npos);
  EXPECT_NE(denied.message.find("cpu 3"), std::string::npos);

  Error perThread = describeOpenError(EPERM, cfg, sys, 1234, -1);
  EXPECT_EQ(perThread.code, ErrorCode::NoPrivilege);
  EXPECT_NE(perThread.message.find("thread 1234"), std::string::npos);

  Error rate = describeOpenError(EINVAL, cfg, sys, -1, 0);
  EXPECT_EQ(rate.code, ErrorCode::InvalidArgument);
  EXPECT_NE(rate.message.find("kernel.perf_event_max_sample_rate"), std::string::npos) << rate.message;
  EXPECT_NE(rate.message.find("25000"), std::string::npos);
  EXPECT_NE(rate.message.find("perf_cpu_time_max_percent"), std::string::npos);

  cfg.samplingHz = 1000;
  Error invalid = describeOpenError(EINVAL, cfg, sys, -1, 0);
  EXPECT_EQ(invalid.code, ErrorCode::InvalidArgument);
  EXPECT_EQ(invalid.message.find("max_sample_rate"), std::string::npos)
      << "the rate is fine: do not blame it";

  EXPECT_EQ(describeOpenError(ESRCH, cfg, sys, 5, -1).code, ErrorCode::NotFound);
  EXPECT_EQ(describeOpenError(ENOENT, cfg, sys, -1, 0).code, ErrorCode::Unsupported);
  EXPECT_EQ(describeOpenError(ENODEV, cfg, sys, -1, 0).code, ErrorCode::Unsupported);
  Error fds = describeOpenError(EMFILE, cfg, sys, -1, 0);
  EXPECT_NE(fds.message.find("ulimit -n"), std::string::npos);
  Error mlock = describeOpenError(EPERM, cfg, sys, -1, 0, true);
  EXPECT_NE(mlock.message.find("perf_event_mlock_kb"), std::string::npos) << mlock.message;
  EXPECT_NE(mlock.message.find("516"), std::string::npos);
}

TEST(PerfErrors, ThrottleAdviceNamesBothSysctls) {
  SysctlInfo sys;
  sys.maxSampleRate = 15000;
  sys.cpuTimeMaxPercent = 25;
  const std::string advice = throttleAdvice(sys, 8000);
  EXPECT_NE(advice.find("kernel.perf_event_max_sample_rate"), std::string::npos);
  EXPECT_NE(advice.find("kernel.perf_cpu_time_max_percent"), std::string::npos);
  EXPECT_NE(advice.find("15000"), std::string::npos);
  EXPECT_NE(advice.find("8000"), std::string::npos);
}

// --- environment and executable lookup --------------------------------------------------------------------

TEST(LaunchUtil, MergeEnvironmentReplacesInPlaceAndAppends) {
  const std::vector<std::string> parent = {"A=1", "PATH=/bin", "B=2", "NOEQUALS"};
  auto merged = mergeEnvironment(parent, {{"B", "two"}, {"TACHYON_AGENT_PIPE", "/run/x.sock"}, {"AA", "x"}});
  EXPECT_EQ(merged, (std::vector<std::string>{"A=1", "PATH=/bin", "B=two", "NOEQUALS", "AA=x",
                                              "TACHYON_AGENT_PIPE=/run/x.sock"}));
  EXPECT_EQ(mergeEnvironment(parent, {}), parent);
}

TEST(LaunchUtil, ResolveExecutable) {
  TempDir dir;
  const auto tool = dir.path() / "mytool";
  {
    std::ofstream(tool) << "#!/bin/sh\nexit 0\n";
    fs::permissions(tool, fs::perms::owner_all);
  }
  std::ofstream(dir.path() / "plain") << "x";  // not executable
  auto found = resolveExecutable("mytool", "/nonexistent:" + dir.path().string(), "");
  ASSERT_TRUE(found.ok());
  EXPECT_EQ(*found, tool.string());
  EXPECT_EQ(resolveExecutable("plain", dir.path().string(), "").error().code, ErrorCode::NotFound);
  EXPECT_EQ(resolveExecutable("missing", dir.path().string(), "").error().code, ErrorCode::NotFound);
  EXPECT_EQ(resolveExecutable(tool.string(), "", "").value(), tool.string());
  EXPECT_EQ(resolveExecutable("./mytool", "", dir.path().string()).value(), "./mytool")
      << "relative to the cwd";
  EXPECT_EQ(resolveExecutable("", "/bin", "").error().code, ErrorCode::InvalidArgument);
}

// --- the stop-on-start handshake -------------------------------------------------------------------------

TEST(LaunchHandshake, ChildDoesNotExecUntilReleased) {
  LaunchSpec spec;
  spec.exe = "/bin/sh";
  spec.args = {"-c", "exit 7"};
  auto spawned = LaunchedProcess::spawnParked(spec);
  ASSERT_TRUE(spawned.ok()) << spawned.error().toString();
  LaunchedProcess& p = *spawned;
  EXPECT_TRUE(p.parked());
  ASSERT_GT(p.pid(), 0u);

  // Parked: still a copy of this program, and it stays one while we take our time.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(exeOf(p.pid()), selfExe()) << "the target has not exec'd yet";
  EXPECT_FALSE(p.exited());

  ASSERT_TRUE(p.release().ok());
  EXPECT_FALSE(p.parked());
  ASSERT_TRUE(waitFor([&] { return p.exited(); }));
  EXPECT_TRUE(WIFEXITED(p.waitStatus()));
  EXPECT_EQ(WEXITSTATUS(p.waitStatus()), 7) << "the program ran after the release";
  EXPECT_EQ(p.release().error().code, ErrorCode::Busy) << "a second release is refused";
}

TEST(LaunchHandshake, ReleaseReturnsOnlyAfterTheExec) {
  LaunchSpec spec;
  spec.exe = "/bin/sleep";
  spec.args = {"30"};
  auto spawned = LaunchedProcess::spawnParked(spec);
  ASSERT_TRUE(spawned.ok());
  ASSERT_TRUE(spawned->release().ok());
  const std::string exe = exeOf(spawned->pid());
  EXPECT_NE(exe, selfExe()) << "release() waits for execve, so the new image is already in place";
  EXPECT_NE(exe.find("sleep"), std::string::npos) << exe;
  spawned->terminate();
  EXPECT_TRUE(spawned->exited());
}

TEST(LaunchHandshake, ExecFailureIsReportedWithItsReason) {
  TempDir dir;
  const auto notExecutable = dir.path() / "bad";
  {
    std::ofstream(notExecutable) << "just text";
    fs::permissions(notExecutable, fs::perms::owner_all);  // executable bit, but not a program
  }
  LaunchSpec spec;
  spec.exe = notExecutable.string();
  auto spawned = LaunchedProcess::spawnParked(spec);
  ASSERT_TRUE(spawned.ok());
  auto released = spawned->release();
  ASSERT_FALSE(released.ok());
  EXPECT_NE(released.error().message.find("cannot start the target"), std::string::npos)
      << released.error().message;
  EXPECT_TRUE(spawned->exited());
}

TEST(LaunchHandshake, MissingExecutableAndDirectoryAreReportedBeforeForking) {
  LaunchSpec spec;
  spec.exe = "/definitely/not/here";
  auto a = LaunchedProcess::spawnParked(spec);
  ASSERT_FALSE(a.ok());
  EXPECT_EQ(a.error().code, ErrorCode::NotFound);
  spec.exe = "/bin/true";
  spec.cwd = "/definitely/not/here";
  auto b = LaunchedProcess::spawnParked(spec);
  ASSERT_FALSE(b.ok());
  EXPECT_EQ(b.error().code, ErrorCode::NotFound);
}

TEST(LaunchHandshake, ChildSeesEnvironmentCwdAndArguments) {
  TempDir dir;
  const auto out = dir.path() / "out.txt";
  LaunchSpec spec;
  spec.exe = "/bin/sh";
  spec.args = {"-c", "printf '%s|%s|%s' \"$TACHYON_AGENT_PIPE\" \"$1\" \"$(pwd)\" > out.txt", "sh",
               "arg with space"};
  spec.cwd = dir.path().string();
  spec.env = {{"TACHYON_AGENT_PIPE", "/run/tachyon.sock"}};
  auto spawned = LaunchedProcess::spawnParked(spec);
  ASSERT_TRUE(spawned.ok());
  ASSERT_TRUE(spawned->release().ok());
  ASSERT_TRUE(waitFor([&] { return spawned->exited(); }));
  std::ifstream in(out);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(text, "/run/tachyon.sock|arg with space|" + fs::canonical(dir.path()).string());
}

TEST(LaunchHandshake, TargetDoesNotInheritIgnoredSigpipeAndSignalMask) {
  // The agent ignores SIGPIPE; the target must get the default disposition back.
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  struct sigaction old {};
  ::sigaction(SIGPIPE, &ignore, &old);
  TempDir dir;
  LaunchSpec spec;
  spec.exe = "/bin/sh";
  spec.args = {"-c", "grep SigIgn /proc/self/status > out.txt"};
  spec.cwd = dir.path().string();
  auto spawned = LaunchedProcess::spawnParked(spec);
  ::sigaction(SIGPIPE, &old, nullptr);
  ASSERT_TRUE(spawned.ok());
  ASSERT_TRUE(spawned->release().ok());
  ASSERT_TRUE(waitFor([&] { return spawned->exited(); }));
  std::ifstream in(dir.path() / "out.txt");
  std::string line;
  std::getline(in, line);
  const uint64_t ignored = std::strtoull(line.substr(line.find(':') + 1).c_str(), nullptr, 16);
  EXPECT_EQ(ignored & (1ull << (SIGPIPE - 1)), 0u) << line;
}

TEST(LaunchHandshake, DestroyingAParkedProcessKillsItAndAReleasedOneSurvives) {
  uint32_t parkedPid = 0, runningPid = 0;
  {
    LaunchSpec spec;
    spec.exe = "/bin/sleep";
    spec.args = {"30"};
    auto parked = LaunchedProcess::spawnParked(spec);
    ASSERT_TRUE(parked.ok());
    parkedPid = parked->pid();
    auto running = LaunchedProcess::spawnParked(spec);
    ASSERT_TRUE(running.ok());
    runningPid = running->pid();
    ASSERT_TRUE(running->release().ok());
    // `running` goes out of scope released: the target outlives the capture.
  }
  EXPECT_FALSE(processAlive("/proc", parkedPid)) << "a parked child is killed (and reaped) with its handle";
  EXPECT_TRUE(processAlive("/proc", runningPid)) << "a released target keeps running";
  ::kill(static_cast<pid_t>(runningPid), SIGKILL);
  int st = 0;
  ::waitpid(static_cast<pid_t>(runningPid), &st, 0);
}

}  // namespace
}  // namespace tachyon::capture::perf
