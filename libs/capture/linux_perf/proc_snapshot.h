// The /proc rundown (WP-70): perf only reports a process's mappings and threads as they change, so what
// already exists when the capture starts is read from /proc and written to the spool as ordinary perf records
// (FORK, COMM, MMAP2) stamped with the session start time. The converter treats them exactly like live
// records.
#pragma once

#include "capture/linux_perf/perf_records.h"
#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::capture::perf {

// One line of /proc/<pid>/maps.
struct MapsLine {
  uint64_t start = 0, end = 0, offset = 0;
  bool read = false, write = false, exec = false, shared = false;
  uint32_t devMajor = 0, devMinor = 0;
  uint64_t inode = 0;
  std::string path;  // may be empty (anonymous), "[heap]", "[vdso]", or a file name with " (deleted)"
};

// Thread-safe: pure function. False for a line that does not follow the format.
bool parseMapsLine(std::string_view line, MapsLine& out);

struct ProcStat {
  char state = '?';
  uint32_t ppid = 0;
  std::string comm;
};

// Thread-safe: pure function. Parses /proc/<pid>/stat ("pid (comm) S ppid ..."; comm may contain spaces and
// parentheses, so the LAST ')' ends it). False when malformed.
bool parseProcStat(std::string_view text, ProcStat& out);

struct RundownOptions {
  std::filesystem::path procRoot = "/proc";
  // Empty: every process. Otherwise these pids, plus their descendants when `includeDescendants`.
  std::vector<uint32_t> rootPids;
  bool includeDescendants = false;
  uint64_t ts = 0;  // timestamp of the synthetic records (the session start)
  RecordLayout layout;
};

struct RundownResult {
  std::vector<uint8_t> records;  // back-to-back perf records, ready for the spool
  std::vector<uint32_t> pids;    // processes covered, ascending
  uint32_t threads = 0;
  uint32_t mappings = 0;
};

// Thread-safe. Reads procRoot; processes that vanish while being read are skipped. Records are ordered
// FORK, COMM... per process, then its MMAP2 records.
RundownResult buildRundown(const RundownOptions& options);

// Thread-safe. The threads (tids) of a process listed in procRoot/<pid>/task, ascending; empty when the
// process does not exist.
std::vector<uint32_t> listThreads(const std::filesystem::path& procRoot, uint32_t pid);

// Thread-safe. True while procRoot/<pid>/stat exists and the process is not dead (a zombie leader whose
// other threads still run counts as alive).
bool processAlive(const std::filesystem::path& procRoot, uint32_t pid);

}  // namespace tachyon::capture::perf
