<!-- Orchestrator-owned. Part 3.4, kept in sync with libs/control/protocol.h. Changes only via orchestrator. -->

### 3.4 Agent control protocol (`libs/control/protocol.h`, namespace `tachyon::control`)
Transport: Windows named pipe `\\.\pipe\tachyon-agent`, Linux Unix socket `/run/user/<uid>/tachyon-agent.sock` (`/tmp/tachyon-agent.sock` when `/run/user/<uid>` does not exist). Overrides: `--socket <path>` / `--pipe <name>` / env `TACHYON_AGENT_PIPE` (also read by the SDK). Optional remote transport: TCP (see "Remote clients"). Framing: one JSON object per line, UTF-8, `\n` terminated, at most 1 MiB per line (`kMaxLineBytes`; longer → error response, then disconnect). Every request has `"id"` echoed in the response. `kProtocolApiVersion` = 65536 (the SDK's `TACHYON_API_VERSION`).

Requests (client → agent):
```json
{"id":1,"cmd":"status"}
{"id":2,"cmd":"start","mode":"launch|attach|system","exe":"...","args":["..."],"cwd":"...","env":{"K":"V"},"pid":1234,
  "sampling_hz":8000,"follow_children":true,"child_filter":"*.exe","circular_mb":0,"out_path":"...","start_immediately":true,
  "stackwalk":{"profile":true,"cswitch":true,"readythread":true},"kernel_stacks":true,"live":false}
{"id":3,"cmd":"stop"}
{"id":4,"cmd":"flush","name":"hitch-frame-12345","seconds":10 [,"reason":"...","trigger":true,"pid":1234]}
{"id":5,"cmd":"cancel"}
{"id":6,"cmd":"inst_hello","pid":1234,"api_version":65536}   // from the SDK; the agent replies with the instrumentation pipe/socket name
{"id":7,"cmd":"auth","token":"..."}                          // remote clients only, see below
{"id":8,"cmd":"fetch","path":"...","offset":0,"length":524288 [,"sha256":true]}
{"id":9,"cmd":"processes" [,"offset":0]}
{"id":10,"cmd":"live_subscribe" [,"from_seq":N]}             // see live.md
{"id":11,"cmd":"live_unsubscribe"}
```
`Cmd` enumerates them in this order: Status, Start, Stop, Flush, Cancel, InstHello, Auth, Fetch, Processes, LiveSubscribe, LiveUnsubscribe (later commands are appended).

Responses: `{"id":N,"ok":true,...}` or `{"id":N,"ok":false,"error":"...","code":"ENOPRIV|EBUSY|ENOTFOUND|EINTERNAL"}`. Success bodies:
- `status`: `{"state":"idle|starting|capturing|converting|done","progress":0.0-1.0,"backend":"etw|perf|mock|...","elevated":bool [,"live":true,"live_seq":N]}` (`live` fields only while a live capture runs).
- `flush`: the flushed trace as a capture summary `{"path","events_lost","duration_s"[,"warnings"]}`.
- `inst_hello`: `{"pipe":"<name>"}`.
- `fetch`, `processes`, `live_subscribe`: see below.
- `stop`, `cancel`, `start`, `auth`, `live_unsubscribe`: plain ok.

Unsolicited events (agent → client): `{"event":"state","state":"...","progress":0.0-1.0}`, `{"event":"capture_done","path":"...","events_lost":0,"duration_s":31.2[,"warnings":[...]]}`, `{"event":"error","message":"..."}`, `{"event":"flushed","path","name","reason","duration_s","events_lost"[,"coalesced"]}` (every client, after each flush), and the live events `live`, `live_gap`, `live_end` (live.md).

Instrumentation data path: after `inst_hello`, the SDK connects to the pipe/socket named in the reply and streams the binary wire format of inst-wire.md (frames `{uint32 type; uint32 byte_len}`; strings sent once per stream as `StringDef` and referenced by key until the agent remaps them to `StringIdx` at merge). The agent spools the frames next to the trace (`<out>.inst/inst-<pid>.tachyon-inst`) and merges them into the finished trace.

## Rules
- **Defaults** for absent optional `start` fields: `sampling_hz` 8000, `follow_children` false, `circular_mb` 0, `start_immediately` true, `stackwalk` all true, `kernel_stacks` true, `live` false, `out_path` → `capture-<epoch>.tachyon` in the agent's working directory. `mode:"launch"` requires `exe`; `mode:"attach"` requires `pid`. `live` is encoded only when set; any mode, combinable with `circular_mb`; a backend without a live source refuses the start.
- **Malformed input** (not JSON, not an object, wrong types, unknown `cmd`, missing required field) → `{"id":<id or null>,"ok":false,"code":"EINTERNAL","error":"..."}`; unknown fields are ignored.
- **Error mapping:** NoPrivilege→ENOPRIV, Busy→EBUSY, NotFound→ENOTFOUND, all else→EINTERNAL. `stop`/`flush`/`cancel` while idle → ENOTFOUND; while starting/converting → EBUSY; a second `start` while not idle → EBUSY.
- **State machine:** idle → starting → capturing → converting → done → idle. `stop` replies `ok` when conversion begins; `capture_done` follows as an event. `done` is transient: events are `state(done)`, `capture_done`, `state(idle)`, but when `capture_done` (or the final `error` event) is observable a `status` request already reports `idle`. `cancel` stops the backend and deletes the output.
- **Deferred launch** (ETW only): `start` with `mode:"launch"` and `start_immediately:false` leaves the state at `starting` with the target parked; a second `start` with `start_immediately:true` (the CLI sends `exe:"-"`, every other field ignored) begins recording; with no parked launch it is ENOTFOUND "no parked launch". `cancel` terminates the parked process; `stop` is EBUSY.
- **Server:** every response and event to a client goes through a per-client outbound queue with its own writer thread (cap 2 MiB). A client that stops reading is dropped and never blocks other clients or state changes. Events emitted while handling a request precede its response.
- During an in-flight `flush` the state stays `capturing`. A concurrent `stop`/`cancel` waits for the flush to finish, then proceeds, and a second `flush` gets `EBUSY`.
- **Windows pipe security:** DACL `D:P` denies network logons and grants SYSTEM, Administrators and the owner full control, and Authenticated Users `0x12019b` (read/write without create-instance). `PIPE_REJECT_REMOTE_CLIENTS` and `FILE_FLAG_FIRST_PIPE_INSTANCE` are set. Clients open the pipe with `FILE_GENERIC_READ|FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES` and retry `ERROR_PIPE_BUSY` for up to 5 s.
- **Linux socket ownership:** `--socket-owner UID` wins; otherwise, when euid is 0, `$PKEXEC_UID` then `$SUDO_UID`. uid 0 and invalid values are refused. The control socket and each `tachyon-inst-<pid>.sock` are chowned (via `lstat` + `lchown`, the socket must be owned by the agent) to that uid (gid = the account's primary gid, unchanged if the account is unknown); modes stay 0600.

### Warnings
`capture_done` (and the `flush` response) carry an optional `warnings` array of strings, emitted only when non-empty: at most 32 entries of 256 characters (`kMaxSummaryWarnings`, `kMaxSummaryWarningChars`; the last kept entry says how many were left out). It carries backend warnings (`CaptureResult::warnings`), merge warnings, failed merges (with the kept spool path), spool caps hit and refused connections. The CLI prints them on stderr and includes them in `--json`.

### `inst_hello`
- Reply `{"id":N,"ok":true,"pipe":"<name>"}`: Linux `<agent socket dir>/tachyon-inst-<pid>.sock`; Windows `\\.\pipe\tachyon-inst-<pid>` when the agent pipe is the default `\\.\pipe\tachyon-agent` (case-insensitive), else `<agent pipe>-tachyon-inst-<pid>`; a name over 256 characters becomes `\\.\pipe\tachyon-inst-<16-hex FNV-1a of the lower-cased agent pipe name>-<pid>` (`libs/control/pipe_names.h`, WP-66b). The SDK always uses the name from the reply.
- ENOTFOUND outside starting or capturing (the SDK then falls back to its sidecar file and retries); ENOPRIV for a pid that is not part of the capture (attach: the attach pid; launch: the root pid; system: any pid; with `follow_children` any pid is accepted; backends without `ITargetWatch` accept any pid in launch mode) and for any remote client; EBUSY over the per-capture process cap (256). The peer pid of each data connection is verified (SO_PEERCRED / GetNamedPipeClientProcessId) against the hello pid.

### Flight recorder (WP-60)
- `flush` is accepted only during a circular capture (`circular_mb > 0`); otherwise EINTERNAL. The response carries the flushed path. Names are sanitized to `[A-Za-z0-9._-]` (other bytes become `_`, leading/trailing dots stripped, at most 120 chars, empty → `flush`, Windows device names such as CON/NUL/COM1 prefixed with `_`); an existing name gets `-N`.
- Optional request fields `reason` (string), `trigger` (bool), `pid` (uint32) are encoded only when set. With `trigger:true` (SDK `Tachyon_RequestFlush`) the agent ignores `name`/`seconds`, uses its window (`--flush-seconds`, default 10), names the file `<process>-<reason>-<YYYYMMDD-HHMMSS>`, answers `ok` at once with the planned path and flushes on a worker; a failed background flush is reported as an `error` event; stop/cancel/shutdown wait for it. Triggers within `--flush-coalesce-ms` (default 2000) of the previous accepted trigger's start, or while a flush runs, are coalesced: `ok` with the previous path, counted in the next `flushed` event's `coalesced`. A trigger during a non-circular capture is refused.

### Remote clients (WP-61, WP-66)
- `--listen HOST:PORT` adds a TCP listener beside the local socket/pipe (same line framing, TCP_NODELAY, keep-alive). The host must be explicit; `0.0.0.0`/`::` only when typed. `listenOn`/`connectTo` accept `tcp://host:port`. The channel is **unencrypted**: use a trusted network or `ssh -L`. Non-loopback binds log a warning.
- Auth: `--token-file PATH` is required with `--listen`. The first line of the file is the token (agent: ≥ 32 chars, no blanks). The file is opened once with `O_NOFOLLOW`; on POSIX the mode (no group/other access) and owner (the agent's euid or root) are checked on the descriptor; Windows has no mode/owner check. A TCP client's first line must be `{"cmd":"auth","token":"..."}` (`id` optional) within 5 s (the whole line is bounded, so dribbling bytes does not extend it); reply `ok`, or `{"ok":false,"code":"ENOPRIV","error":"authentication failed: ..."}` then close. Constant-time compare; 5 failed handshakes per minute per peer IP; at most 16 unauthenticated and 32 authenticated remote clients. No events before auth. The token is never logged. `auth` on a local connection or after the handshake is accepted and ignored.
- Restrictions: `start` with `mode:"launch"` → ENOPRIV unless `--allow-remote-launch`; `inst_hello` → ENOPRIV. A remote `out_path` is reduced to a sanitized file name inside `--remote-out-dir` (default: the agent's working directory), `-N` on collision. The output name of a remote `start` and of every flush is reserved with `O_CREAT|O_EXCL|O_NOFOLLOW` (Windows `CREATE_NEW`); an existing name, including a dangling symlink, gets `-N`. A reservation is removed on failure only if it is still the same file. After the backend returns, the file's identity (st_dev/st_ino) must equal the reservation's, else the capture/flush fails ("replaced") and nothing is served.
- Output files: trace outputs are opened without following links (O_NOFOLLOW; a reparse point is refused on Windows) and an existing file with more than one hard link is never overwritten. The final trace of a remote capture is written into exactly the file the agent reserved (`CaptureOptions::reservedOutput`); flushes and the instrumentation merge open by name with the same refusals, and the post-write identity check stays; after a merge `noteProduced` records and compares the identity of the file the merge renamed into place. A `--remote-out-dir` writable by group or others without the sticky bit only logs a warning; one owned by another user is refused (WP-66b).
- `{"cmd":"fetch","path","offset","length"[,"sha256":true]}` → `{"ok":true,"data":<base64>,"offset","size","eof"[,"sha256"]}`; chunks at most 512 KiB raw (`kMaxFetchChunkBytes`); `sha256` (hex, whole file) on the final chunk or on request (cached per (path, size, mtime)); an offset past the end → ok, empty data, `eof:false`. Only files this agent produced (captures and flushes, in this process lifetime) are served; anything else, `..`/symlink escapes and missing files → ENOTFOUND with one message; a NUL in the path → EINTERNAL. `fetch` opens the file first (no symlinks) and requires the descriptor's identity to equal the one recorded when the agent produced it. Clients write `<name>.part`, resume from its size and verify SHA-256 (a mismatching resume restarts once; a mismatching fresh transfer is an error). `tachyon-cli capture --fetch` also fetches the traces announced by `flushed` events, after the capture ends.
- `{"cmd":"processes"[,"offset":N]}` → `{"ok":true,"processes":[{"pid","name","user","cmdline"}],"offset","total"}`, sorted by pid, cmdline capped at 256 chars; clients page until `offset + count == total`.
- `connectTcp` gives each resolved address an equal share of the remaining deadline; time left by a fast failure carries over.
- Live: `live_subscribe` / `live_unsubscribe` are allowed for authenticated remote clients; at most `maxLiveSubscribers` (16) subscribers, further ones get EBUSY (live.md).

## Changelog
- 2026-10-25 (WP-10, WP-11, WP-52): header ownership, defaults, error mapping, state machine, `inst_hello`; server queues and pipe DACL; `warnings`.
- 2026-11-03 (WP-60, WP-41b, WP-61, WP-66): flight recorder fields and `flushed` event; socket ownership; remote transport, auth, `fetch`, `processes`; output reservation and identity checks.
- 2026-11-08 (WP-66b, WP-65a): per-agent Windows instrumentation pipe names; `live`, `live_subscribe`, `live_unsubscribe`, `live`/`live_gap`/`live_end` events, status `live`/`live_seq`.
- 2026-11-15: clarifications folded into the sections above; success bodies and the full command list documented.
