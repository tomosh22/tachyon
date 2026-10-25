# Tachyon instrumentation wire format, version 1

Produced by the SDK runtime (`sdk/src`, WP-51), consumed by the agent (WP-52) and by tests. The normative
definition is `sdk/src/inst_wire.h` (namespace `tachyon::inst`); this file explains it.

## Transports

The same byte stream is used for all of them:

- Agent connection: after the control-channel `inst_hello` handshake (see control-protocol.md) the SDK
  connects to the returned pipe/socket (`\\.\pipe\tachyon-inst-<pid>` / `<agent socket dir>/tachyon-inst-<pid>.sock`)
  and writes the stream. The SDK never reads from this connection.
- Sidecar file `.tachyon-inst`: exactly the same byte stream written to a file. Default path
  `<cwd>/<exe-name>-<pid>.tachyon-inst` (exe name without directory or extension); `TACHYON_SIDECAR=<path>`
  overrides, `TACHYON_SIDECAR=off` disables the sidecar.

All integers are little-endian, all structs are packed (no padding). Timestamps are `uint64` ticks of the trace
clock (QPC on Windows, `CLOCK_MONOTONIC_RAW` nanoseconds on Linux); `clock_frequency_hz` in the StreamHeader gives
ticks per second. `tid` is the OS thread id (Linux `gettid`, Windows `GetCurrentThreadId`).

## Frames

Every frame is `FrameHeader { uint32 type; uint32 byte_len; }` followed by `byte_len` payload bytes.
`byte_len` counts the payload only. Readers must skip frames of unknown type using `byte_len`.

| type | name | payload (fixed part, then variable part) |
|---|---|---|
| 1 | StreamHeader | `u32 magic ('TINS' = 0x534E4954)`, `u32 wire_version (=1)`, `u32 pid`, `u32 platform (1 Windows, 2 Linux)`, `u64 clock_frequency_hz`, `u64 process_start_ts`, `u32 api_version`, `u32 reserved` (40 bytes) |
| 2 | StringDef | `u64 ptr_key`, `u32 len`, then `len` UTF-8 bytes |
| 3 | ScopeBegin | `u64 ts`, `u32 tid`, `u32 color_rgba`, `u64 id_key`, `u64 caller_addr`, `u16 data_len`, then `data_len` bytes of data (34 + data_len) |
| 4 | ScopeEnd | `u64 ts`, `u32 tid` (12 bytes) |
| 5 | FiberEvent | `u64 ts`, `u32 tid`, `u8 kind` (0 register, 1 unregister, 2 switch_begin, 3 switch_end), `u8[3] reserved`, `u64 fiber_id`, `u64 other_fiber_id` (32 bytes) |
| 6 | FrameMark | `u64 ts`, `u32 tid`, `u64 name_key`, `u64 frame_number` (28 bytes) |
| 7 | Marker | `u64 ts`, `u32 tid`, `u64 name_key`, `u32 color_rgba`, `u16 data_len`, then data (26 + data_len) |
| 8 | Plot | `u64 ts`, `u64 name_key`, `f64 value` (24 bytes; plots are not per-thread, as in 3.1) |
| 9 | ThreadName | `u64 ts`, `u32 tid`, `u16 name_len`, then name bytes (14 + name_len) |
| 10 | Dropped | `u64 ts`, `u32 tid`, `u64 count` (20 bytes) |

The first frame of every stream is a StreamHeader.

`ScopeEnd` carries no id: it closes the innermost open scope of that thread (or, with fibers, of the fiber the
thread is currently running; see Fibers). `ScopeBegin.caller_addr` is the return address into the function
that called `Tachyon_BeginEvent`, used by analysis to splice the scope above the matching frame.
Lock waits are ScopeBegin frames with `color_rgba == 0x4C4F43FF` (TACHYON_LOCK_COLOR_SENTINEL), `id` = the lock
name or the reserved string `Tachyon.LockAcquire`, and data `0x<hex lock address>`.

## Strings

- Static strings (scope `id`, frame-mark `name`, marker `name`, plot `name`) are sent once per stream with a
  `StringDef` and referenced afterwards by `ptr_key` (`id_key`, `name_key`). A StringDef always precedes the
  first frame that references its key, in stream order, and never appears twice in one stream. The agent remaps
  keys to `StringIdx` at conversion.
- `ptr_key`: low 48 bits = the string's pointer value, high 16 bits = tag. Tag 0: NUL-terminated string
  (`Tachyon_BeginEvent`, `FrameMark`, ...), length found by strlen on first use. Tag `L+1`: explicit-length
  variant (`Tachyon_BeginEvent_N` with `idLen = L`). The same pointer used with different lengths therefore has
  different keys and separate StringDefs, so every key denotes exactly one string. Strings are truncated to 4096
  bytes. A pointer value reused for different content later in the process (static string in an unloaded module)
  keeps its first definition; this is outside the "id must be static" contract.
- Dynamic strings (scope `data`, marker `data`, thread names, the lock hex address) are carried inline after
  the fixed part with a `u16` length, truncated at 4096 bytes (thread names at 256). They are never StringDefs
  because their pointers are not stable.

## Dropped records

Each thread writes into a fixed-size ring (64 KiB default). When it is full the record is dropped and counted;
the runtime later emits a `Dropped` frame with the number of records lost on that thread since the previous
report (frame ts = time of the report). A `ScopeBegin` that is dropped also drops its matching `ScopeEnd`, and
the SDK reserves ring space so that the `ScopeEnd` of a recorded `ScopeBegin` is never dropped (outside fiber
migration), so within one thread the recorded Begin/End frames always nest properly. Conversion should show each
Dropped frame as a `Marker("tachyon.dropped", data = count)` at that timestamp. Scope nesting deeper than 256 on
one thread/fiber is not recorded (counted as dropped).

## Ordering

Frames from one thread appear in the order the thread produced them. Frames of different threads are
interleaved at ring-drain granularity (about 1 ms), so cross-thread order must be recovered from `ts`.
When the SDK switches streams (agent lost -> sidecar, sidecar -> agent) the new stream starts with a
StreamHeader and re-sends every StringDef and ThreadName seen so far.

## Fibers

`FiberEvent` kinds follow 3.1. `switch_begin(fiber_id = current, other_fiber_id = next)` is written on the
thread that is leaving `current`; `switch_end(fiber_id)` on the thread that resumes `fiber_id`, possibly a
different thread. The SDK saves/restores the open-scope bookkeeping per fiber across the pair.
