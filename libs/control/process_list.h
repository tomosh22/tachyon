// Process enumeration shared by the agent (`processes` command, WP-61) and the viewer's Attach dialog. Linux
// reads /proc; Windows uses Toolhelp32.
#pragma once

#include "control/protocol.h"
#include "core/result.h"

#include <filesystem>
#include <vector>

namespace tachyon::control {

// Thread-safe (reads the OS; call it off latency-sensitive threads). Lists the processes visible to this
// user, sorted by pid. Kernel threads (empty command line on Linux) are left out. `procRoot` is "/proc"
// except in tests; ignored on Windows. The command line is not truncated here.
Result<std::vector<ProcessEntry>> listProcesses(const std::filesystem::path& procRoot = "/proc");

}  // namespace tachyon::control
