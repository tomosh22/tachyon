// `tachyon-cli export` (WP-63): offline export of a .tachyon trace to speedscope or Chrome trace JSON.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace tachyon::cli {

// Thread-safe (no shared state). `args` are the words after "export"; `json` is the global --json flag.
// Exit codes: see cli.h (0 ok, 1 trace/symbol/output error, 3 usage).
int runExport(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err);

}  // namespace tachyon::cli
