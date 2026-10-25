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
Instrumentation data path: after `inst_hello`, the agent opens a second pipe `\\.\pipe\tachyon-inst-<pid>`; the SDK streams binary `ScopeEventRec`/`FiberEventRec`/... frames prefixed with `{uint32 type; uint32 byte_len}`; strings are sent once per process as `{type=StringDef, uint64 ptr_key, uint32 len, bytes}` and referenced by `ptr_key` until the agent remaps to `StringIdx` at conversion.

## Orchestrator clarifications (binding; merged with WP-10, 2026-10-25)
Headers: `libs/control/protocol.h` (`tachyon::control`), now orchestrator-owned.
- Defaults for absent optional `start` fields: `sampling_hz` 8000, `follow_children` false, `circular_mb` 0, `start_immediately` true, `stackwalk` all true, `kernel_stacks` true, `out_path` → `capture-<epoch>.tachyon` in the agent's working directory. `mode:"launch"` requires `exe`; `mode:"attach"` requires `pid`.
- Malformed input (not JSON, not an object, wrong types, unknown `cmd`, missing required field) → `{"id":<id or null>,"ok":false,"code":"EINTERNAL","error":"..."}`; unknown fields are ignored. Max line length 1 MiB (longer → error response, then disconnect).
- Error mapping: NoPrivilege→ENOPRIV, Busy→EBUSY, NotFound→ENOTFOUND, all else→EINTERNAL. `stop`/`flush`/`cancel` while idle → ENOTFOUND; while starting/converting → EBUSY; a second `start` while not idle → EBUSY.
- `stop` replies `ok` when conversion begins; `capture_done` follows as an event. `done` is transient: events are `state(done)`, `capture_done`, `state(idle)`.
- `cancel` stops the backend and deletes the output.
- `inst_hello` reply: `{"id":N,"ok":true,"pipe":"<name>"}`; Linux `<agent socket dir>/tachyon-inst-<pid>.sock`, Windows `\\.\pipe\tachyon-inst-<pid>`.
- Overrides: `--socket <path>` / `--pipe <name>` / env `TACHYON_AGENT_PIPE`.

## Clarifications added with WP-11 (2026-10-25)
- Server: every response and event to a client goes through a per-client outbound queue with its own writer thread (cap 2 MiB). A client that stops reading is dropped and never blocks other clients or state changes. Events emitted while handling a request precede its response.
- During an in-flight `flush` the state stays `capturing`. A concurrent `stop`/`cancel` waits for the flush to finish, then proceeds, and a second `flush` gets `EBUSY`.
- Windows named pipe: DACL `D:P` denies network logons and grants SYSTEM, Administrators and the owner full control, and Authenticated Users `0x12019b` (read/write without create-instance). `PIPE_REJECT_REMOTE_CLIENTS` and `FILE_FLAG_FIRST_PIPE_INSTANCE` are set. Clients open the pipe with `FILE_GENERIC_READ|FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES` and retry `ERROR_PIPE_BUSY` for up to 5 s.
