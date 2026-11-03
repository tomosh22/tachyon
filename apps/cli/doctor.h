// `tachyon-cli doctor` (WP-73): checks whether this machine can run a Tachyon capture and says how to fix
// what cannot. Linux checks the perf_event_open prerequisites; other platforms print a short note and
// succeed.
//
// The checks read everything through a DoctorEnv (proc and sys roots, the agent binary's file capabilities,
// the trial perf_event_open calls), so the unit tests drive them against fake directory trees.
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
// "summary":{"pass":n,"warn":n,"fail":n},"checks":[{"id","title","status","detail","hint"}]}.
std::string formatDoctorJson(const DoctorReport& report, const DoctorEnv& env);

// Thread-safe. The environment of this machine (Linux only; elsewhere an empty DoctorEnv). `agentOverride`
// is the --agent path; empty: tachyon-agent next to tachyon-cli, else on $PATH.
DoctorEnv makeSystemDoctorEnv(uint32_t requestedHz, const std::string& socketAddress,
                              const std::string& agentOverride);

// Thread-safe. Runs `doctor` with the options after the command word (--hz N, --agent PATH, plus the global
// --socket / --pipe / --timeout / --connect-timeout-ms / --json). Exit codes: 0 no check failed, 1 at least
// one FAIL, 3 usage error. On platforms other than Linux: prints a note and returns 0.
int runDoctor(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err);

// Thread-safe. Prints the report of `env` and returns its exit code. Used by runDoctor and the tests.
int runDoctorWithEnv(const DoctorEnv& env, bool json, std::ostream& out);

}  // namespace tachyon::cli
