<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.2. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.2 In-memory model (`libs/model/model.h`)
```cpp
struct Module { uint32_t pid; uint64_t base, size; std::string path; ModuleId id; uint64_t load_ts, unload_ts; SymbolState state; };
struct Thread { uint32_t tid, pid; std::string name; uint64_t first_ts, last_ts; bool is_target; ThreadRole role; };
struct Frame  { ModuleIdx module; uint64_t addr_or_rva; };               // symbolication attaches FunctionIdx lazily (symbols lib)
struct CallstackNode { CallstackIdx parent; FrameIdx frame; };            // nodes[0] = root
struct SampleColumns { std::vector<uint64_t> ts; std::vector<CallstackIdx> callstack; std::vector<uint16_t> cpu; std::vector<uint16_t> flags; }; // per thread, sorted by ts
struct ThreadStateInterval { uint64_t start, end; ThreadState state; uint8_t wait_reason; uint32_t cpu; CallstackIdx blocking_stack; CallstackIdx unblocking_stack; uint32_t woke_by_tid; uint64_t ready_ts; };
enum class ThreadState : uint8_t { Running, Ready, Waiting, Unknown };
struct ScopeInstance { uint64_t begin, end; StringIdx id; StringIdx data; uint32_t color; uint16_t depth; uint64_t caller_addr; };
struct TraceModel {
  FileHeader header; std::vector<Process>; std::vector<Thread>; std::vector<Module>; std::vector<Frame> frames; std::vector<CallstackNode> nodes;
  StringTable strings;
  std::unordered_map<uint32_t, ThreadIdx> tidToThread;
  std::vector<SampleColumns> samplesByThread;                 // index = ThreadIdx
  std::vector<std::vector<ThreadStateInterval>> statesByThread;
  std::vector<std::vector<ScopeInstance>> scopesByThread;     // sorted by begin, with depth
  std::vector<FiberSwitch> fiberSwitches; std::vector<FrameMark> frameMarks; std::vector<Marker> markers; std::vector<PlotSeries> plots;
  uint64_t sampleIntervalTicks;                               // from CaptureInfo sampling_hz
};
```
Memory budget: resident size ≤ 3× the compressed `.tachyon` size for a 10-minute 8 kHz capture. Callstack node = 8 bytes. Sample = 16 bytes in memory (per-thread `SampleColumns`; tid implicit) and 20 bytes on disk (3.1 SoA columns). [Corrected 2026-10-25 by orchestrator; was "18 bytes", which matched neither layout.]
