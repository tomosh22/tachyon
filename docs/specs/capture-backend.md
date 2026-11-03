<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.5. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.5 Capture backend interface (`libs/capture/capture_backend.h`)
```cpp
struct CaptureOptions { CaptureMode mode; std::string exe, cwd; std::vector<std::string> args; std::map<std::string,std::string> env; uint32_t pid; uint32_t samplingHz; bool followChildren; std::string childFilter; uint32_t circularMb; std::filesystem::path outPath; bool startImmediately; bool stackProfile, stackCSwitch, stackReady; bool kernelStacks; };
class ICaptureBackend { public:
  virtual Result<void> checkPrivileges() = 0;            // SeSystemProfilePrivilege / CAP_PERFMON
  virtual Result<void> start(const CaptureOptions&) = 0; // returns once the kernel session is live and the target is running
  virtual Result<CaptureResult> stop() = 0;              // stops session, converts, returns {.tachyon path, events_lost, raw path}
  virtual Result<CaptureResult> flush(std::string name, uint32_t seconds) = 0; // circular mode only
  virtual CaptureState state() const = 0;
  virtual void setProgressCallback(std::function<void(CaptureState,float)>) = 0; };
```
Implementations: `WindowsEtwBackend` (WP-11/12), `LinuxPerfBackend` (WP-70), `MockBackend` (WP-10; copies a fixture to `outPath`).

## Orchestrator clarifications (binding; merged with WP-10, 2026-10-25)
Header: `libs/capture/capture_backend.h` (`tachyon::capture`), now orchestrator-owned. Adds `enum class CaptureMode { Launch, Attach, System }`, `enum class CaptureState { Idle, Starting, Capturing, Converting, Done }` (names match protocol states), `struct CaptureResult { std::filesystem::path tracePath; uint64_t eventsLost; std::filesystem::path rawPath; double durationS; }`, and name/parse helpers. `MockBackend` lives in `libs/capture/mock/`.
- Backends may call the progress callback from any thread, including from inside `stop()`/`flush()`; the agent must not hold its state lock across backend calls (fix scheduled in WP-11).

## Clarifications added with WP-70 (2026-11-02, binding)
- `LinuxPerfBackend` (`tachyon::capture_linux_perf`, agent `--backend perf`).
  - System mode uses per-CPU events and needs CAP_PERFMON/CAP_SYS_ADMIN or paranoid ≤ 0.
  - Launch and attach use the same per-CPU plan when it is allowed. Otherwise they use per-task `inherit=1` events per CPU, with all of a CPU's events sharing one ring via PERF_EVENT_IOC_SET_OUTPUT. That costs threads × cpus file descriptors.
- `rawPath` is the spool directory `<out>.perfspool` (session.json, meta.bin, data-NNNNNN.bin).
- A flush writes `<dir>/<name>.tachyon` without stopping the capture. With raw files kept, it also writes `<name>.perfspool`. In circular mode the retention rule of the WP-60 clarification below applies (superseding "consumed segments are discarded").
- stop and flush are mutually exclusive: either one returns Busy while the other is running.
- PERF_RECORD_LOST adds to events_lost. THROTTLE periods do not. They are reported as warnings and under "throttle" in CaptureInfo.
- A launch with startImmediately=false parks the forked child before exec until the second start(). The stop-on-start pipe guarantees that events are enabled before exec (enable_on_exec).
- Kernel frames are written as absolute frames (module kInvalidIdx, address ≥ 0xFFFF800000000000). Modules are created when a sample first lands in a mapping, and `base` follows the ELF rule in trace-format.md.

## Clarifications added with WP-60 (2026-11-03, binding)

- Linux perf circular retention: `flush(name, seconds)` seals the current segment and converts all sealed segments clipped to `[flushTs − seconds, flushTs]`; it then releases only sealed segments with `seconds > 0` and a known `lastTs` older than `flushTs − H`, where H is the largest `seconds` of any flush of the current capture (reset at `start()`). A `seconds == 0` flush releases nothing and does not raise H. Consecutive flushes therefore each hold the full last `seconds` of samples and context switches. The `circularMb` budget still drops the oldest segments regardless of the window. A circular flush's header time range starts at the window start.
- ETW circular flush restarts the session, so each flushed trace's sampled data covers the ETL segment since the previous flush, capped to `seconds` (known limitation).
- Instrumentation in circular captures uses a ring spool: rotating segment files keep the newest bytes within max(circular_mb, `--ring-min-mb` = 64) MiB per process; each segment re-emits the stream header, the StringDefs (prefix cap 8 MiB) and the latest thread names, so each decodes alone. Flushes merge a pinned snapshot of the segments, clipped to the window, with scopes balanced at the window edges (orphan Ends dropped, synthetic Ends at the window end; threads with fiber events are not balanced). Non-circular captures keep the append-only spool.

## Clarifications added with WP-66 (2026-11-03, binding)

- Perf flight-recorder retention: the horizon H of a flush is the largest `seconds` among the flushes started in the last 5 minutes, this one included (`LinuxPerfBackendConfig::horizonMemoryNs`, default 5 min). A flush with `seconds == 0` releases nothing and does not raise H. A sealed segment is released when its `lastTs` is known and `lastTs < flushTs - H`; a segment with `lastTs == 0` is released when a later-sequence segment is released; if no sealed segment has a timestamp, all but the newest are released. The circular budget still drops the oldest segments regardless. A flush never loses data inside its own window (H >= its seconds).
- Instrumentation ring spool: a segment rotates at `max(segmentBytes, prefix + segmentBytes/2)` bytes, so it always holds at least half a segment of payload. Hitting the 8 MiB StringDef prefix cap adds one capture warning per process. When the next segment file cannot be created the spool keeps writing the current one, retries after another `segmentBytes/4` and reports a warning.
