// `tachyon-cli doctor` (WP-73, Windows WP-69): checks whether this machine can run a Tachyon capture and says
// how to fix what cannot. Linux checks the perf_event_open prerequisites; Windows checks elevation,
// SeSystemProfilePrivilege, the ETW sessions, the agent pipe and the symbol setup; other platforms print a
// short note and succeed.
//
// The Linux checks read everything through a DoctorEnv (proc and sys roots, the agent binary's file
// capabilities, the trial perf_event_open calls), the Windows checks through a WindowsDoctorEnv of callbacks,
// so the unit tests drive both against fake environments on any platform.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace tachyon::cli {

enum class DoctorStatus { Pass, Warn, Fail };

// Thread-safe: pure function. "PASS", "WARN" or "FAIL".
const char* doctorStatusName(DoctorStatus status);

struct DoctorCheck {
  std::string id;     // stable machine name, e.g. "perf_event_paranoid"
  std::string title;  // human title
  DoctorStatus status = DoctorStatus::Pass;
  std::string detail;  // what was found
  std::string hint;    // how to fix it; empty on PASS
};

struct DoctorReport {
  std::string platform = "linux";  // "linux" or "windows"
  std::vector<DoctorCheck> checks;
  int pass = 0, warn = 0, fail = 0;
  // Exit code of `doctor`: 0 when no check failed, 1 otherwise (WARN does not fail).
  int exitCode() const { return fail > 0 ? 1 : 0; }
};

// The agent binary as the filesystem describes it.
struct DoctorAgentFile {
  enum class Xattr { NotChecked, Absent, Present, Unsupported, Error };
  std::filesystem::path path;  // empty: the agent binary was not found
  bool found = false;
  Xattr xattr = Xattr::NotChecked;
  std::string xattrBlob;    // raw security.capability value when xattr == Present
  bool setuidRoot = false;  // owned by root with the set-user-ID bit
};

struct DoctorEnv {
  std::filesystem::path procRoot = "/proc";
  std::filesystem::path sysRoot = "/sys";
  uint32_t requestedHz = 8000;
  std::string socketAddress;  // the agent's control socket path (--socket or the default)
  DoctorAgentFile agent;
  std::vector<std::filesystem::path> buildIdDirs;  // e.g. /usr/lib/debug/.build-id
  std::string debuginfodUrls;                      // $DEBUGINFOD_URLS
  // Thread-safe by contract. True when the directory exists and the current user may create files in it.
  std::function<bool(const std::filesystem::path&)> isWritableDir;
  // Thread-safe by contract. Mode bits (permissions only) and owner uid of a directory; false when it cannot
  // be read. Null: the privacy check of the socket directory is skipped.
  std::function<bool(const std::filesystem::path&, uint32_t& mode, uint32_t& owner)> statDir;
  uint32_t uid = 0;  // the current user, for statDir's owner comparison
  // Trial perf_event_open of the cpu-clock event with callchains, closed again at once. Return 0 on success
  // or the errno. Null: the trial cannot be made on this platform (the check reports "unknown").
  std::function<int()> trialSystemWide;
  std::function<int()> trialPerProcess;
};

// ---- Windows (doctor_windows.cpp)
// ---------------------------------------------------------------------------

enum class WindowsPrivilege { Unknown, Absent, Present, Enabled };  // Present: held but disabled

struct WindowsTokenState {
  bool known = false;  // false: the token could not be examined (error says why)
  bool elevated = false;
  WindowsPrivilege profilePrivilege = WindowsPrivilege::Unknown;  // SeSystemProfilePrivilege
  std::string error;
};

struct WindowsSessionList {
  bool ok = false;
  bool accessDenied = false;  // when !ok: QueryAllTraces needs elevation
  std::string error;
  std::vector<std::string> names;  // every ETW logger name
};

enum class WindowsPipeProbe { Reachable, Busy, NotFound, AccessDenied, Error };

