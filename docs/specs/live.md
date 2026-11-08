<!-- Orchestrator-owned spec for WP-65 (real-time live view, Linux + Windows). Changes only via orchestrator. -->

# Live view (WP-65)

Goal: while a capture runs, the viewer shows the last 30 s of the capture scrolling, with the same timeline,
Call Graph and Function List it uses for files, refreshed about twice a second. Linux (perf backend) and
Windows (ETW real-time consumer) both feed it. The capture still produces its normal `.tachyon` at `stop`.

## Split

- **WP-65a** (Linux + mock, lane A): the backend-neutral live layer, the protocol, `libs/model` `LiveModel`,
  the perf backend's live source, the mock backend's live source, `tachyon-cli live`.
- **WP-65w** (Windows, after 65a): the ETW real-time consumer feeding the same live layer.
- **WP-65b** (viewer, after 65a): the viewer "Live" mode.

## 1. Backend side (`libs/capture`)

- `CaptureOptions` gains `bool live = false` (appended at the end of the struct). With `live`, the backend
  decodes events as they arrive (in addition to whatever it writes for the final trace) and hands them to a
  `capture::ILiveSink` set with `ICaptureBackend::setLiveSink(std::shared_ptr<ILiveSink>)` before `start`.
  Backends that cannot do live return `NotSupported` from `start` when `live` is set and no sink support exists.
- The sink receives backend-neutral, already-decoded events with **raw identities** (pids, tids, addresses,
  module paths/ids, strings), never indices: `onProcess`, `onThread` (incl. name changes), `onModule`
  (load/unload, path, base, size, build-id/GUID+age), `onSample(ts, pid, tid, cpu, flags, span<const uint64_t>
  ips /*leaf first*/)`, `onContextSwitch(ContextSwitchRec-like with tids)`, `onReady`, `onScopeEvent`,
  `onFrameMark`, `onMarker`, `onPlot` (instrumentation from the agent's ingest for live captures), and
  `onTimeAdvance(ts)` (a heartbeat so a quiet capture still scrolls). Calls may come from any one backend thread
  at a time (the sink serialises nothing: the backend guarantees one caller at a time). Timestamps use the
  trace clock (same as the final trace).
- Live decoding must not lose final-trace data: if the live path falls behind, it drops live events (counted,
  reported as `dropped` in the next batch), never trace events.

## 2. Agent side (`apps/agent`)

- `start` gains optional `"live": true` (any mode; combinable with `circular_mb`). The agent owns a
  `LiveEncoder` (in `libs/control` or `apps/agent`) implementing `ILiveSink`: it interns raw identities into
  **live indices** (strings, processes, threads, modules, frames = (module, rva) or unmapped address, callstack
  nodes as a prefix tree), batches events every 250 ms (or 256 KiB), and keeps the last `window_s` (default
  30, `--live-window-seconds`) of batches plus all table definitions for late subscribers.
- Protocol additions (JSON lines, `Cmd` appended after `Processes`):
  - `{"cmd":"live_subscribe"[,"from_seq":N]}` → ok `{"seq":N}`; then events
    `{"event":"live","seq":N,"keyframe":bool,"data":"<base64>"}` until `live_unsubscribe`, capture end
    (`{"event":"live_end","seq":N}`), or disconnect. Without `from_seq`, or when `from_seq` is no longer
    retained, the first event is a **keyframe**: all table definitions plus every retained batch, re-encoded,
    split over several events if needed (each `data` at most 512 KiB raw, under the 1 MiB line limit).
  - `{"cmd":"live_unsubscribe"}` → ok.
  - A client whose outbound queue would exceed its cap gets `{"event":"live_gap","seq":N}` instead of the
    batches it missed and is expected to resubscribe (the next event is a keyframe).
  - `status` reports `"live":true` and `"live_seq":N` while a live capture runs.
  - Allowed for remote (TCP) clients after auth; `live_subscribe` outside a live capture → ENOTFOUND.
- Batch encoding (`data`, little-endian): a sequence of `{uint32 type; uint32 byte_len; payload}` records:
  `StringDef{u32 idx; bytes}`, `ProcessDef{u32 idx; ProcessRec with string indices}`, `ThreadDef{u32 idx;
  ThreadRec}`, `ThreadName{u32 thread; u32 string}`, `ModuleDef{u32 idx; ModuleRec}`, `FrameDef{u32 idx;
  FrameRec}`, `NodeDef{u32 idx; CallstackNodeRec}`, `Samples{u32 count; then columns ts[], thread[], node[],
  cpu[], flags[]}`, `ContextSwitches{ContextSwitchRec[]}`, `Ready{ReadyRec[]}`, `Scopes{ScopeEventRec[]}`,
  `FrameMarks{FrameMarkRec[]}`, `Markers{MarkerRec[]}`, `Plots{PlotSampleRec[]}`, `TimeAdvance{u64 ts}`,
  `Dropped{u64 events}`. Record structs are the `libs/trace/trace_format.h` ones with live indices in their
  index fields. Definitions always precede their first use within the stream. Unknown types are skipped.
- Bounded: the encoder's retained window, the per-client queue (2 MiB cap like other events, live events
  dropped first) and the interning tables (capped; an over-cap identity maps to an `<overflow>` entry and is
  counted) are all bounded.

