// WP-69: the Windows checks of `tachyon-cli doctor`, driven through injected WindowsDoctorEnv callbacks so
// they run on every platform, plus a smoke run of the real calls on Windows.
#include "cli/cli.h"
#include "cli/doctor.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <set>
#include <sstream>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;

// A machine that passes every check.
WindowsDoctorEnv goodEnv() {
  WindowsDoctorEnv e;
  e.requestedHz = 8000;
  e.pipeName = R"(\\.\pipe\tachyon-agent)";
  e.osBuild = 22631;
  e.symbolCacheDir = R"(C:\Users\dev\AppData\Local\Tachyon\symbols)";
  e.agentFound = true;
  e.agentPath = R"(C:\tachyon\tachyon-agent.exe)";
  e.tokenState = [] {
    WindowsTokenState t;
    t.known = true;
    t.elevated = true;
    t.profilePrivilege = WindowsPrivilege::Present;
    return t;
  };
  e.sessions = [] {
    WindowsSessionList s;
    s.ok = true;
    s.names = {"EventLog-Application", "Microsoft-Windows-Rdp-Graphics-RdpIdd-Trace"};
    return s;
  };
  e.probePipe = [](const std::string&) { return WindowsPipeProbe::Reachable; };
  e.cacheDirWritable = [](const fs::path&, std::string&) { return true; };
  e.dirExists = [](const fs::path&) { return true; };
  e.diaAvailable = [](std::string&) { return true; };
  return e;
}

const DoctorCheck& find(const DoctorReport& r, const std::string& id) {
  for (const DoctorCheck& c : r.checks)
    if (c.id == id) return c;
  ADD_FAILURE() << "no check " << id;
  static const DoctorCheck none;
  return none;
}

DoctorStatus statusOf(const WindowsDoctorEnv& e, const std::string& id) {
  return find(runWindowsDoctorChecks(e), id).status;
}

TEST(WindowsDoctor, AHealthyMachinePassesEveryCheck) {
  const DoctorReport r = runWindowsDoctorChecks(goodEnv());
  EXPECT_EQ(r.platform, "windows");
  EXPECT_EQ(r.fail, 0);
  EXPECT_EQ(r.warn, 0);
  EXPECT_EQ(r.pass, 9);
  EXPECT_EQ(r.exitCode(), 0);
  std::set<std::string> ids;
  for (const DoctorCheck& c : r.checks) {
    ids.insert(c.id);
    EXPECT_TRUE(c.hint.empty()) << c.id;
  }
  EXPECT_EQ(ids,
            (std::set<std::string>{"windows_version", "elevation", "sysprofile_privilege", "etw_sessions",
                                   "agent_pipe", "agent_binary", "symbol_cache", "nt_symbol_path", "dia"}));
}

TEST(WindowsDoctor, OldWindowsFails) {
  WindowsDoctorEnv e = goodEnv();
  e.osBuild = 14393;
  const DoctorCheck c = find(runWindowsDoctorChecks(e), "windows_version");
  EXPECT_EQ(c.status, DoctorStatus::Fail);
  EXPECT_FALSE(c.hint.empty());
  e.osBuild = 15063;
  EXPECT_EQ(statusOf(e, "windows_version"), DoctorStatus::Pass);
  e.osBuild = 0;
  EXPECT_EQ(statusOf(e, "windows_version"), DoctorStatus::Warn);
}

