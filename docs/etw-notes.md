# ETW notes (WP-01 `etlstat`)

Status: **documented layouts only**. This file was written on a Linux machine without Windows, so nothing
below has been observed yet. Every item tagged **[VERIFY]** must be confirmed (or corrected) by running
`etlstat` on an elevated Windows prompt; replace the tag with `[OBSERVED <date>, <Windows build>]` and fix
the text. WP-11 and WP-12 depend on this document, so do not leave unverified claims untagged.

Sources: Tachyon spec Appendix B, Microsoft documentation for `EVENT_TRACE_PROPERTIES`,
`TraceSetInformation`, `TRACE_PROFILE_INTERVAL`, `CLASSIC_EVENT_ID`, and the public kernel MOF classes
(Process_V4, Thread_V3, Image_Load, PerfInfo_SampledProfile, StackWalk_Event, CSwitch, ReadyThread).

## 1. Recipe implemented by `etlstat record`

| Step | Call | Notes |
|---|---|---|
| Privilege | `AdjustTokenPrivileges(SeSystemProfilePrivilege)` | Required for the PROFILE flag and the sampling interval. Held (but not enabled) by Administrators; a non-elevated token gets `ERROR_NOT_ALL_ASSIGNED` and etlstat reports `NoPrivilege`. [VERIFY: whether `StartTrace` alone also needs `SeSystemProfilePrivilege` or only `TraceSetInformation`] |
| Session | `StartTraceW(&h, L"Tachyon etlstat <pid>", props)` | `LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL \| EVENT_TRACE_SYSTEM_LOGGER_MODE`, `Wnode.ClientContext = 1` (QPC), `Wnode.Flags = WNODE_FLAG_TRACED_GUID`. |
| Flags | `EnableFlags = PROCESS \| THREAD \| IMAGE_LOAD \| PROFILE \| CSWITCH \| DISPATCHER` | = `0x01000817`. |
| Buffers | `BufferSize = 1024` KB, `MinimumBuffers = max(64, 4 x cores)`, `MaximumBuffers = 4 x MinimumBuffers`, `FlushTimer = 1` s | Formula in `session_config.cpp` (unit tested). |
| Interval | `TraceSetInformation(h, TraceSampledProfileIntervalInfo, &TRACE_PROFILE_INTERVAL{Source=0, Interval=1221}, ...)` | Units are 100 ns. 1221 = 122.1 us = ~8.19 kHz is the smallest value Windows accepts. `--hz` >= 8000 uses 1221; lower rates use `10,000,000 / hz`. [VERIFY: whether the call takes the session handle or 0. etlstat tries the session handle first, then 0, and prints which one worked: "interval set with session handle".] |
| Stacks | `TraceSetInformation(h, TraceStackTracingInfo, CLASSIC_EVENT_ID[3], ...)` | PerfInfo/46 (SampledProfile), Thread/36 (CSwitch), Thread/50 (ReadyThread). |
| Run | optional `CreateProcessW(--exe)`, wait N s (or until the process exits), `TerminateProcess` if still running | The process is started after the session so its process/thread/image start events are captured. |
| Stop | `ControlTraceW(h, name, props, EVENT_TRACE_CONTROL_STOP)` | Rewrites `props` with `EventsLost`, `LogBuffersLost`, `RealTimeBuffersLost`, `BuffersWritten`, `NumberOfBuffers`; printed by `record`. |

### Why a uniquely named system logger

The classic "NT Kernel Logger" is a singleton: WPA, xperf, PerfView or a second tool would make `StartTrace`
fail with `ERROR_ALREADY_EXISTS` (or steal our session). With `EVENT_TRACE_SYSTEM_LOGGER_MODE` (Windows 8+)
a session with any name can receive kernel events; up to 8 such system loggers can coexist. etlstat names the
session `Tachyon etlstat <pid>` and gives it a GUID derived from the pid and a timestamp. If `StartTrace`
returns `ERROR_INVALID_PARAMETER` it retries with `SystemTraceControlGuid`; the `record` output says when that
fallback was needed. [VERIFY: which GUID Windows accepts for a custom-named system logger.]

