# tachyon-cli

Command-line client for `tachyon-agent` (the control protocol of `libs/control/protocol.h`) plus offline tools.
There is no `--help` option: running `tachyon-cli` with no command or an unknown one prints the usage text on
stderr and exits 3 (the full option list is also the header comment of `apps/cli/cli.h`).

```
tachyon-cli [--socket PATH | --pipe NAME | --agent ADDRESS] [--token-file FILE] [--json] [--timeout SECONDS]
            <command> [options]
```

| command | what it does | needs the agent |
|---|---|---|
| `status`, `start`, `stop`, `resume`, `flush`, `cancel`, `capture` | drive a capture | yes |
| `fetch` | copy a trace the agent produced to this machine (resumes, verifies) | yes |
| `processes` | list the processes visible to the agent (a remote agent has its own) | yes |
| `merge` | add an SDK sidecar file to a trace | no |
| `export` | write a speedscope or Chrome trace file | no |
| `doctor` | check whether this machine can capture | no |

## Commands and options

Global options (before the command): `--socket PATH`, `--pipe NAME` (synonyms) or `--agent ADDRESS` choose the
agent (default: `$TACHYON_AGENT_PIPE`, else the platform default, see [apps/agent/README.md](../agent/README.md));
`--token-file FILE` for a `tcp://` agent; `--json`: every response and event becomes one JSON line on stdout (the
protocol's own encoding) and diagnostics stay on stderr; `--timeout SECONDS` (default 600) bounds every wait
(`capture` and `live` without `--duration`, `--wait`); `--connect-timeout-ms N` retries the connection for N
milliseconds (default: one attempt).

| Command | Meaning |
|---|---|
| `status` | state of the agent (`idle`, `starting`, `capturing`, `converting`), progress, backend, whether it is elevated |
| `start <target> [start options] [--no-start] [--wait]` | start a capture and return; `--wait` blocks until `capture_done`; `--no-start` (synonym `--defer-start`) launches the program parked, `resume` begins recording |
| `stop [--wait]` | end the capture; the agent converts the data and reports `capture_done` with the trace path and `events_lost` |
| `resume` | begin recording a launch parked with `start --no-start` |
| `flush --name N --seconds S [--reason R]` | circular captures only: write the last S seconds to `<out dir>/<N>.tachyon` (flight recorder) |
| `cancel` | abandon the capture, discarding its data |
| `capture <target> [start options] [--duration S] [--fetch [--fetch-dir DIR]]` | `start`, wait for `--duration` seconds or for the target to exit, `stop`, wait for `capture_done`; `flushed` events (also from `Tachyon_RequestFlush`) are printed while it waits |
| `live <target> ...` | see [live](#live) |
| `fetch PATH [--out FILE]`, `processes` | see [Remote agents](#remote-agents-tcp) |
| `merge TRACE.tachyon INST.tachyon-inst [--out PATH] [--clip] [--no-siblings] [--force]` | offline: merge an SDK sidecar file, plus its numbered siblings `<stem>.<n><ext>` in numeric order, into the trace. Without `--out` the trace is replaced atomically through a temporary file; `--clip` drops records outside the trace window (default: keep them and extend the window); a sidecar merged before is refused without `--force`; unbalanced scopes and dropped records print as warnings on stderr (exit stays 0) |
| `export TRACE.tachyon --format speedscope\|chrome --out PATH [--threads TID,...] [--from-ms X] [--to-ms Y] [--symbols PATH]... [--symbol-server URL]...` | offline: write a [speedscope](https://www.speedscope.app) file (one sampled profile per thread, names need symbols) or a Chrome / Perfetto trace (scopes, frame marks, markers, plots, per-cpu running slices); `--threads` takes OS thread ids, `--from-ms` / `--to-ms` are milliseconds from the trace start, `--out -` writes to stdout; with `--json` the summary is one JSON object |
| `doctor [--hz N] [--agent PATH] [--socket PATH]` | see [doctor](#doctor-linux-and-windows) |

**Target** (`start`, `capture`, `live`): `--launch EXE`, `--attach PID` or `--system`; or the long form `--mode
launch|attach|system [--exe E] [--pid N]`.

**Start options:**

| Option | Meaning |
|---|---|
| `--args "A B \"C D\""` | arguments of the launched program as one string, split with the Windows command-line rules |
| `--args A B C` | legacy form: every following word up to the next `--option` |
| `--arg A` | one argument, repeatable; the way to pass an argument that itself starts with `--` |
| `--cwd D`, `--env K=V` (repeatable) | working directory and extra environment of the launched program |
| `--hz N` | sampling rate (default 8000; Linux: must not exceed `kernel.perf_event_max_sample_rate`) |
| `--follow-children`, `--child-filter GLOB` | also capture child processes (only those whose image name matches GLOB) |
| `--circular-mb N` | circular (flight recorder) capture of about N MiB instead of recording everything |
| `--out PATH` | output trace. **Interpreted by the agent**: a relative name is relative to the agent's working directory; default `capture-<unix time>.tachyon` there |
| `--no-kernel-stacks` | leave kernel frames out of the call stacks (both backends) |
| `--no-profile`, `--no-cswitch`, `--no-readythread` | ETW only (ignored by the Linux backend): turn off sampling, context-switch or ready-thread events |

A `capture` of a launched program ends when `--duration` elapses or the program exits, whichever is first.
`capture_done` is printed as `capture_done: path=... events_lost=N duration_s=...`; warnings from the agent (for
example that the scheduler tracepoints were unavailable) follow on stderr.

Exit codes: 0 success; 1 the agent or the capture reported an error (offline commands: bad input or output;
`doctor`: at least one FAIL); 2 cannot reach the agent or a wait timed out; 3 usage error; 4 the trace was
written but events were lost.

## Remote agents (tcp://)

`--agent tcp://HOST:PORT` (IPv6: `tcp://[::1]:7331`) talks to a `tachyon-agent --listen HOST:PORT --token-file
FILE` on another machine. The token is read from `--token-file FILE` or `$TACHYON_AGENT_TOKEN_FILE`: the first
line of the file, which must not be readable by group or others on POSIX (`chmod 600`). It is sent once, in the
first line of the connection, and never printed or logged. The channel is **not encrypted**; see "Remote
capture" in [apps/agent/LINUX.md](../agent/LINUX.md) (use a trusted network or `ssh -L`).

```sh
# on the machine with the game
head -c 24 /dev/urandom | base64 > agent.token && chmod 600 agent.token
tachyon-agent --listen 0.0.0.0:7331 --token-file agent.token --backend perf

# on the workstation (agent.token copied over a secure channel)
export TACHYON_AGENT_TOKEN_FILE=$PWD/agent.token
tachyon-cli --agent tcp://build-box:7331 processes
tachyon-cli --agent tcp://build-box:7331 capture --attach 4242 --duration 20 --fetch --fetch-dir ./captures
tachyon-cli --agent tcp://build-box:7331 fetch /var/tmp/capture-1700000000.tachyon --out ./game.tachyon
```

* `capture ... --fetch [--fetch-dir DIR]` copies the finished trace to DIR (default: the current directory) after
  `capture_done`. `--out` of a remote `capture` is a file name for the agent to use in its own output directory.
* `fetch PATH [--out FILE]` takes the path from `capture_done` / `flushed`. Only traces the agent produced since
  it started are served; any other path is refused (exit 1). The data lands in `FILE.part`; if the transfer is
  interrupted, running the same command again continues from the size of `FILE.part`. The SHA-256 the agent
  computed over the whole file must match before `FILE` appears; a mismatch of a resumed transfer discards the
  partial file and starts over once.
* `start --launch` is refused (`ENOPRIV`, exit 1) unless the agent was started with `--allow-remote-launch`.
  `--attach` and `--system` are allowed.
* Exit codes: a wrong token or an unreachable agent is 2, a missing token file is 3 (usage), a refused fetch or
  launch is 1.

## live

```
tachyon-cli [--agent ADDRESS] [--token-file FILE] [--json] live (--launch EXE [--args "A B"] | --attach PID | --system)
            [--duration S] [--window S] [--interval S] [--hz N] [--follow-children] [--child-filter GLOB]
            [--cwd D] [--env K=V]... [--circular-mb N] [--out PATH]
```

Starts a **live** capture (the agent decodes events as they arrive and streams them, see
[docs/specs/live.md](../../docs/specs/live.md)), subscribes to it and prints one line per `--interval` seconds
(default 1) built from the last `--window` seconds (default 30) of the capture:

```
[12.0s] window 12.0 s, 95871 samples, latency 0.16 s, gaps 0, dropped 0, top: testbed+0x1a40 39.9% testbed+0x1b80 30.0% ...
```

* `window` is the span of trace time in the live window, `samples` the samples in it.
* `top` lists the five functions with the most exclusive samples. No symbols are resolved: a function is its
  module plus RVA (`testbed+0x1a40`) or an absolute address, so these are frames, not source functions.
* `latency` is this machine's clock minus the timestamp of the newest sample; it is only shown for a local
  agent on Linux (the trace clock is `CLOCK_MONOTONIC_RAW`), `n/a` otherwise.
* `gaps` counts `live_gap` events the CLI survived by resubscribing (a gap means the CLI fell behind: the agent
  then sends a fresh keyframe); `dropped` is the number of live events the agent dropped because its live path
  fell behind (the final trace is unaffected).
* With `--json` every line is an object `{"t","window_s","samples","latency_s","gaps","dropped","top":[{"name",
  "samples","pct"}]}`.

The capture ends after `--duration` seconds (required for `--attach` and `--system`), when the launched target
exits, or on an error; the CLI then stops it and prints the path of the normal `.tachyon` trace, which is
written exactly as without `--live`. Exit codes are those of `capture`.

## doctor (Linux and Windows)

```
tachyon-cli [--json] doctor [--hz N] [--agent PATH] [--socket PATH]
```

Checks the prerequisites of a Linux capture and prints one block per check: `PASS`, `WARN` or `FAIL`, what was
found, and a fix hint for everything that is not `PASS`.

* `WARN` means captures work with a limit (for example: scheduler events fall back to context-switch records, or
  only your own processes can be sampled). It does not change the exit code.
* `FAIL` means a capture will not work as configured (for example: the kernel refuses `perf_event_open`, the
  requested `--hz` is above `kernel.perf_event_max_sample_rate`, the agent socket directory is not writable).
* Exit code: **0** when no check printed `FAIL`, **1** when any did, **3** on a usage error.
* `--hz N` (default 8000) is the sampling rate to test against `kernel.perf_event_max_sample_rate`.
* `--agent PATH` names the agent binary whose file capabilities are read. The default is `tachyon-agent` next to
  `tachyon-cli`, then on `$PATH`.
* `--socket PATH` (or `$TACHYON_AGENT_PIPE`) names the agent socket; the directory it lives in must be writable.
* Missing or unreadable files in `/proc` and `/sys` never abort the run: that check reports `WARN` with
  "unknown".

What each check means and how to fix it is in [apps/agent/LINUX.md](../agent/LINUX.md). Ids in order:
`kernel_version`, `process_capabilities`, `agent_capabilities`, `perf_event_paranoid`, `max_sample_rate`,
`cpu_time_max_percent`, `perf_event_mlock_kb`, `tracefs`, `perf_open_system`, `perf_open_process`, `rlimit_nofile`,
`rlimit_memlock`, `ptrace_scope`, `socket_dir`, `symbols`.

The trial `perf_event_open` checks run with the privileges of the doctor process itself. An unprivileged doctor
next to an agent that has file capabilities therefore reports warnings for the trials, not failures; run
`sudo tachyon-cli doctor` to test as root.

### JSON output

With `--json` stdout is a single line (schema version 1). Keys and their order do not change within a schema
version; new checks may be added to `checks`.

```json
{"schema":1,"platform":"linux","ok":false,"exit_code":1,"requested_hz":8000,
 "summary":{"pass":9,"warn":5,"fail":1},
 "checks":[{"id":"perf_event_paranoid","title":"kernel.perf_event_paranoid","status":"FAIL",
            "detail":"value 3 allows nothing without CAP_PERFMON (distribution patch: perf_event_open is root-only)",
            "hint":"run `sudo sysctl kernel.perf_event_paranoid=-1` ..."}]}
```

| key | type | meaning |
|---|---|---|
| `schema` | number | 1 |
| `platform` | string | `"linux"` or `"windows"` |
| `ok` | bool | no check failed |
| `exit_code` | number | the process exit code (0 or 1) |
| `requested_hz` | number | the `--hz` value |
| `summary` | object | `pass`, `warn`, `fail` counts |
| `checks[].id` | string | stable machine name |
| `checks[].title` | string | human title |
| `checks[].status` | string | `"PASS"`, `"WARN"` or `"FAIL"` |
| `checks[].detail` | string | what was found |
| `checks[].hint` | string | how to fix it; empty on `PASS` |

### Windows

`doctor` runs the Windows checks below with the same output, exit codes and JSON schema as on Linux (`platform` is
`"windows"`). `--pipe NAME` selects the agent pipe to probe (default `\\.\pipe\tachyon-agent`), `--agent PATH`
the agent executable; `--hz` is only echoed. Platforms other than Linux and Windows print a one-line note and exit 0.

| id | what it checks | FAIL when |
|---|---|---|
| `windows_version` | build number >= 15063 (Windows 10 1703: ETW system logger sessions) | older build |
| `elevation` | this process runs elevated (the capture agent must) | WARN when not elevated |
| `sysprofile_privilege` | the token holds `SeSystemProfilePrivilege` (enabled on demand) | an elevated token lacks it (WARN when not elevated) |
| `etw_sessions` | ETW loggers via `QueryAllTraces`: 64-session limit, `NT Kernel Logger` in use by another tool, stale `Tachyon ...` sessions | all 64 slots used (WARN for the rest, and when enumeration needs elevation) |
| `agent_pipe` | an agent listens on the control pipe (`WaitNamedPipe`, no connection is made) | access denied (WARN when no agent runs) |
| `agent_binary` | `tachyon-agent.exe` next to the CLI or on `PATH` | never (WARN) |
| `symbol_cache` | `%LOCALAPPDATA%\Tachyon\symbols` can be created and written | never (WARN) |
| `nt_symbol_path` | `_NT_SYMBOL_PATH` elements are well formed (`srv*cache*server`, bare URLs, plain http, missing directories) | never (WARN) |
| `dia` | `msdia140.dll` loads (next to the executable) | cannot be loaded |

Needs Windows verification: the real calls (`OpenProcessToken`, `QueryAllTraces`, `WaitNamedPipe`, DIA loading);
the windows CI job runs `tachyon-cli doctor --json` and checks that it parses.
