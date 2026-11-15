<!-- Orchestrator-owned. Part 3.5, kept in sync with libs/capture/capture_backend.h. Changes only via orchestrator. -->

### 3.5 Capture backend interface (`libs/capture/capture_backend.h`, namespace `tachyon::capture`)
```cpp
enum class CaptureMode  { Launch, Attach, System };
enum class CaptureState { Idle, Starting, Capturing, Converting, Done };      // names match the protocol "state" values (idle, starting, ...)
struct ReservedOutput { uint64_t volume = 0, index = 0; };                    // identity (st_dev/st_ino; volume serial/file index) of a file the caller reserved
struct CaptureOptions {
  CaptureMode mode = System; std::string exe, cwd; std::vector<std::string> args; std::map<std::string,std::string> env;
  uint32_t pid = 0; uint32_t samplingHz = 8000; bool followChildren = false; std::string childFilter; uint32_t circularMb = 0;
  std::filesystem::path outPath; bool startImmediately = true;
  bool stackProfile = true, stackCSwitch = true, stackReady = true; bool kernelStacks = true;
  bool live = false;                                   // WP-65a: also decode events as they arrive and hand them to the ILiveSink (live.md)
  std::optional<ReservedOutput> reservedOutput;        // WP-66b: the final trace (not a flush) is written into exactly this file
};
struct CaptureResult { std::filesystem::path tracePath; uint64_t eventsLost = 0; std::filesystem::path rawPath; double durationS = 0.0;
                       std::vector<std::string> warnings; };                  // problems that did not fail the capture (WP-70b); the agent adds them to capture_done.warnings
class ICaptureBackend { public:
  virtual Result<void> checkPrivileges() = 0;            // SeSystemProfilePrivilege / CAP_PERFMON
  virtual Result<void> start(const CaptureOptions&) = 0; // returns once the kernel session is live and the target is running
  virtual Result<CaptureResult> stop() = 0;              // stops session, converts, returns {.tachyon path, events_lost, raw path, duration, warnings}
  virtual Result<CaptureResult> flush(std::string name, uint32_t seconds) = 0; // circular mode only
  virtual CaptureState state() const = 0;
  virtual void setProgressCallback(std::function<void(CaptureState,float)>) = 0;
  virtual void setLiveSink(std::shared_ptr<ILiveSink>) {}   // WP-65a; call before start()
  virtual bool supportsLive() const { return false; } };    // a live start on a backend without it fails with Unsupported
```
Helpers: `captureModeName` / `captureStateName` / `parseCaptureMode` / `parseCaptureState`. `ILiveSink` and the `Live*` event structs are defined in the same header and specified in live.md. Implementations: `WindowsEtwBackend` (`libs/capture/windows_etw`), `LinuxPerfBackend` (`libs/capture/linux_perf`, namespace `tachyon::capture_linux_perf`), `MockBackend` (`libs/capture/mock`; copies a fixture to `outPath`).

Common rules:
- Backends may call the progress callback from any thread, including from inside `stop()`/`flush()`; the agent does not hold its state lock across backend calls.
- stop and flush are mutually exclusive in the perf backend: either one returns Busy while the other is running.
- Output files (WP-66b): the final trace is opened through `TraceWriter::open` (never follows a link at the final path, refuses a non-regular file and an existing file with more than one hard link). `CaptureOptions::reservedOutput`, when set, is copied into the perf/ETW `ConvertOptions::reservedOutput`; the final trace is then written through `TraceWriter::open(path, header, reserved)` (the existing file with that device/inode, nlink 1). Flushes keep the by-name open with the same refusals.