## 3. Model side (`libs/model`)

- `model::LiveModel` (thread-compatible, one writer): `apply(span<const std::byte> data, bool keyframe)`
  decodes a batch (a keyframe resets the window and tables), keeps table definitions forever (capped as in
  the encoder) and events for the last `windowNs` of trace time (default 30 s; older events are discarded as
  newer ones arrive; `TimeAdvance` moves "now").
- `std::shared_ptr<const TraceModel> LiveModel::snapshot() const` builds an ordinary `TraceModel` of the window
  (header start/end = window bounds, `isPartial = true`, `loadStatus.complete = false`, `captureInfo.raw_json`
  with `{"live":true}`), indices remapped so it is a valid model for analysis, LOD, thread states and scope
  pairing. Target: snapshot of 30 s × 8 kHz × 16 threads (~4 M samples) in under 150 ms on the dev VM
  (asserted at 2×). `generation()` increases with every applied batch.

## 4. Viewer side (`apps/viewer`)

- Capture menu: "Live..." (local or remote agent, using the existing AgentSession / remote connection):
  starts a capture with `live:true` (launch, attach or system, as the other dialogs) and opens a Live document.
- A Live document holds a `LiveModel` fed by `live` events on the AgentSession worker; the UI thread takes a
  `snapshot()` at most every 500 ms and swaps it in like a streaming preview (WP-64): the timeline keeps its
  row flags and, while "Follow" is on, shows the last 30 s ending at "now" and scrolls; panning or zooming
  turns Follow off, a "Follow" button turns it back on. Analysis panels refresh on each swap (queries on the
  previous snapshot are cancelled).
- "Pause" freezes the view (the LiveModel keeps receiving); "Save last 30 s" sends `flush` when the capture is
  circular, else is disabled with a hint; "Stop" stops the capture and opens the final trace (the normal
  capture_done path).
- Gaps (`live_gap`) resubscribe and show a toast; `live_end` turns the document into a static snapshot.
- Symbols: the Live document resolves modules as they appear (SymbolService per snapshot generation is too
  costly: resolve against the LiveModel's module table once per new module and reuse results across snapshots).

## 5. Acceptance (whole WP-65)

- Linux: `tachyon-agent --backend perf` + testbed `spin` live for 60 s: the viewer (headless test harness)
  shows samples within 1 s of their timestamp (p95 latency, asserted at 2 s), the window never exceeds 30 s,
  shares of the spin functions in the live snapshot match the final trace within ±5 points, viewer memory flat
  (growth < 10% from second 20 to 60), no `live_gap` at 8 kHz on the dev VM.
- Windows: the same with `--backend etw` on the elevated CI runner (latency asserted at 3 s: ETW real-time
  buffers flush about once a second).
- Mock backend: deterministic tests of encoder/decoder, keyframes, gaps, window trimming, remote subscribe.

## Clarifications added with WP-65a (2026-11-08, binding)

- Encoding: the authoritative table is the header comment of `libs/model/live_model.h` (record types 1–17 in the order of section 2; type 17 `Info` = `u64 clock_frequency_hz; u32 platform; u32 sampling_hz; u64 start_ts`, the first record of every keyframe and of the first batch). String 0 is `""`, string 1 is reserved as `<overflow>`, node 0 is the root. Table caps (`live_wire::kMax*`) are shared by encoder and model; the model refuses definitions beyond them, and `apply` returns CorruptData on a malformed batch after applying what precedes it (the client resubscribes).
- A keyframe may span several `live` events with the same `seq`; only the first carries `keyframe:true` (the client passes `keyframe=false` to `LiveModel::apply` for the rest). `live_subscribe` replies `{"seq":N}`: without `from_seq`, N is the last batch inside the keyframe (the next event is N+1); with a retained `from_seq`, N = `from_seq` and no keyframe is sent; a `from_seq` that is not retained or lies in the future gets a keyframe.
- Each subscriber is served by its own agent thread, which waits while the client's outbound queue is above 1.5 MiB (below the 2 MiB cap, so other traffic is never dropped) and sends `live_gap` and stops after 5 s stalled (`AgentConfig::liveStallMs`) or when it falls behind the retained window.
- `ICaptureBackend::supportsLive()`; a live `start` on a backend without it is refused (EINTERNAL "not supported"). `ILiveSink` adds `onSessionInfo` and `onDropped`; switch/ready callbacks carry the pids of the new/readied thread. Instrumentation callbacks may arrive concurrently from the agent's per-process connection threads; `LiveEncoder` is fully synchronised and `LiveModel` is internally locked (`snapshot()` blocks `apply()` while it copies).
- The perf live decoder drops sample and switch chunks that waited more than 2 s (`LiveSourceConfig::maxLag`) and reports them as `Dropped`; metadata is never dropped. The final trace is unaffected.
- `libs/control` (which holds `LiveEncoder`) links `libs/model` (which holds the wire format).
- Acceptance memory criterion: flat from second 35 (the 30 s window is still filling before that); bytes per window sample < 64.
- Measured (dev VM, Linux perf, 8 kHz): latency p50 0.16 s / p95 0.27 s (spin, 60 s); window ≤ 30.00 s; shares live vs final identical to 0.01 points; snapshot of 3.84 M samples 34–47 ms; encoder 174 ns/sample.
