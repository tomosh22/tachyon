// Windows checks of `tachyon-cli doctor` (WP-69). The decision logic (runWindowsDoctorChecks) is platform
// neutral and reads everything through WindowsDoctorEnv callbacks, so the unit tests exercise it on Linux;
// makeSystemWindowsDoctorEnv fills the callbacks with the real calls on Windows (needs Windows verification
// beyond the windows CI job, which runs `tachyon-cli doctor --json` and checks that it parses).
#include "cli/doctor.h"

#include "capture/windows_etw/etw_session.h"
#include "core/platform.h"
#include "symbols/dia/dia_session.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#if TACHYON_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace tachyon::cli {

namespace fs = std::filesystem;

namespace {

// Windows 10 version 1703: the first build with EVENT_TRACE_SYSTEM_LOGGER_MODE, which the ETW backend uses so
// its session does not compete for the single "NT Kernel Logger".
constexpr uint32_t kMinSystemLoggerBuild = 15063;
constexpr size_t kMaxLoggers = 64;  // ETW supports at most 64 concurrent sessions
constexpr size_t kLoggerWarnAt = 56;

struct Ctx {
  const WindowsDoctorEnv& env;
  std::vector<DoctorCheck>& out;

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
};

bool iequals(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

bool startsWithNoCase(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool isUrl(const std::string& s) {
  return startsWithNoCase(s, "http://") || startsWithNoCase(s, "https://");
}

void checkVersion(Ctx& c) {
  const char* title = "Windows version (ETW system logger)";
  if (c.env.osBuild == 0) {
    c.add("windows_version", title, DoctorStatus::Warn, "the Windows build number could not be read",
          "Tachyon needs Windows 10 1703 (build 15063) or later");
    return;
  }
  const std::string detail = "build " + std::to_string(c.env.osBuild);
  if (c.env.osBuild < kMinSystemLoggerBuild)
    c.add("windows_version", title, DoctorStatus::Fail,
          detail + ": ETW system logger sessions need build " + std::to_string(kMinSystemLoggerBuild) +
              " or later",
          "update to Windows 10 version 1703 or later (Windows 11 is fine)");
  else
    c.add("windows_version", title, DoctorStatus::Pass, detail);
}

void checkToken(Ctx& c) {
  const char* elevTitle = "Elevated process";
  const char* privTitle = "SeSystemProfilePrivilege";
  if (!c.env.tokenState) {
    c.add("elevation", elevTitle, DoctorStatus::Warn, "the process token could not be examined",
          "run the agent from an elevated prompt");
    c.add("sysprofile_privilege", privTitle, DoctorStatus::Warn, "not checked",
          "the agent needs SeSystemProfilePrivilege to start an ETW system logger");
    return;
  }
  const WindowsTokenState t = c.env.tokenState();
  if (!t.known) {
    const std::string why = t.error.empty() ? "unknown error" : t.error;
    c.add("elevation", elevTitle, DoctorStatus::Warn, "the process token could not be examined: " + why,
          "run the agent from an elevated prompt");
    c.add("sysprofile_privilege", privTitle, DoctorStatus::Warn, "not checked",
          "the agent needs SeSystemProfilePrivilege to start an ETW system logger");
    return;
  }
  if (t.elevated)
    c.add("elevation", elevTitle, DoctorStatus::Pass, "this process runs elevated");
  else
    c.add("elevation", elevTitle, DoctorStatus::Warn,
          "this process is not elevated; the capture agent must be (this check does not apply to a "
          "`tachyon-cli` that only talks to an elevated agent)",
          "start `tachyon-agent` from an elevated prompt (Run as administrator), or use \"Start elevated\" "
          "in the viewer");
  switch (t.profilePrivilege) {
    case WindowsPrivilege::Enabled:
      c.add("sysprofile_privilege", privTitle, DoctorStatus::Pass, "held and enabled");
      break;
    case WindowsPrivilege::Present:
      c.add("sysprofile_privilege", privTitle, DoctorStatus::Pass,
            "held (disabled); the agent enables it when it starts a capture");
      break;
    case WindowsPrivilege::Absent:
      c.add(
          "sysprofile_privilege", privTitle, t.elevated ? DoctorStatus::Fail : DoctorStatus::Warn,
          t.elevated ? "this elevated token does not hold the privilege (removed by group policy?)"
                     : "this token does not hold the privilege (a non-elevated token never does)",
          t.elevated
              ? "grant \"Profile system performance\" to your account in secpol.msc > Local Policies > User "
                "Rights Assignment, then sign out and in"
              : "run the agent elevated (administrators hold the privilege), or grant \"Profile system "
                "performance\" to this account in secpol.msc");
      break;
    case WindowsPrivilege::Unknown:
      c.add("sysprofile_privilege", privTitle, DoctorStatus::Warn, "could not be determined",
            "run the agent from an elevated prompt");
      break;
  }
}

void checkSessions(Ctx& c) {
  const char* title = "ETW sessions (NT Kernel Logger, stale Tachyon sessions)";
  if (!c.env.sessions) {
    c.add("etw_sessions", title, DoctorStatus::Warn, "ETW sessions cannot be enumerated here");
    return;
  }
  const WindowsSessionList s = c.env.sessions();
  if (!s.ok) {
    if (s.accessDenied)
      c.add("etw_sessions", title, DoctorStatus::Warn,
            "cannot enumerate ETW sessions without elevation: " + s.error,
            "run `tachyon-cli doctor` from an elevated prompt to see the running sessions");
    else
      c.add("etw_sessions", title, DoctorStatus::Warn, "QueryAllTraces failed: " + s.error,
            "check that the Windows Event Log / ETW services are running");
    return;
  }
  std::vector<std::string> tachyon;
  bool kernelLogger = false;
  for (const std::string& n : s.names) {
    if (iequals(n, "NT Kernel Logger")) kernelLogger = true;
    if (startsWithNoCase(n, "Tachyon ")) tachyon.push_back(n);
  }
  const std::string count =
      std::to_string(s.names.size()) + " of " + std::to_string(kMaxLoggers) + " sessions";
  if (s.names.size() >= kMaxLoggers) {
    c.add("etw_sessions", title, DoctorStatus::Fail, count + " are in use: no free session slot",
          "stop unused sessions: `logman query -ets` lists them, `logman stop \"<name>\" -ets` stops one");
    return;
  }
  std::string detail = count + " in use";
  std::string hint;
  DoctorStatus status = DoctorStatus::Pass;
  if (!tachyon.empty()) {
    status = DoctorStatus::Warn;
    detail += "; " + std::to_string(tachyon.size()) + " Tachyon session(s) running (" + tachyon.front() +
              (tachyon.size() > 1 ? ", ..." : "") + ")";
    hint =
        "if no capture is in progress these are stale: starting the agent stops those of dead processes, "
        "or run `logman stop \"" +
        tachyon.front() + "\" -ets`";
  }
  if (kernelLogger) {
    status = DoctorStatus::Warn;
    detail += "; \"NT Kernel Logger\" is in use by another tool (WPA, PerfView, Superluminal, ...)";
    if (!hint.empty()) hint += "; ";
    hint +=
        "Tachyon uses its own system-logger session, so a capture still starts, but the other tool's "
        "kernel events and stack walking share the kernel's sampling profile; stop it for a clean capture "
        "(`logman stop \"NT Kernel Logger\" -ets`)";
  }
  if (s.names.size() >= kLoggerWarnAt && status == DoctorStatus::Pass) {
    status = DoctorStatus::Warn;
    hint = "stop unused sessions: `logman query -ets` lists them";
  }
  c.add("etw_sessions", title, status, detail, hint);
}

void checkPipe(Ctx& c) {
  const char* title = "Agent control pipe";
  if (!c.env.probePipe || c.env.pipeName.empty()) {
    c.add("agent_pipe", title, DoctorStatus::Warn, "the pipe cannot be probed here");
    return;
  }
  const std::string& name = c.env.pipeName;
  switch (c.env.probePipe(name)) {
    case WindowsPipeProbe::Reachable:
      c.add("agent_pipe", title, DoctorStatus::Pass, name + " (an agent is listening)");
      break;
    case WindowsPipeProbe::Busy:
      c.add("agent_pipe", title, DoctorStatus::Pass, name + " (an agent is listening; all instances busy)");
      break;
    case WindowsPipeProbe::NotFound:
      c.add("agent_pipe", title, DoctorStatus::Warn, "no agent is listening on " + name,
            "start it from an elevated prompt: `tachyon-agent --backend etw --pipe " + name +
                "` (or use another pipe with --pipe)");
      break;
    case WindowsPipeProbe::AccessDenied:
      c.add("agent_pipe", title, DoctorStatus::Fail, "access to " + name + " was denied",
            "the pipe belongs to an agent started by another user or with a restrictive DACL; stop it or use "
            "a different --pipe");
      break;
    case WindowsPipeProbe::Error:
      c.add("agent_pipe", title, DoctorStatus::Warn, "probing " + name + " failed",
            "check that the pipe name is of the form \\\\.\\pipe\\<name>");
      break;
  }
}

void checkAgentBinary(Ctx& c) {
  const char* title = "Agent executable";
  if (c.env.agentFound)
    c.add("agent_binary", title, DoctorStatus::Pass, c.env.agentPath.string());
  else
    c.add(
        "agent_binary", title, DoctorStatus::Warn,
        "tachyon-agent.exe was not found next to tachyon-cli.exe or on PATH",
        "pass --agent PATH, or install the agent beside the CLI (an agent that is already running is fine)");
}

void checkSymbolCache(Ctx& c) {
  const char* title = "Symbol cache directory";
  if (c.env.symbolCacheDir.empty()) {
    c.add("symbol_cache", title, DoctorStatus::Warn, "no cache directory (%LOCALAPPDATA% is not set)",
          "set LOCALAPPDATA, or pass a cache directory to the viewer's symbol settings");
    return;
  }
  if (!c.env.cacheDirWritable) {
    c.add("symbol_cache", title, DoctorStatus::Warn, c.env.symbolCacheDir.string() + " (not checked)");
    return;
  }
  std::string why;
  if (c.env.cacheDirWritable(c.env.symbolCacheDir, why))
    c.add("symbol_cache", title, DoctorStatus::Pass, c.env.symbolCacheDir.string() + " (writable)");
  else
    c.add("symbol_cache", title, DoctorStatus::Warn,
          c.env.symbolCacheDir.string() + " is not writable" + (why.empty() ? "" : ": " + why),
          "symbols downloaded from a server cannot be cached; fix the directory's permissions or free disk "
          "space");
}

// Splits _NT_SYMBOL_PATH and describes what is suspicious about each element.
std::vector<std::string> ntSymbolPathProblems(const WindowsDoctorEnv& env) {
  std::vector<std::string> problems;
  std::istringstream in(env.ntSymbolPath);
  std::string item;
  while (std::getline(in, item, ';')) {
    if (item.empty()) continue;
    const bool srv = startsWithNoCase(item, "srv*") || startsWithNoCase(item, "symsrv*");
    const bool cacheOnly = startsWithNoCase(item, "cache*");
    if (!srv && !cacheOnly) {
      if (isUrl(item)) {
        problems.push_back("'" + item + "' is a bare URL; write srv*<cache>*" + item);
      } else if (env.dirExists && !env.dirExists(fs::path(item))) {
        problems.push_back("directory '" + item + "' does not exist");
      }
      continue;
    }
    // srv*a*b*c / symsrv*symsrv.dll*a*b : the elements after the prefix (and the DLL name) are stores.
    std::vector<std::string> parts;
    std::istringstream ps(item);
    std::string p;
    while (std::getline(ps, p, '*')) parts.push_back(p);
    size_t first = 1;
    if (startsWithNoCase(item, "symsrv*")) first = 2;  // symsrv*symsrv.dll*...
    if (parts.size() <= first || std::all_of(parts.begin() + static_cast<std::ptrdiff_t>(first), parts.end(),
                                             [](const std::string& s) { return s.empty(); })) {
      problems.push_back("'" + item + "' names no symbol store");
      continue;
    }
    for (size_t i = first; i < parts.size(); ++i) {
      const std::string& store = parts[i];
      if (store.empty()) {
        problems.push_back("'" + item + "' has an empty store (a doubled '*')");
      } else if (startsWithNoCase(store, "http://")) {
        problems.push_back("'" + store +
                           "' uses plain http; the Microsoft server and most others support https");
      } else if (!isUrl(store) && env.dirExists && !env.dirExists(fs::path(store)) &&
                 store.find('\\') != std::string::npos) {
        problems.push_back("cache directory '" + store + "' does not exist (it is created on first use)");
      }
    }
  }
  return problems;
}

void checkNtSymbolPath(Ctx& c) {
  const char* title = "_NT_SYMBOL_PATH";
  if (c.env.ntSymbolPath.empty()) {
    c.add("nt_symbol_path", title, DoctorStatus::Pass,
          "not set (symbol servers come from the viewer's Settings or --symbol-server)");
    return;
  }
  const std::vector<std::string> problems = ntSymbolPathProblems(c.env);
  if (problems.empty()) {
    c.add("nt_symbol_path", title, DoctorStatus::Pass, c.env.ntSymbolPath);
    return;
  }
  std::string detail = c.env.ntSymbolPath + ": ";
  for (size_t i = 0; i < problems.size(); ++i) detail += (i == 0 ? "" : "; ") + problems[i];
  c.add("nt_symbol_path", title, DoctorStatus::Warn, detail,
        "the form is srv*<local cache>*https://msdl.microsoft.com/download/symbols; separate elements with "
        "';'");
}

void checkDia(Ctx& c) {
  const char* title = "DIA SDK (msdia140.dll)";
  if (!c.env.diaAvailable) {
    c.add("dia", title, DoctorStatus::Warn, "not checked");
    return;
  }
  std::string why;
  if (c.env.diaAvailable(why))
    c.add("dia", title, DoctorStatus::Pass, "msdia140.dll loads");
  else
    c.add(
        "dia", title, DoctorStatus::Fail, why.empty() ? "msdia140.dll cannot be loaded" : why,
        "copy msdia140.dll (from the Visual Studio DIA SDK, `DIA SDK\\bin\\amd64`) next to tachyon-cli.exe / "
        "tachyon-viewer.exe; without it PDB symbols do not resolve (JSON symbol files still do)");
}

}  // namespace

DoctorReport runWindowsDoctorChecks(const WindowsDoctorEnv& env) {
  DoctorReport report;
  report.platform = "windows";
  Ctx c{env, report.checks};
  checkVersion(c);
  checkToken(c);
  checkSessions(c);
  checkPipe(c);
  checkAgentBinary(c);
  checkSymbolCache(c);
  checkNtSymbolPath(c);
  checkDia(c);
  for (const DoctorCheck& chk : report.checks) {
    switch (chk.status) {
      case DoctorStatus::Pass: ++report.pass; break;
      case DoctorStatus::Warn: ++report.warn; break;
      case DoctorStatus::Fail: ++report.fail; break;
    }
  }
  return report;
}

#if TACHYON_PLATFORM_WINDOWS

namespace {

std::string getEnv(const char* name) {
  char* buffer = nullptr;
  size_t length = 0;
  std::string result;
  if (_dupenv_s(&buffer, &length, name) == 0 && buffer != nullptr) result = buffer;
  std::free(buffer);
  return result;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

uint32_t readBuildNumber() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return 0;
  const auto fn =
      reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
  if (fn == nullptr) return 0;
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (fn(&info) != 0) return 0;
  return static_cast<uint32_t>(info.dwBuildNumber);
}

WindowsTokenState readTokenState() {
  WindowsTokenState t;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    t.error = "OpenProcessToken failed with Windows error " + std::to_string(GetLastError());
    return t;
  }
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
    t.known = true;
    t.elevated = elevation.TokenIsElevated != 0;
  } else {
    t.error =
        "GetTokenInformation(TokenElevation) failed with Windows error " + std::to_string(GetLastError());
  }
  LUID luid{};
  if (t.known && LookupPrivilegeValueW(nullptr, L"SeSystemProfilePrivilege", &luid)) {
    DWORD need = 0;
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &need);
    std::vector<char> buf(need);
    t.profilePrivilege = WindowsPrivilege::Absent;
    if (need != 0 && GetTokenInformation(token, TokenPrivileges, buf.data(), need, &need)) {
      const auto* privs = reinterpret_cast<const TOKEN_PRIVILEGES*>(buf.data());
      for (DWORD i = 0; i < privs->PrivilegeCount; ++i) {
        const LUID_AND_ATTRIBUTES& p = privs->Privileges[i];
        if (p.Luid.LowPart == luid.LowPart && p.Luid.HighPart == luid.HighPart) {
          t.profilePrivilege = (p.Attributes & SE_PRIVILEGE_ENABLED) != 0 ? WindowsPrivilege::Enabled
                                                                          : WindowsPrivilege::Present;
          break;
        }
      }
    } else {
      t.profilePrivilege = WindowsPrivilege::Unknown;
    }
  }
  CloseHandle(token);
  return t;
}

}  // namespace