TEST(WindowsDoctor, ElevationAndPrivilege) {
  WindowsDoctorEnv e = goodEnv();
  e.tokenState = [] {
    WindowsTokenState t;
    t.known = true;
    t.elevated = false;
    t.profilePrivilege = WindowsPrivilege::Absent;
    return t;
  };
  EXPECT_EQ(statusOf(e, "elevation"), DoctorStatus::Warn);
  EXPECT_EQ(statusOf(e, "sysprofile_privilege"), DoctorStatus::Warn);  // not elevated: expected to be absent
  EXPECT_FALSE(find(runWindowsDoctorChecks(e), "elevation").hint.empty());

  e.tokenState = [] {
    WindowsTokenState t;
    t.known = true;
    t.elevated = true;
    t.profilePrivilege = WindowsPrivilege::Absent;
    return t;
  };
  EXPECT_EQ(statusOf(e, "elevation"), DoctorStatus::Pass);
  const DoctorCheck priv = find(runWindowsDoctorChecks(e), "sysprofile_privilege");
  EXPECT_EQ(priv.status, DoctorStatus::Fail);
  EXPECT_NE(priv.hint.find("secpol"), std::string::npos);

  e.tokenState = [] {
    WindowsTokenState t;
    t.known = true;
    t.elevated = true;
    t.profilePrivilege = WindowsPrivilege::Enabled;
    return t;
  };
  EXPECT_EQ(statusOf(e, "sysprofile_privilege"), DoctorStatus::Pass);

  e.tokenState = [] {
    WindowsTokenState t;
    t.error = "OpenProcessToken failed";
    return t;
  };
  EXPECT_EQ(statusOf(e, "elevation"), DoctorStatus::Warn);
  EXPECT_EQ(statusOf(e, "sysprofile_privilege"), DoctorStatus::Warn);
  e.tokenState = nullptr;
  EXPECT_EQ(statusOf(e, "elevation"), DoctorStatus::Warn);
}

TEST(WindowsDoctor, EtwSessions) {
  WindowsDoctorEnv e = goodEnv();
  e.sessions = [] {
    WindowsSessionList s;
    s.ok = true;
    s.names = {"EventLog-System", "NT Kernel Logger"};
    return s;
  };
  DoctorCheck c = find(runWindowsDoctorChecks(e), "etw_sessions");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.detail.find("NT Kernel Logger"), std::string::npos);
  EXPECT_NE(c.hint.find("logman stop"), std::string::npos);

  e.sessions = [] {
    WindowsSessionList s;
    s.ok = true;
    s.names = {"Tachyon 4242 20261108", "Other"};
    return s;
  };
  c = find(runWindowsDoctorChecks(e), "etw_sessions");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.detail.find("Tachyon 4242 20261108"), std::string::npos);
  EXPECT_NE(c.hint.find("stale"), std::string::npos);

  e.sessions = [] {
    WindowsSessionList s;
    s.ok = true;
    for (int i = 0; i < 64; ++i) s.names.push_back("S" + std::to_string(i));
    return s;
  };
  EXPECT_EQ(statusOf(e, "etw_sessions"), DoctorStatus::Fail);
  e.sessions = [] {
    WindowsSessionList s;
    s.ok = true;
    for (int i = 0; i < 58; ++i) s.names.push_back("S" + std::to_string(i));
    return s;
  };
  EXPECT_EQ(statusOf(e, "etw_sessions"), DoctorStatus::Warn);

  e.sessions = [] {
    WindowsSessionList s;
    s.accessDenied = true;
    s.error = "QueryAllTraces failed with Windows error 5";
    return s;
  };
  c = find(runWindowsDoctorChecks(e), "etw_sessions");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.hint.find("elevated"), std::string::npos);

  e.sessions = nullptr;
  EXPECT_EQ(statusOf(e, "etw_sessions"), DoctorStatus::Warn);
}