### Pitfalls to check on the real machine

- Admin is required; a non-elevated run reports `NoPrivilege` (exit code 1).
- Complete kernel stacks may need the `DisablePagingExecutive=1` registry value (HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management, reboot). etlstat does not set it. [VERIFY: compare kernel-frame completeness with and without.]
- 64-bit user stacks truncate when stack pages are paged out; expect a small fraction of short stacks.
- Stack walks are separate StackWalk events and are not guaranteed to follow their parent immediately, nor to exist for every parent (see section 3).
- `LogBuffersLost` / header `BuffersLost` mean whole buffers were dropped: raise `MaximumBuffers` or lower `--hz`.

## 2. Event layouts (x64) and how etlstat decodes them

All fields little-endian. `ptr` = 8 bytes. Trailing bytes beyond the documented layout are ignored by the
decoders (`etw_payload.cpp`), short payloads are counted as `malformedEvents`. All layouts are covered by unit
tests on hand-built byte arrays, which only prove the decoder matches *this document*; the observed versions
and sizes are recorded in the table in section 5 once measured.

Providers and opcodes:

| Provider | GUID | Opcodes |
|---|---|---|
| Process | `3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c` | Start 1, End 2, DCStart 3, DCEnd 4 |
| Thread | `3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c` | Start 1, End 2, DCStart 3, DCEnd 4, CSwitch 36, ReadyThread 50, SetName 72 |
| Image | `2cb15d1d-5fc1-11d2-abe1-00a0c911f518` | Unload 2, DCStart 3, DCEnd 4, Load 10 |
| PerfInfo | `ce1dbfb4-137e-4da6-87b0-3f59aa102cbc` | SampledProfile 46 |
| StackWalk | `def2fe46-7bd6-4b80-bd94-f57fe20d0ce3` | Stack 32, StackKeyKernel 34, StackKeyUser 35, StackKeyRundown 36, StackKeyDelete 37 |
| EventTrace (file header event) | `68fdd900-4a3e-11d1-84f4-0000f80464e3` | 0 |

[VERIFY] all opcodes, in particular 34-37 (key-based stack events appear only with `PERF_STACK_KEY`-style
stack caching and are only counted by etlstat, never decoded) and Thread/72.

### PerfInfo / SampledProfile (46) [VERIFY version; expected v2, size 16]

| Offset | Type | Field |
|---|---|---|
| 0 | ptr | InstructionPointer |
| 8 | u32 | ThreadId (0 = idle thread) |
| 12 | u16 | Count |
| 14 | u16 | Reserved |

### Thread / CSwitch (36) [VERIFY version; expected v2, size 24]

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | NewThreadId |
| 4 | u32 | OldThreadId |
| 8 | i8 | NewThreadPriority |
| 9 | i8 | OldThreadPriority |
| 10 | u8 | PreviousCState |
| 11 | i8 | SpareByte |
| 12 | i8 | OldThreadWaitReason (KWAIT_REASON, see `wait_reason.cpp`) |
| 13 | i8 | OldThreadWaitMode |
| 14 | i8 | OldThreadState |
| 15 | i8 | OldThreadWaitIdealProcessor |
| 16 | u32 | NewThreadWaitTime |
| 20 | u32 | Reserved |

### Thread / ReadyThread (50) [VERIFY version; expected v2, size 8]

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | TThreadId (the thread being readied) |
| 4 | i8 | AdjustReason |
| 5 | i8 | AdjustIncrement |
| 6 | i8 | Flag |
| 7 | i8 | Reserved |

The readying thread is the header ThreadId. [VERIFY]

### StackWalk / Stack (32) [VERIFY version; expected v2]