WindowsDoctorEnv makeSystemWindowsDoctorEnv(uint32_t requestedHz, const std::string& pipeName,
                                            const std::string& agentOverride) {
  WindowsDoctorEnv env;
  env.requestedHz = requestedHz;
  env.pipeName = pipeName;
  env.osBuild = readBuildNumber();
  env.ntSymbolPath = getEnv("_NT_SYMBOL_PATH");
  if (const std::string local = getEnv("LOCALAPPDATA"); !local.empty())
    env.symbolCacheDir = fs::path(local) / "Tachyon" / "symbols";

  std::error_code ec;
  fs::path agent;
  if (!agentOverride.empty()) {
    agent = agentOverride;
  } else {
    wchar_t exe[4096] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n > 0 && n < std::size(exe) &&
        fs::is_regular_file(fs::path(exe).parent_path() / "tachyon-agent.exe", ec))
      agent = fs::path(exe).parent_path() / "tachyon-agent.exe";
    if (agent.empty()) {
      wchar_t found[4096] = {};
      if (SearchPathW(nullptr, L"tachyon-agent.exe", nullptr, static_cast<DWORD>(std::size(found)), found,
                      nullptr) > 0)
        agent = found;
    }
  }
  if (!agent.empty() && fs::is_regular_file(agent, ec)) {
    env.agentFound = true;
    env.agentPath = agent;
  }

  env.tokenState = [] { return readTokenState(); };
  env.sessions = [] {
    WindowsSessionList out;
    auto r = capture::etw::EtwSession::listSessions();
    if (r.ok()) {
      out.ok = true;
      out.names = std::move(*r);
    } else {
      out.accessDenied = r.error().code == ErrorCode::NoPrivilege;
      out.error = r.error().toString();
    }
    return out;
  };
  env.probePipe = [](const std::string& name) {
    // WaitNamedPipe neither connects nor consumes a pipe instance: it only asks whether one is available.
    if (WaitNamedPipeW(widen(name).c_str(), 200)) return WindowsPipeProbe::Reachable;
    switch (GetLastError()) {
      case ERROR_SEM_TIMEOUT: return WindowsPipeProbe::Busy;
      case ERROR_FILE_NOT_FOUND:
      case ERROR_PATH_NOT_FOUND: return WindowsPipeProbe::NotFound;
      case ERROR_ACCESS_DENIED: return WindowsPipeProbe::AccessDenied;
      default: return WindowsPipeProbe::Error;
    }
  };
  env.cacheDirWritable = [](const fs::path& dir, std::string& why) {
    std::error_code e;
    fs::create_directories(dir, e);
    if (e) {
      why = e.message();
      return false;
    }
    const fs::path probe = dir / (".tachyon-doctor-" + std::to_string(GetCurrentProcessId()));
    {
      std::ofstream f(probe, std::ios::binary | std::ios::trunc);
      if (!f) {
        why = "cannot create a file in it";
        return false;
      }
      f << "x";
    }
    fs::remove(probe, e);
    return true;
  };
  env.dirExists = [](const fs::path& p) {
    std::error_code e;
    return fs::is_directory(p, e);
  };
#if defined(TACHYON_HAVE_DIA)
  env.diaAvailable = [](std::string& why) {
    std::string err;
    const auto opener = symbols::makeComDiaOpener(&err);
    if (opener) return true;
    why = err;
    return false;
  };
#endif  // a build without DIA leaves the check "not checked" (WARN) instead of failing it
  return env;
}

#else

WindowsDoctorEnv makeSystemWindowsDoctorEnv(uint32_t requestedHz, const std::string& pipeName,
                                            const std::string&) {
  WindowsDoctorEnv env;  // callbacks stay empty: every check reports "not checked"
  env.requestedHz = requestedHz;
  env.pipeName = pipeName;
  return env;
}

#endif

}  // namespace tachyon::cli
