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
