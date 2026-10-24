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
