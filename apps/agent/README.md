# tachyon-agent

The capture daemon. It owns the capture backend, runs **one capture at a time**, and is driven over the control
protocol ([docs/specs/control-protocol.md](../../docs/specs/control-protocol.md): one JSON object per line) by
[`tachyon-cli`](../cli/README.md), by the viewer's Capture menu, and by the SDK's flush trigger. It also ingests
the instrumentation stream of SDK clients and merges it into the finished `.tachyon` trace.

```
tachyon-agent [--backend mock|etw|perf] [--socket PATH | --pipe NAME] [--socket-owner UID]
              [--log-level trace|debug|info|warn|error]
              [--listen HOST:PORT --token-file FILE [--allow-remote-launch] [--remote-out-dir DIR]]
              [--mock-fixture FILE] [--mock-duration-ms N] [--mock-steps N] [--mock-live-speed X]
              [--etw-delete-raw] [--perf-delete-raw] [--perf-ring-pages N]
              [--inst-spool-mb N] [--inst-spool-dir DIR] [--keep-inst-spool]
              [--flush-seconds N] [--flush-coalesce-ms N] [--ring-min-mb N] [--live-window-seconds N]
```

`tachyon-agent --help` prints the same text. The agent runs in the foreground until it receives Ctrl+C /
SIGINT / SIGTERM (Windows: also console close and shutdown), then exits 0.

**The default backend is `mock`**, which does not capture: it copies a fixture trace to the requested output
path. For real captures always pass `--backend etw` (Windows) or `--backend perf` (Linux), or start the agent
from the viewer, which picks the platform's real backend.

## Privileges

| Backend | Platform | Needs |
|---|---|---|
| `etw` | Windows 10 1703+ | an elevated process (administrator) holding `SeSystemProfilePrivilege`; an unelevated start exits with code 3 (`ENOPRIV`) |
| `perf` | Linux | root, or `CAP_PERFMON` (or `CAP_SYS_ADMIN`), or `kernel.perf_event_paranoid` of 0 or lower; the agent refuses to start otherwise (exit 3). Details, the `setcap` recipe, containers and tracefs in [LINUX.md](LINUX.md). |
| `mock` | any | nothing |

The CLI and the viewer connect without privileges. On Windows the agent creates its pipe so that the invoking
(unelevated) user can connect; on Linux the control socket is mode `0600`, owned by the agent's user, or handed
to the invoking user when the agent runs as root through `pkexec` / `sudo` (`--socket-owner UID`, see LINUX.md).
Run `tachyon-cli doctor` to check a machine.

## Address

`--socket PATH` and `--pipe NAME` are synonyms. Default: `$TACHYON_AGENT_PIPE`, else Windows
`\\.\pipe\tachyon-agent`, else Linux `/run/user/<uid>/tachyon-agent.sock`, or `/tmp/tachyon-agent.sock` when
`/run/user/<uid>` does not exist. The CLI, the viewer and the SDK use the same default and honour the same
environment variable. A program the agent launches gets `TACHYON_AGENT_PIPE` set to the agent's address so its
SDK finds the agent.

## Running a capture

Start the agent, then use the CLI (see its README for every command and flag):

```sh
tachyon-agent --backend perf &                       # Linux; Windows: tachyon-agent.exe --backend etw (elevated)
tachyon-cli capture --launch ./game --duration 30 --out /data/captures/game.tachyon
```

- **Output location.** `--out` is interpreted by the agent: a relative name lands in the *agent's* working
  directory, and no `--out` produces `capture-<unix time>.tachyon` there. Use absolute paths, or `fetch` the file.
- **State machine.** `idle` -> `starting` -> `capturing` -> `converting` -> `done` -> `idle`; a `start` while not
  idle is refused with `EBUSY`. A launch started with `--no-start` is parked (`starting`) until `resume`.
  `cancel` discards a capture. When a launched or attached target exits, the agent finishes the capture itself.
- **Backend files.** The ETW backend keeps the raw `<out>.etl` next to the trace unless `--etw-delete-raw`. The
  perf backend keeps its spool directory `<out>.perfspool` unless `--perf-delete-raw`; `--perf-ring-pages N` sets
  the per-cpu ring size (a power of two, default 256 pages = 1 MiB). These can be large; delete them when you
  only need the `.tachyon` file.
- **Lost events.** `capture_done` reports `events_lost`; `tachyon-cli capture` then exits 4 though the trace is
  written, and the viewer shows a warning.

## Instrumentation from the SDK

