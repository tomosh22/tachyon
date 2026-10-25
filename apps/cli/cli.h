// tachyon-cli: client for the agent control protocol (WP-10, extended by WP-13).
//
// Usage: tachyon-cli [--socket PATH | --pipe NAME] [--json] [--timeout SECONDS] <command> [options]
//
//   status
//   start   <target> [start options] [--no-start] [--wait]
//   stop    [--wait]
//   resume                          starts the recording of a launch parked with `start --no-start`
//   flush   --name N --seconds S
//   cancel
//   capture <target> [start options] [--duration S]
//             start + wait for --duration seconds or for the target to exit + stop + wait for capture_done
//
//   <target>: --launch EXE | --attach PID | --system | --mode launch|attach|system [--exe E] [--pid N]
//   start options: --args "A B \"C D\""  (one string, split with the Windows command-line rules)
//                  --args A B C          (legacy: every following word up to the next --option)
//                  --arg A               (repeatable; the way to pass an argument that starts with "--")
//                  --cwd D  --env K=V (repeatable)  --hz N  --follow-children  --child-filter GLOB
//                  --circular-mb N  --out PATH  --no-start  --no-profile --no-cswitch --no-readythread
//                  --no-kernel-stacks   (--defer-start is a synonym of --no-start)
//
// Exit codes:
//   0  success
//   1  the agent answered with an error, or the capture ended with an error event
//   2  cannot connect to the agent, the connection broke, or a wait hit --timeout
//   3  usage error
//   4  the capture finished and the trace was written, but the session lost events (events_lost > 0)
// --json prints every response and event as one JSON line (the protocol's own encoding) and nothing else on
// stdout; diagnostics stay on stderr.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace tachyon::cli {

constexpr int kExitOk = 0;
constexpr int kExitProtocolError = 1;
constexpr int kExitConnectionFailure = 2;
constexpr int kExitUsage = 3;
constexpr int kExitEventsLost = 4;

// Thread-safe (no shared state). args excludes argv[0].
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace tachyon::cli
