<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.4. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.4 Agent control protocol (`libs/control/protocol.h`)
Transport: Windows named pipe `\\.\pipe\tachyon-agent`, Linux Unix socket `/run/user/<uid>/tachyon-agent.sock` (fallback `/tmp`). Framing: one JSON object per line, UTF-8, `\n` terminated. Every request has `"id"` echoed in the response.
Requests (client → agent):
```json
{"id":1,"cmd":"status"}
{"id":2,"cmd":"start","mode":"launch|attach|system","exe":"...","args":["..."],"cwd":"...","env":{"K":"V"},"pid":1234,
  "sampling_hz":8000,"follow_children":true,"child_filter":"*.exe","circular_mb":0,"out_path":"...","start_immediately":true,
  "stackwalk":{"profile":true,"cswitch":true,"readythread":true},"kernel_stacks":true}
{"id":3,"cmd":"stop"}
{"id":4,"cmd":"flush","name":"hitch-frame-12345","seconds":10}
{"id":5,"cmd":"cancel"}
{"id":6,"cmd":"inst_hello","pid":1234,"api_version":65536}   // from SDK client; agent replies with shm/pipe details
```
Responses/events (agent → client): `{"id":N,"ok":true,...}` or `{"id":N,"ok":false,"error":"...","code":"ENOPRIV|EBUSY|ENOTFOUND|EINTERNAL"}`; unsolicited events `{"event":"state","state":"idle|starting|capturing|converting|done","progress":0.0-1.0}`, `{"event":"capture_done","path":"...","events_lost":0,"duration_s":31.2}`, `{"event":"error","message":"..."}`.
Instrumentation data path: after `inst_hello`, the agent opens a second pipe (name from the `inst_hello` reply, below); the SDK streams binary `ScopeEventRec`/`FiberEventRec`/... frames prefixed with `{uint32 type; uint32 byte_len}`; strings are sent once per process as `{type=StringDef, uint64 ptr_key, uint32 len, bytes}` and referenced by `ptr_key` until the agent remaps to `StringIdx` at conversion.

## Orchestrator clarifications (binding; merged with WP-10, 2026-10-25)
Headers: `libs/control/protocol.h` (`tachyon::control`), now orchestrator-owned.
- Defaults for absent optional `start` fields: `sampling_hz` 8000, `follow_children` false, `circular_mb` 0, `start_immediately` true, `stackwalk` all true, `kernel_stacks` true, `out_path` → `capture-<epoch>.tachyon` in the agent's working directory. `mode:"launch"` requires `exe`; `mode:"attach"` requires `pid`.
- Malformed input (not JSON, not an object, wrong types, unknown `cmd`, missing required field) → `{"id":<id or null>,"ok":false,"code":"EINTERNAL","error":"..."}`; unknown fields are ignored. Max line length 1 MiB (longer → error response, then disconnect).
- Error mapping: NoPrivilege→ENOPRIV, Busy→EBUSY, NotFound→ENOTFOUND, all else→EINTERNAL. `stop`/`flush`/`cancel` while idle → ENOTFOUND; while starting/converting → EBUSY; a second `start` while not idle → EBUSY.
- `stop` replies `ok` when conversion begins; `capture_done` follows as an event. `done` is transient: events are `state(done)`, `capture_done`, `state(idle)`.
- `cancel` stops the backend and deletes the output.
- `inst_hello` reply: `{"id":N,"ok":true,"pipe":"<name>"}`; Linux `<agent socket dir>/tachyon-inst-<pid>.sock`, Windows `\\.\pipe\tachyon-inst-<pid>` when the agent pipe is the default `\\.\pipe\tachyon-agent`, else `<agent pipe>-tachyon-inst-<pid>`; a name over 256 characters becomes `\\.\pipe\tachyon-inst-<16-hex FNV-1a of the lower-cased agent pipe name>-<pid>` (WP-66b). The SDK always uses the name from the reply.
- Overrides: `--socket <path>` / `--pipe <name>` / env `TACHYON_AGENT_PIPE`.

## Clarifications added with WP-11 (2026-10-25)
- Server: every response and event to a client goes through a per-client outbound queue with its own writer thread (cap 2 MiB). A client that stops reading is dropped and never blocks other clients or state changes. Events emitted while handling a request precede its response.
- During an in-flight `flush` the state stays `capturing`. A concurrent `stop`/`cancel` waits for the flush to finish, then proceeds, and a second `flush` gets `EBUSY`.
- Windows named pipe: DACL `D:P` denies network logons and grants SYSTEM, Administrators and the owner full control, and Authenticated Users `0x12019b` (read/write without create-instance). `PIPE_REJECT_REMOTE_CLIENTS` and `FILE_FLAG_FIRST_PIPE_INSTANCE` are set. Clients open the pipe with `FILE_GENERIC_READ|FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES` and retry `ERROR_PIPE_BUSY` for up to 5 s.

## Clarifications added with WP-52 (2026-10-25, binding)
- `CaptureSummary` / `capture_done` gains an optional `warnings` array of strings, emitted only when non-empty. At most 32 entries of 256 chars each (`kMaxSummaryWarnings`, `kMaxSummaryWarningChars`). It carries merge warnings, failed merges (with the kept spool path), spool caps hit and refused connections. The CLI prints them on stderr and includes them in `--json`.
- `inst_hello` returns ENOTFOUND outside starting or capturing, ENOPRIV for a pid that is not part of the capture (attach: the attach pid; launch: the root pid; system: any pid; with follow_children any pid is accepted), and EBUSY over the per-capture process cap.
- When `capture_done` (or the final error event) is observable, a `status` request already reports `idle`. The wire order stays `state(done)`, `capture_done`, `state(idle)`.