While a capture runs, an SDK client in a captured process says hello on the control address; the agent answers
with a per-process address (`tachyon-inst-<pid>.sock` beside the control socket on Linux, a
`tachyon-inst-...` named pipe on Windows) and spools its stream to `<out>.inst/inst-<pid>.tachyon-inst`. When the
capture ends the spool is merged into the trace (scopes, fibers, frame marks, markers, plots, thread names).

- `--inst-spool-mb N` caps the spool bytes per capture (default 1024). `--inst-spool-dir DIR` puts the spool
  elsewhere; `--keep-inst-spool` keeps the files after the merge (a failed merge always keeps them, and
  `tachyon-cli merge` can redo it).
- Only processes that belong to the capture may say hello: the launched or attached process, any process in a
  system capture, descendants with `--follow-children`. The agent checks the peer pid of the connection.
- If no agent answers, the SDK writes a sidecar `<exe>-<pid>.tachyon-inst` instead; add it with
  `tachyon-cli merge`. See [docs/engine-integration.md](../../docs/engine-integration.md#7-environment-variables).
- The wire format is [docs/specs/inst-wire.md](../../docs/specs/inst-wire.md).

## Flight recorder

A capture started with `--circular-mb N` (CLI) or *Circular buffer (MB)* (viewer) records continuously and keeps
only the newest data: the instrumentation rings hold `max(N, --ring-min-mb)` MiB per process (`--ring-min-mb`
default 64). A **flush** writes the last `--flush-seconds` seconds (default 10) to
`<output dir>/<name>.tachyon`:

- on demand: `tachyon-cli flush --name N --seconds S`, or the viewer's *Flush now*;
- from the program: `Tachyon_RequestFlush("reason")` writes
  `<process>-<reason>-<YYYYMMDD-HHMMSS>.tachyon`. Triggers within `--flush-coalesce-ms` (default 2000) of the
  previous flush are coalesced into it.

The output directory is the directory of the capture's `--out`. A flush of a capture that is not circular is
refused. On Windows each ETW flush restarts the kernel session, so a flushed trace's sampled data covers only the
time since the previous flush. Flushed traces are announced with a `flushed` event (`tachyon-cli capture` prints
them; the viewer's Flight Recorder panel lists them).

## Live view

A capture started with live mode (`tachyon-cli live`, the viewer's *Capture > Live...*) streams decoded events
to clients that subscribe; the agent retains the last `--live-window-seconds` seconds (default 30) for a client
that joins late. The final trace is the same with or without live. Design and costs:
[docs/specs/live.md](../../docs/specs/live.md) and the "Live view" section of [LINUX.md](LINUX.md).

## Remote capture

`--listen HOST:PORT --token-file FILE` adds an authenticated TCP listener next to the local socket, so a
workstation can capture a machine running the game. The channel is **not encrypted** (use a trusted network or an
SSH tunnel). Remote clients cannot launch programs unless `--allow-remote-launch`, cannot choose output paths
(`--remote-out-dir`, default the agent's working directory) and can only `fetch` traces this agent produced. The
token file needs a first line of at least 32 characters and, on POSIX, must not be readable by group or others;
`--token-file` and `--allow-remote-launch` are refused without `--listen`. Full description, with the CLI side:
[LINUX.md](LINUX.md#remote-capture-tcp) and [../cli/README.md](../cli/README.md#remote-agents-tcp).

## Mock backend (demos and tests)

`--backend mock` replays a fixture: `--mock-fixture FILE` (or `$TACHYON_MOCK_FIXTURE`) is copied to the requested
output path. `--mock-duration-ms N` adds an artificial conversion delay to `stop`, split into `--mock-steps N`
progress reports (default 4). With `--mock-live-speed X` a live capture replays the fixture to subscribers at X
times real time (default 0: nothing is emitted). Useful for trying the viewer's capture UI on a machine that
cannot capture:

```sh
tachyon-agent --backend mock --mock-fixture build/linux-release/fixtures/medium.tachyon &
tachyon-cli capture --system --duration 3 --out /tmp/mock.tachyon
```

## Other options

- `--log-level trace|debug|info|warn|error` (default `info`).
- `--socket-owner UID`: give the control socket and the SDK sockets to this (non-zero) uid when running as root.
  Defaults to `$PKEXEC_UID`, then `$SUDO_UID`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | normal shutdown (signal), or `--help` |
| 1 | fatal error (backend not available in this build, cannot listen, ...) |
| 2 | usage error (unknown option, bad value, `--listen` without `--token-file`, unsafe `--remote-out-dir`) |
| 3 | `ENOPRIV`: the backend needs privileges the process does not have |
