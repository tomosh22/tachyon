// tachyon-cli: thin client for the agent control protocol (WP-10). WP-13 extends this.
//
// Usage: tachyon-cli [--socket PATH | --pipe NAME] [--json] [--timeout SECONDS] <command> [options]
//   status
//   start --mode launch|attach|system [--exe E] [--args A...] [--arg A]... [--cwd D] [--env K=V]...
//         [--pid N] [--hz N] [--follow-children] [--child-filter G] [--circular-mb N] [--out PATH]
//         [--defer-start] [--no-profile] [--no-cswitch] [--no-readythread] [--no-kernel-stacks] [--wait]
//   stop [--wait]
//   flush --name N --seconds S
//   cancel
// Exit codes: 0 ok, 1 protocol error response (or capture error event), 2 connection failure/timeout,
// 3 usage error.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace tachyon::cli {

constexpr int kExitOk = 0;
constexpr int kExitProtocolError = 1;
constexpr int kExitConnectionFailure = 2;
constexpr int kExitUsage = 3;

// Thread-safe (no shared state). args excludes argv[0].
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace tachyon::cli
