# tachyon-cli

Command-line client for `tachyon-agent` (the control protocol of `libs/control/protocol.h`) plus offline tools.
`tachyon-cli --help`-style usage is printed on a usage error; the full option list is the header comment of
`apps/cli/cli.h`.

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

## doctor (Linux)

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
| `platform` | string | `"linux"` |
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

`doctor` prints a one-line note that it is not implemented on this platform and exits 0 (with `--json`:
`{"schema":1,"platform":"windows","ok":true,"exit_code":0,"note":"..."}`). On Windows the agent needs an elevated
process (`SeSystemProfilePrivilege`).