TEST(WindowsDoctor, AgentPipe) {
  WindowsDoctorEnv e = goodEnv();
  std::string asked;
  e.probePipe = [&](const std::string& n) {
    asked = n;
    return WindowsPipeProbe::NotFound;
  };
  DoctorCheck c = find(runWindowsDoctorChecks(e), "agent_pipe");
  EXPECT_EQ(asked, e.pipeName);
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.hint.find("tachyon-agent --backend etw"), std::string::npos);

  e.probePipe = [](const std::string&) { return WindowsPipeProbe::Busy; };
  EXPECT_EQ(statusOf(e, "agent_pipe"), DoctorStatus::Pass);
  e.probePipe = [](const std::string&) { return WindowsPipeProbe::AccessDenied; };
  EXPECT_EQ(statusOf(e, "agent_pipe"), DoctorStatus::Fail);
  e.probePipe = [](const std::string&) { return WindowsPipeProbe::Error; };
  EXPECT_EQ(statusOf(e, "agent_pipe"), DoctorStatus::Warn);
  e.probePipe = nullptr;
  EXPECT_EQ(statusOf(e, "agent_pipe"), DoctorStatus::Warn);
}

TEST(WindowsDoctor, AgentBinaryAndSymbolCache) {
  WindowsDoctorEnv e = goodEnv();
  e.agentFound = false;
  EXPECT_EQ(statusOf(e, "agent_binary"), DoctorStatus::Warn);

  e = goodEnv();
  e.cacheDirWritable = [](const fs::path&, std::string& why) {
    why = "access denied";
    return false;
  };
  const DoctorCheck c = find(runWindowsDoctorChecks(e), "symbol_cache");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.detail.find("access denied"), std::string::npos);
  e.symbolCacheDir.clear();
  EXPECT_EQ(statusOf(e, "symbol_cache"), DoctorStatus::Warn);
}

TEST(WindowsDoctor, NtSymbolPath) {
  WindowsDoctorEnv e = goodEnv();
  EXPECT_EQ(statusOf(e, "nt_symbol_path"), DoctorStatus::Pass);  // not set
  e.ntSymbolPath = R"(srv*C:\symcache*https://msdl.microsoft.com/download/symbols;D:\mysyms)";
  EXPECT_EQ(statusOf(e, "nt_symbol_path"), DoctorStatus::Pass);
  e.ntSymbolPath = R"(symsrv*symsrv.dll*C:\symcache*https://example.com/syms)";
  EXPECT_EQ(statusOf(e, "nt_symbol_path"), DoctorStatus::Pass);

  e.ntSymbolPath = "https://msdl.microsoft.com/download/symbols";
  DoctorCheck c = find(runWindowsDoctorChecks(e), "nt_symbol_path");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.detail.find("bare URL"), std::string::npos);

  e.ntSymbolPath = R"(srv*C:\symcache*http://example.com/syms)";
  c = find(runWindowsDoctorChecks(e), "nt_symbol_path");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.detail.find("plain http"), std::string::npos);

  e.ntSymbolPath = "srv*";
  EXPECT_EQ(statusOf(e, "nt_symbol_path"), DoctorStatus::Warn);
  e.ntSymbolPath = R"(srv*C:\a**https://x)";
  c = find(runWindowsDoctorChecks(e), "nt_symbol_path");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.detail.find("empty store"), std::string::npos);

  e.ntSymbolPath = R"(D:\gone)";
  e.dirExists = [](const fs::path&) { return false; };
  c = find(runWindowsDoctorChecks(e), "nt_symbol_path");
  EXPECT_EQ(c.status, DoctorStatus::Warn);
  EXPECT_NE(c.detail.find("does not exist"), std::string::npos);
  EXPECT_FALSE(c.hint.empty());
}

TEST(WindowsDoctor, DiaAvailability) {
  WindowsDoctorEnv e = goodEnv();
  e.diaAvailable = [](std::string& why) {
    why = "cannot load msdia140.dll; tried: x";
    return false;
  };
  const DoctorCheck c = find(runWindowsDoctorChecks(e), "dia");
  EXPECT_EQ(c.status, DoctorStatus::Fail);
  EXPECT_NE(c.detail.find("msdia140"), std::string::npos);
  EXPECT_NE(c.hint.find("next to"), std::string::npos);
  e.diaAvailable = nullptr;
  EXPECT_EQ(statusOf(e, "dia"), DoctorStatus::Warn);
}