| Offset | Type | Field |
|---|---|---|
| 0 | u64 | EventTimeStamp (timestamp of the parent event, same clock as the header) |
| 8 | u32 | StackProcess |
| 12 | u32 | StackThread |
| 16 | ptr[] | Stack, `(UserDataLength - 16) / 8` frames, innermost first |

Kernel-mode addresses are >= `0xFFFF800000000000`. The user/kernel split inside one array is a position in the
array; [VERIFY] whether kernel frames come first (innermost first) and whether user frames of a CSwitch
stack are present.

### Image / Load (10), DCStart (3), DCEnd (4), Unload (2) [VERIFY version; expected v3]

| Offset | Type | Field |
|---|---|---|
| 0 | ptr | ImageBase |
| 8 | ptr | ImageSize |
| 16 | u32 | ProcessId |
| 20 | u32 | ImageChecksum |
| 24 | u32 | TimeDateStamp |
| 28 | u32 | Reserved0 |
| 32 | ptr | DefaultBase |
| 40 | u32 x4 | Reserved1..4 |
| 56 | wstr | FileName (NUL terminated UTF-16, NT device path) |

### Thread / Start, End, DCStart, DCEnd [VERIFY version; expected v3]

etlstat decodes only the leading `u32 ProcessId; u32 TThreadId`. Remaining expected fields: StackBase,
UserStackBase, StackLimit, UserStackLimit, Affinity, Win32StartAddr, TebBase (ptr each), SubProcessTag (u32),
BasePriority, PagePriority, IoPriority, ThreadFlags (u8 each).

### Thread / SetName (72) [VERIFY existence and version]

Expected `{u32 ProcessId; u32 ThreadId; wstr ThreadName}`. Whether the kernel logger emits it, only for
threads named after the session started (`SetThreadDescription`), or only via rundown, is the open question
that `etlstat stats` answers ("Thread/SetName events: PRESENT/ABSENT" plus the first 16 names). WP-11 needs
this to decide how thread names are obtained.

### Process / Start, End, DCStart, DCEnd [VERIFY version; expected v3/v4]

Expected x64 prefix: `ptr UniqueProcessKey; u32 ProcessId; u32 ParentId; u32 SessionId; i32 ExitStatus;
ptr DirectoryTableBase; [v4: u32 Flags;] sid UserSID; char ImageFileName[]; wchar CommandLine[]`. The
`UserSID` encoding is variable. etlstat's heuristic: a zero pointer-size value means "no SID" (8 bytes);
otherwise a 16-byte `TOKEN_USER`-like header followed by `SID{rev, subCount, idAuth[6], sub[subCount]}`; if
the image name is still empty it scans for the first printable NUL-terminated string. Used only by
`--image` to resolve a pid. [VERIFY, and prefer `--pid` if the name is not found.]

### CPU number and timestamps

CPU = `EVENT_RECORD.BufferContext.ProcessorIndex` (`ProcessorNumber` is the low byte of the same union).
Timestamps are raw QPC ticks because of `PROCESS_TRACE_MODE_RAW_TIMESTAMP`; frequency =
`LogfileHeader.PerfFreq` (typically 10,000,000 on modern Windows). [VERIFY]

## 3. Stack-to-parent matching (what `etlstat stats` implements)

A StackWalk/Stack event belongs to an earlier event of the same thread. Matching key:
`(EventTimeStamp, StackThread)` against pending parents, which are SampledProfile, CSwitch and ReadyThread
events (the only types stack tracing is enabled for). Because it is not documented which thread id the
kernel puts in `StackThread` for CSwitch (header thread, NewThreadId, or OldThreadId), every parent is
registered under all candidates and the report prints a histogram of which role matched
("matched via header ThreadId / payload thread / payload OldThreadId"). [VERIFY, then reduce to the single
correct role in WP-11.]

Rules: each parent can be matched once; a stack with no pending parent is `unmatched`; a parent not matched
within one second of trace time (or at end of file) is counted as `withoutStack`. Idle-thread samples
(ThreadId 0) are tracked separately because acceptance is judged on busy cores.

