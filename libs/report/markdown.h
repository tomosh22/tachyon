// Markdown summary of a Report for a pull-request comment.
#pragma once

#include "report/baseline.h"
#include "report/report.h"

#include <string>

namespace tachyon::report {

// Thread-safe: pure function. Deterministic and free of absolute paths and machine-specific data (lock
// addresses are left out). `diff` is the comparison with a baseline, or nullptr for a plain report.
std::string renderMarkdown(const Report& report, const Diff* diff);

}  // namespace tachyon::report
