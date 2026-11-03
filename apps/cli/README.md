# tachyon-cli

Command-line client for `tachyon-agent` (the control protocol of `libs/control/protocol.h`) plus offline tools.
`tachyon-cli --help`-style usage is printed on a usage error; the full option list is the header comment of
`apps/cli/cli.h`.

```
tachyon-cli [--socket PATH | --pipe NAME] [--json] [--timeout SECONDS] <command> [options]
```

| command | what it does | needs the agent |
|---|---|---|
| `status`, `start`, `stop`, `resume`, `flush`, `cancel`, `capture` | drive a capture | yes |
| `merge` | add an SDK sidecar file to a trace | no |
| `export` | write a speedscope or Chrome trace file | no |
| `doctor` | check whether this machine can capture | no |

Exit codes: 0 success; 1 the agent or the capture reported an error (offline commands: bad input or output;
`doctor`: at least one FAIL); 2 cannot reach the agent or a wait timed out; 3 usage error; 4 the trace was
written but events were lost.

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