## 4. How to measure (acceptance procedure)

On an elevated prompt, on a machine with >= 8 cores, build and run (see the WP report for exact commands):

```
etlstat record --out testbed.etl --seconds 30 --hz 8000 --exe <path>\tachyon-testbed.exe
etlstat stats testbed.etl --image tachyon-testbed.exe --dump-hex 3 --dump-out testbed.hexevents
etlstat stats testbed.etl --image tachyon-testbed.exe --json > testbed-stats.json
```

Acceptance: >= 7,500 samples/s per busy core, > 95% of SampledProfile events with a matched stack, notes
complete. `--dump-hex N` writes the first N payloads of every (provider, opcode, version) as
`event provider=... opcode=... version=... ts=... tid=... pid=... cpu=... data=<hex>` lines; commit that
file under `tests/fixtures/etw/` (WP-12 reuses it) and replay it anywhere with `etlstat replay <file>`.
`tests/fixtures/etw/synthetic_basic.hexevents` is a hand-built file in the same format and is not a capture.

## 5. Measurements (to be filled on Windows)

Machine: _CPU model, cores, Windows build, power plan: TBD_

| Quantity | Target | Measured |
|---|---|---|
| Interval accepted by `TraceSetInformation` (100 ns units) | 1221 | TBD |
| Handle needed for interval call (session / 0) | document | TBD |
| Session GUID accepted (unique / SystemTraceControlGuid) | document | TBD |
| Samples/s per busy core (min over busy CPUs) | >= 7,500 | TBD |
| Samples/s total (all CPUs) | n x 8,190 | TBD |
| Samples/s for tachyon-testbed threads | document | TBD |
| SampledProfile with matched stack | > 95% | TBD |
| CSwitch with matched stack | document | TBD |
| ReadyThread with matched stack | document | TBD |
| Unmatched StackWalk events | document | TBD |
| Max stack depth (frames) | document | TBD |
| Thread/SetName (72) present | document | TBD |
| `EventsLost` / `BuffersLost` (logfile header) at 8 kHz, 30 s | 0 | TBD |
| `EventsLost` / `LogBuffersLost` (from `record`) | 0 | TBD |
| ETL size for 30 s | document | TBD |
| `DisablePagingExecutive` effect on kernel frames | document | TBD |

Observed event versions and sizes (copy from `etlstat stats` table):

| Provider / opcode | Version | Payload size (min..max) | Matches section 2? |
|---|---|---|---|
| PerfInfo / 46 SampledProfile | TBD | TBD | TBD |
| Thread / 36 CSwitch | TBD | TBD | TBD |
| Thread / 50 ReadyThread | TBD | TBD | TBD |
| Thread / 72 SetName | TBD | TBD | TBD |
| Thread / 1,2,3,4 | TBD | TBD | TBD |
| Process / 1,2,3,4 | TBD | TBD | TBD |
| Image / 2,3,4,10 | TBD | TBD | TBD |
| StackWalk / 32 | TBD | TBD | TBD |
| StackWalk / 34..37 | TBD | TBD | TBD |

## Observed on the GitHub windows-2022 runner (WP-11 CI, 2026-10-25)
- [OBSERVED] `TraceSetInformation(TraceSampledProfileIntervalInfo)` fails with the session handle and succeeds with handle 0.
- [OBSERVED] A unique session name and GUID with EVENT_TRACE_SYSTEM_LOGGER_MODE start without the SystemTraceControlGuid fallback.
- [OBSERVED] 2 s system capture at 8 kHz on 4 cores: EventsLost 0, 43,965 samples, 15 MB ETL, 250 KB .tachyon. A 5 s testbed spin: the testbed's busy thread got 41,902 samples (≈ 8,380/s); EventsLost, LogBuffersLost and RealTimeBuffersLost all 0.
- [OBSERVED] Flush via FLUSH + stop + restart: capture gap ≈ 47–54 ms.
