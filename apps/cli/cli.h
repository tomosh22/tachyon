// tachyon-cli: client for the agent control protocol (WP-10, extended by WP-13).
//
// Usage: tachyon-cli [--socket PATH | --pipe NAME | --agent ADDRESS] [--token-file FILE] [--json]
//                    [--timeout SECONDS] <command> [options]
//
//   ADDRESS: a socket path, a pipe name, or tcp://HOST:PORT for a remote agent (WP-61). A tcp:// agent
//   needs the token: --token-file FILE or $TACHYON_AGENT_TOKEN_FILE (a file whose first line is the token;
//   on POSIX it must not be readable by group or others). The channel is not encrypted.
//
//   status
//   start   <target> [start options] [--no-start] [--wait]
//   stop    [--wait]
//   resume                          starts the recording of a launch parked with `start --no-start`
//   flush   --name N --seconds S [--reason R]
//             circular (flight recorder) captures only: writes the last S seconds to <out dir>/<N>.tachyon (N
//             is made a safe file name and gets a -N suffix when it exists). `capture` prints the agent's
//             `flushed` events (also those caused by Tachyon_RequestFlush) while it waits.
//   cancel
//   merge   TRACE.tachyon INST.tachyon-inst [--out PATH] [--clip] [--no-siblings] [--force]
//             offline (no agent): merges the instrumentation stream of an SDK sidecar file, plus its numbered
//             siblings <stem>.<n><ext> in numeric order, into the trace. Without --out the trace is replaced
//             through a temporary file and an atomic rename; with --out the input is never touched. --clip
//             drops records outside the trace window (default: keep them and extend the window). Unbalanced
//             scopes, dropped records and similar are printed as warnings on stderr (the exit code stays 0).
//             A sidecar whose content was merged into the trace before is refused unless --force is given.
//   export  TRACE.tachyon --format speedscope|chrome --out PATH [--threads TID,...] [--from-ms X] [--to-ms Y]
//             [--symbols PATH]... [--symbol-server URL]...
//             offline (no agent): exports a trace for speedscope (one sampled profile per thread) or for
//             Perfetto / chrome://tracing (scopes, frame marks, markers, plots, per-CPU running slices).
//             --threads takes OS thread ids; --from-ms / --to-ms are milliseconds from the trace start.
//             --symbols (repeatable) takes a symbol directory, file, or tracegen *.symbols.json override;
//             --symbol-server (repeatable) a symbol store URL. Symbols matter for speedscope only. --out -
//             writes to stdout (and prints no summary). With --json the summary is one JSON object on stdout.
//   doctor  [--hz N] [--agent PATH]
//             offline (no agent): Linux: checks kernel version, perf_event_paranoid and the other perf
//             sysctls, capabilities of this process and of the agent binary, tracefs and scheduler
//             tracepoints, trial perf_event_open calls, rlimits, ptrace scope, the agent socket directory and
//             symbol sources. Each item prints PASS, WARN or FAIL with a fix hint. --hz (default 8000) is
//             compared with kernel.perf_event_max_sample_rate; --agent names the agent binary (default: next
//             to tachyon-cli, then $PATH); --socket / --pipe name the agent socket whose directory is
//             checked. With --json stdout is one object
//             {"schema":1,"platform","ok","exit_code","requested_hz","summary",
//             "checks":[{"id","title","status","detail","hint"}]}. Exit 0 when nothing FAILed (WARN does not
//             fail), 1 when any item FAILed, 3 on a usage error. Windows: prints a note, exits 0.
//   capture <target> [start options] [--duration S] [--fetch [--fetch-dir DIR]]
//             start + wait for --duration seconds or for the target to exit + stop + wait for capture_done.
//             --fetch then copies the finished trace to DIR (default: the current directory) over the
//             connection, as `fetch` does; use it with a tcp:// agent, whose files are not on this machine.
//   fetch   PATH [--out FILE]
//             copies a trace the agent produced (the path capture_done / flushed reported; any other path is
//             refused) to FILE (default: ./<file name>). The data goes to FILE.part first; an interrupted
//             transfer resumes from the size of FILE.part on the next call, and the SHA-256 the agent
//             computed must match before FILE appears. With --json stdout is
//             {"ok":true,"fetched":{"remote","path","size","sha256","resumed_from","restarted"}}.
//   processes
//             lists the processes visible to the agent (pid, name, user, command line; tab separated, or a
//             JSON array with --json). For remote agents, which have processes of their own.
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
//   1  the agent answered with an error, or the capture ended with an error event; for the offline commands
//      (merge, export): a missing or unreadable input, or an output that cannot be written
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