// Everything the Windows checks read. A null callback makes its check report WARN "not checked" (the
// callbacks are thread-safe by contract).
struct WindowsDoctorEnv {
  uint32_t requestedHz = 8000;
  std::string pipeName;                  // the agent's control pipe, \\.\pipe\tachyon-agent by default
  uint32_t osBuild = 0;                  // Windows build number; 0: unknown
  std::string ntSymbolPath;              // $_NT_SYMBOL_PATH
  std::filesystem::path symbolCacheDir;  // empty: unknown
  bool agentFound = false;
  std::filesystem::path agentPath;
  std::function<WindowsTokenState()> tokenState;
  std::function<WindowsSessionList()> sessions;
  std::function<WindowsPipeProbe(const std::string&)> probePipe;
  // Creates the directory when needed and a probe file in it; false (with the reason) when that fails.
  std::function<bool(const std::filesystem::path&, std::string& why)> cacheDirWritable;
  std::function<bool(const std::filesystem::path&)> dirExists;
  // True when msdia140.dll loads; the reason otherwise.
  std::function<bool(std::string& why)> diaAvailable;
};

// Thread-safe: calls the env's callbacks only. report.platform is "windows".
DoctorReport runWindowsDoctorChecks(const WindowsDoctorEnv& env);

// Thread-safe. The environment of this machine (real calls on Windows; elsewhere callbacks stay empty).
WindowsDoctorEnv makeSystemWindowsDoctorEnv(uint32_t requestedHz, const std::string& pipeName,
                                            const std::string& agentOverride);

// Thread-safe. Prints the report of `env` (same text and JSON formats as the Linux doctor, platform
// "windows") and returns its exit code.
int runDoctorWithEnv(const WindowsDoctorEnv& env, bool json, std::ostream& out);

// Thread-safe: pure functions. The same formats for any platform's report.
std::string formatDoctorText(const DoctorReport& report, uint32_t requestedHz);
std::string formatDoctorJson(const DoctorReport& report, uint32_t requestedHz);

// The decoded security.capability extended attribute (struct vfs_cap_data, revisions 1 to 3).
struct FileCapabilities {
  bool valid = false;
  int revision = 0;
  bool effective = false;  // the "e" flag
  uint64_t permitted = 0;
  uint64_t inheritable = 0;
  std::string error;  // when !valid
};

// Thread-safe: pure function.
FileCapabilities parseFileCapabilities(const std::string& blob);

// Thread-safe: reads the files below env.procRoot / env.sysRoot and calls the env's callbacks.
DoctorReport runDoctorChecks(const DoctorEnv& env);

// Thread-safe: pure functions.
std::string formatDoctorText(const DoctorReport& report, const DoctorEnv& env);
// Stable schema (schema 1): {"schema":1,"platform":"linux","ok":bool,"exit_code":n,"requested_hz":n,
// "summary":{"pass":n,"warn":n,"fail":n},"checks":[{"id","title","status","detail","hint"}]} ("platform" is
// report.platform).
std::string formatDoctorJson(const DoctorReport& report, const DoctorEnv& env);

// Thread-safe. The environment of this machine (Linux only; elsewhere an empty DoctorEnv). `agentOverride`
// is the --agent path; empty: tachyon-agent next to tachyon-cli, else on $PATH.
DoctorEnv makeSystemDoctorEnv(uint32_t requestedHz, const std::string& socketAddress,
                              const std::string& agentOverride);

// Thread-safe. Runs `doctor` with the options after the command word (--hz N, --agent PATH, plus the global
// --socket / --pipe / --timeout / --connect-timeout-ms / --json). Exit codes: 0 no check failed, 1 at least
// one FAIL, 3 usage error. On platforms other than Linux and Windows: prints a note and returns 0.
int runDoctor(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err);

// Thread-safe. Prints the report of `env` and returns its exit code. Used by runDoctor and the tests.
int runDoctorWithEnv(const DoctorEnv& env, bool json, std::ostream& out);

}  // namespace tachyon::cli
