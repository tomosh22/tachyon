// Conversion-time discovery of followed child processes (WP-13).
#pragma once

#include "capture/windows_etw/process_tree.h"
#include "core/result.h"

#include <filesystem>

namespace tachyon::capture::etw {

// Not thread-safe per tracker. Windows: runs ProcessTrace over `etl` and feeds the Process Start / End events
// to `tracker` in trace order (an extra pass that decodes only process events). Elsewhere: Unsupported.
Result<void> scanEtlProcessEvents(const std::filesystem::path& etl, ProcessTreeTracker& tracker);

}  // namespace tachyon::capture::etw