TEST(WindowsDoctor, ExitCodeFollowsFailNotWarnAndOutputMatchesTheLinuxFormats) {
  WindowsDoctorEnv e = goodEnv();
  std::ostringstream ok;
  EXPECT_EQ(runDoctorWithEnv(e, false, ok), 0);
  EXPECT_NE(ok.str().find("Windows capture prerequisites (sampling 8000 Hz)"), std::string::npos) << ok.str();
  EXPECT_NE(ok.str().find("[PASS] Elevated process"), std::string::npos);
  EXPECT_NE(ok.str().find("this machine is ready"), std::string::npos);

  e.probePipe = [](const std::string&) { return WindowsPipeProbe::NotFound; };  // WARN
  std::ostringstream warn;
  EXPECT_EQ(runDoctorWithEnv(e, false, warn), 0);
  EXPECT_NE(warn.str().find("[WARN]"), std::string::npos);
  EXPECT_NE(warn.str().find("       fix: "), std::string::npos);

  e.osBuild = 10240;  // FAIL
  std::ostringstream fail;
  EXPECT_EQ(runDoctorWithEnv(e, false, fail), 1);
  EXPECT_NE(fail.str().find("[FAIL]"), std::string::npos);

  std::ostringstream json;
  EXPECT_EQ(runDoctorWithEnv(e, true, json), 1);
  const nlohmann::json j = nlohmann::json::parse(json.str());
  EXPECT_EQ(j["schema"], 1);
  EXPECT_EQ(j["platform"], "windows");
  EXPECT_EQ(j["ok"], false);
  EXPECT_EQ(j["exit_code"], 1);
  EXPECT_EQ(j["requested_hz"], 8000);
  EXPECT_EQ(j["summary"]["fail"], 1);
  EXPECT_EQ(j["summary"]["warn"], 1);
  ASSERT_EQ(j["checks"].size(), 9u);
  for (const auto& c : j["checks"]) {
    EXPECT_TRUE(c.contains("id") && c.contains("title") && c.contains("status") && c.contains("detail") &&
                c.contains("hint"));
  }
}

#if defined(_WIN32)
// Smoke run of the real calls (needs Windows verification beyond CI): `doctor --json` emits one valid JSON
// document whose exit code and verdict agree. Whether the checks pass depends on the runner.
TEST(WindowsDoctor, RealMachineSmoke) {
  std::ostringstream out, err;
  const int code =
      cli::run({"doctor", "--json", "--pipe", R"(\\.\pipe\tachyon-doctor-smoke-nobody)"}, out, err);
  ASSERT_TRUE(code == 0 || code == 1) << code << " " << err.str();
  const nlohmann::json j = nlohmann::json::parse(out.str(), nullptr, false);
  ASSERT_TRUE(j.is_object()) << out.str();
  EXPECT_EQ(j["schema"], 1);
  EXPECT_EQ(j["platform"], "windows");
  EXPECT_EQ(j["exit_code"], code);
  EXPECT_EQ(j["ok"], code == 0);
  EXPECT_EQ(j["checks"].size(), 9u);
  const DoctorReport r = runWindowsDoctorChecks(makeSystemWindowsDoctorEnv(8000, R"(\\.\pipe\none)", ""));
  EXPECT_EQ(find(r, "windows_version").status, DoctorStatus::Pass);  // windows-2022 and newer
  EXPECT_EQ(find(r, "agent_pipe").status, DoctorStatus::Warn);       // nobody listens on that pipe
}
#else
// On other platforms the real environment has no callbacks: every check reports "not checked", never crashes.
TEST(WindowsDoctor, SystemEnvWithoutCallbacksDoesNotCrash) {
  const DoctorReport r = runWindowsDoctorChecks(makeSystemWindowsDoctorEnv(8000, "pipe", ""));
  EXPECT_EQ(r.checks.size(), 9u);
  EXPECT_EQ(r.fail, 0);
}
#endif

}  // namespace
}  // namespace tachyon::cli