## Clarifications added with WP-60 (2026-11-03, binding)

- `flush` is accepted only during a circular capture (`circular_mb > 0`); otherwise EINTERNAL. The response carries the flushed path. Names are sanitized to `[A-Za-z0-9._-]` (other bytes become `_`, leading/trailing dots stripped, at most 120 chars, empty → `flush`, Windows device names such as CON/NUL/COM1 prefixed with `_`); an existing name gets `-N`.
- Optional request fields: `reason` (string), `trigger` (bool), `pid` (uint32), encoded only when set. With `trigger:true` (SDK `Tachyon_RequestFlush`) the agent ignores `name`/`seconds`, uses its window (`--flush-seconds`, default 10), names the file `<process>-<reason>-<YYYYMMDD-HHMMSS>`, answers `ok` at once with the planned path and flushes on a worker; a failed background flush is reported as an `error` event; stop/cancel/shutdown wait for it. Triggers within `--flush-coalesce-ms` (default 2000) of the previous accepted trigger's start, or while a flush runs, are coalesced: `ok` with the previous path, counted in the next `flushed` event.
- New optional event `{"event":"flushed","path","name","reason","duration_s","events_lost"[,"coalesced"]}`, sent to every client after each flush.

## Clarifications added with WP-41b and WP-61 (2026-11-03, binding)

- Socket ownership (Linux): `--socket-owner UID` wins; otherwise, when euid is 0, `$PKEXEC_UID` then `$SUDO_UID`. uid 0 and invalid values are refused. The control socket and each `tachyon-inst-<pid>.sock` are chowned to that uid (gid = the account's primary gid, unchanged if the account is unknown); modes stay 0600.
- Remote transport: `--listen HOST:PORT` adds a TCP listener beside the local socket/pipe (same line framing, TCP_NODELAY, keep-alive). The host must be explicit; `0.0.0.0`/`::` only when typed. `listenOn`/`connectTo` accept `tcp://host:port`. The channel is **unencrypted**: use a trusted network or `ssh -L`. Non-loopback binds log a warning.
- Auth: `--token-file PATH` is required with `--listen`. The first line of the file is the token (agent: >= 32 chars, no blanks; on POSIX not accessible to group/other). A TCP client's first line must be `{"cmd":"auth","token":"..."}` (`id` optional) within 5 s; reply `ok`, or `{"ok":false,"code":"ENOPRIV","error":"authentication failed: ..."}` then close. Constant-time compare; 5 failed handshakes per minute per peer IP; at most 16 unauthenticated and 32 authenticated remote clients. No events before auth. The token is never logged. `auth` on a local connection or after the handshake is accepted and ignored.
- Remote restrictions: `start` with `mode:"launch"` → ENOPRIV unless `--allow-remote-launch`; `inst_hello` → ENOPRIV. A remote `out_path` is reduced to a sanitized file name inside `--remote-out-dir` (default: the agent's working directory), `-N` on collision.
- `{"cmd":"fetch","path","offset","length"[,"sha256":true]}` → `{"ok":true,"data":<base64>,"offset","size","eof"[,"sha256"]}`; chunks at most 512 KiB raw (`kMaxFetchChunkBytes`); `sha256` (hex, whole file) on the final chunk or on request; an offset past the end → ok, empty data, `eof:false`. Only files this agent produced (captures and flushes) are served; anything else, `..`/symlink escapes and missing files → ENOTFOUND with one message; a NUL in the path → EINTERNAL. Clients write `<name>.part`, resume from its size and verify SHA-256 (a mismatching resume restarts once; a mismatching fresh transfer is an error).
- `{"cmd":"processes"[,"offset":N]}` → `{"ok":true,"processes":[{"pid","name","user","cmdline"}],"offset","total"}`, sorted by pid, cmdline capped at 256 chars; clients page until `offset + count == total`.
- `Cmd` appends `Auth`, `Fetch`, `Processes` after `InstHello`.

## Clarifications added with WP-66 (2026-11-03, binding)

- Remote `start` and every `flush` output name is reserved with `O_CREAT|O_EXCL|O_NOFOLLOW` (Windows `CREATE_NEW`); an existing name, including a dangling symlink, gets `-N`. A reservation is removed on failure only if it is still the same file. After the backend returns, the file's identity (st_dev/st_ino) must equal the reservation's, else the capture/flush fails ("replaced") and nothing is served.
- Trace outputs are opened without following links (O_NOFOLLOW; a reparse point is refused on Windows), and an existing file with more than one hard link is never overwritten. The final trace of a remote capture is written into exactly the file the agent reserved (same device and inode, `CaptureOptions::reservedOutput`); flushes and the instrumentation merge open by name with the same refusals, and the post-write identity check stays. After a merge, `noteProduced` records and compares the identity of the file the merge renamed into place. So a `--remote-out-dir` writable by group or others without the sticky bit only logs a warning (a local user can delete or swap a trace, which then fails or is not served, but cannot redirect a write); one owned by another user is refused (WP-66b).
- Token file: opened once with `O_NOFOLLOW`; mode (no group/other access) and owner (the agent's euid or root) are checked on the descriptor. Windows: no mode/owner check.
- `fetch` opens the file first (no symlinks) and requires the descriptor's identity to equal the one recorded when the agent produced it. The whole-file SHA-256 is cached per (path, size, mtime).
- `connectTcp` gives each resolved address an equal share of the remaining deadline; time left by a fast failure carries over.
- `tachyon-cli capture --fetch` also fetches the traces announced by `flushed` events, after the capture ends.