## Linux perf backend (`LinuxPerfBackend`, agent `--backend perf`; WP-70, WP-71)
- System mode uses per-CPU events and needs CAP_PERFMON/CAP_SYS_ADMIN or `perf_event_paranoid` ≤ 0. Launch and attach use the same per-CPU plan when it is allowed. Otherwise they use per-task `inherit=1` events per CPU, with all of a CPU's events sharing one ring via PERF_EVENT_IOC_SET_OUTPUT. That costs threads × cpus file descriptors (checked against RLIMIT_NOFILE up front; the soft limit is raised to the hard one and the error names the count).
- `rawPath` is the spool directory `<out>.perfspool` (session.json, meta.bin, data-NNNNNN.bin), kept unless `--perf-delete-raw`. A flush writes `<dir>/<name>.tachyon` without stopping the capture; with raw files kept it also writes `<name>.perfspool`.
- PERF_RECORD_LOST adds to `events_lost`. THROTTLE periods do not: they are reported as warnings and under `throttle` in CaptureInfo.
- A launch with `startImmediately=false` parks the forked child before exec until the second `start()`. The stop-on-start pipe guarantees that events are enabled before exec (enable_on_exec).
- Kernel frames are written as absolute frames (module kInvalidIdx, address ≥ 0xFFFF800000000000). Modules are created when a sample first lands in a mapping, and `base` follows the ELF rule in trace-format.md.
- Scheduler events (WP-71): the sched tracepoint path (needs tracefs) records switches with wait reasons and blocking stacks and wakeups as ReadyRecs; the fallback (per-thread plans, or no tracefs) records PERF_RECORD_SWITCH only. CaptureInfo `sched.path` records which, and a warning explains what is missing. Encoding of the records: attribution-rules.md, "Wait reasons".
- Exiting tasks (WP-71b, 2026-11-15): switch records (PERF_RECORD_SWITCH / SWITCH_CPU_WIDE) of a task the kernel has already unhashed (an exiting task's final switch) carry tid −1.
  - CPU_WIDE: the switch-out is attributed to the task that last got that cpu. A switch-in record that names the previous task as −1 repeats the switch-out just seen and is not counted.
  - Per-task: the converter tracks, per cpu, the target task most recently switched in. An unnamed switch-out on that cpu is attributed to that task, unless the tid had already exited when it was switched in. Any switch-out on the cpu, and any PERF_RECORD_LOST, drops the tracking.
  - Records that still cannot be attributed are dropped and counted in CaptureInfo `convert_stats.sched_unattributed_switches`. The count becomes a capture warning only when it is at least 100, or at least 0.1 % of all switch records. Below that it is informational and shown in the viewer's Trace Info.
- Live source (WP-65a): decodes the same rings as they are drained; chunks that waited more than 2 s (`LiveSourceConfig::maxLag`) are dropped from the live view only and reported through `onDropped`.

## Windows ETW backend (`WindowsEtwBackend`, agent `--backend etw`; WP-11, WP-12, WP-13, WP-65w)
- Session recipe and event layouts: `docs/etw-notes.md`. The agent keeps the raw `<out>.etl` next to the trace unless `--etw-delete-raw`.
- Only the ETW backend implements `ITargetWatch` (`libs/capture/windows_etw/target_watch.h`: `launchPending()`, `targetExited()`, `rootPid()`), which lets the agent finish a capture when the target exits, defer a launch (`startImmediately=false`) and check `inst_hello` pids.
- Circular flush restarts the session, so each flushed trace's sampled data covers the ETL segment since the previous flush, capped to `seconds` (known limitation). The capture gap of a restart was ≈ 47–54 ms on the CI runner.
- Live source (WP-65w): see live.md.

## Flight recorder retention (WP-60, WP-66)
- `flush(name, seconds)` is valid only for circular captures (`circularMb > 0`). A circular flush's header time range starts at the window start.
- Perf: `flush` seals the current segment and converts all sealed segments clipped to `[flushTs − seconds, flushTs]`. It then releases sealed segments whose `lastTs` is known and `< flushTs − H`. H, the horizon, is the largest `seconds` among the flushes started in the last 5 minutes, this one included (`LinuxPerfBackendConfig::horizonMemoryNs`, default 5 min). A flush with `seconds == 0` releases nothing and does not raise H. A segment with `lastTs == 0` is released when a later-sequence segment is released; if no sealed segment has a timestamp, all but the newest are released. The `circularMb` budget still drops the oldest segments regardless of the window. A flush never loses data inside its own window (H ≥ its seconds), so consecutive flushes each hold the full last `seconds` of samples and context switches. This supersedes the WP-70 rule "consumed segments are discarded".
- Instrumentation in circular captures uses a ring spool (`libs/inst/spool.h`): rotating segment files keep the newest bytes within max(`circular_mb`, `--ring-min-mb` = 64) MiB per process. A segment rotates at `max(segmentBytes, prefix + segmentBytes/2)` bytes, so it always holds at least half a segment of payload; each segment re-emits the stream header, the StringDefs (prefix cap 8 MiB; hitting it adds one capture warning per process) and the latest thread names, so each decodes alone. When the next segment file cannot be created the spool keeps writing the current one, retries after another `segmentBytes/4` and reports a warning. Flushes merge a pinned snapshot of the segments, clipped to the window, with scopes balanced at the window edges (orphan Ends dropped, synthetic Ends at the window end; threads with fiber events are not balanced). Non-circular captures keep the append-only spool.

## Changelog
- 2026-10-25 (WP-10): header ownership; `CaptureMode`, `CaptureState`, `CaptureResult`; progress-callback threading rule.
- 2026-11-02 (WP-70): Linux perf plans, spool, flush, LOST/THROTTLE, parked launch, kernel frames, ELF base.
- 2026-11-03 (WP-60, WP-66): circular retention, horizon rule, ring spool rules.
- 2026-11-08 (WP-66b, WP-65a): `reservedOutput`, output safety, `live`, `ILiveSink`, `setLiveSink`, `supportsLive`, `CaptureResult::warnings` (WP-70b).
- 2026-11-15 (WP-71b): exiting-task switch attribution and `sched_unattributed_switches`.
- 2026-11-15: clarifications folded in and grouped per backend; struct listing brought up to date.
