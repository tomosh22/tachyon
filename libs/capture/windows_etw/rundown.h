// What already existed when a live capture started (WP-65w): processes, threads and loaded modules.
//
// The kernel logger writes its rundown (Process / Thread / Image DCEnd) when the session ENDS, which is
// useless for a live view; an attach or a system capture needs the state at the START to name threads and
// resolve module addresses. captureRundown() takes it from the Toolhelp snapshot API (the same facts, from
// the OS). Platform independent data; the capture itself is Windows only (elsewhere it returns Unsupported).
#pragma once

#include "core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tachyon::capture::etw {

struct RundownProcess {
  uint32_t pid = 0, ppid = 0;
  std::string name;  // image file name, UTF-8
};

struct RundownThread {
  uint32_t tid = 0, pid = 0;
  std::string name;  // SetThreadDescription name, may be empty
};

struct RundownModule {
  uint32_t pid = 0;
  uint64_t base = 0, size = 0;
  std::string path;  // DOS path, UTF-8
};

struct RundownSnapshot {
  std::vector<RundownProcess> processes;
  std::vector<RundownThread> threads;
  std::vector<RundownModule> modules;
};

// Thread-safe. Windows: the process list and the thread list of the whole machine, and the modules (and
// thread names) of `detailPids`, or of every process when `detailPids` is empty. Processes whose modules
// cannot be read (protected, not yet initialised) are skipped without an error. Elsewhere: Unsupported.
Result<RundownSnapshot> captureRundown(const std::vector<uint32_t>& detailPids);

}  // namespace tachyon::capture::etw
