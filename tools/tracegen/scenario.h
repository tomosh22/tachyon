// Resolved scenario model. parseScenario() expands every generator/template in the scenario JSON (generated
// function tables, generated stacks, repeated scopes, ...) so the rest of tracegen only sees plain data.
// All times in this model are ticks relative to the trace start unless stated otherwise.
#pragma once

#include "core/result.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::tracegen {

inline constexpr const char* kGeneratorVersion = "1.0.0";
inline constexpr uint32_t kUnmappedModule = 0xFFFFFFFFu;

struct InlineFrameSpec {
  std::string function;
  std::string file;
  uint32_t line = 0;
};
struct InlineSpec {
  uint64_t rva = 0;
  uint32_t size = 0;
  std::vector<InlineFrameSpec> chain;  // innermost first
};
struct LineSpec {
  uint64_t rva = 0;
  uint32_t size = 0;
  uint32_t line = 0;
};
struct FunctionSpec {
  std::string name;
  uint64_t rva = 0;
  uint32_t size = 0;
  std::string file;
  uint32_t line = 0;
  std::vector<LineSpec> lines;
  std::vector<InlineSpec> inlines;
};
struct ModuleSpec {
  std::string name;
  std::string path;
  uint64_t base = 0;
  uint64_t size = 0;
  uint8_t idKind = 1;  // 1 pdb, 2 gnu build-id
  std::vector<uint8_t> id;
  uint32_t age = 0;
  std::vector<FunctionSpec> functions;  // sorted by rva, non-overlapping
};

// One frame of a synthetic stack. module == kUnmappedModule: `value` is an absolute address and `function` is
// unused; otherwise `value` is the RVA (inside modules[module].functions[function]).
struct StackFrame {
  uint32_t module = kUnmappedModule;
  uint32_t function = 0;
  uint64_t value = 0;
};
struct StackSpec {
  std::string name;
  std::vector<StackFrame> rootToLeaf;
};

struct MixEntry {
  uint32_t stack = 0;   // index into Scenario::stacks
  uint32_t weight = 1;  // relative share
};

enum class SegState : uint8_t { Run = 0, Wait = 1, Ready = 2 };

struct IntervalSpec {
  SegState state = SegState::Run;
  bool toEnd = false;      // no duration given: extends to the end of the trace (last interval only)
  uint64_t duration = 0;   // ticks
  uint8_t waitReason = 0;  // Wait/Ready
  uint8_t waitMode = 0;
  uint8_t priority = 8;
  int32_t blockingStack = -1;  // stack index of the switch-out callstack
  uint64_t readyAfter = 0;     // Wait only: ticks after the interval start, 0 = no ReadyRec
  uint32_t wakerTid = 0;
  int32_t wakerStack = -1;
  int32_t mixOverride = -1;  // Run only: index into ThreadSpec::mixOverrides
};

struct PatternSpec {
  bool present = false;
  uint64_t runLo = 0, runHi = 0;
  uint64_t waitLo = 0, waitHi = 0;
  std::vector<uint32_t> waitReasons;
  std::vector<int32_t> blockingStacks;
  uint32_t readyPct = 0;    // % of waits that get a ReadyRec
  uint32_t preemptPct = 0;  // % of off-intervals that are preemptions (Ready state, no ReadyRec)
  std::vector<uint32_t> wakerTids;
  int32_t wakerStack = -1;
  uint64_t latencyLo = 1, latencyHi = 1;  // ticks between ReadyRec and the next switch-in
  uint32_t priority = 8;
};

struct ExtraSample {
  uint64_t at = 0;  // ticks since trace start
  uint32_t stack = 0;
};

struct FiberSwitch {
  uint64_t at = 0;
  uint64_t gap = 0;
  uint64_t to = 0;
};

struct ThreadSpec {
  uint32_t tid = 0;
  std::string name;
  uint32_t cpu = 0;
  uint64_t startOffset = 0;  // first event time when !fromStart
  bool fromStart = false;    // running at trace start, no switch-in record
  std::vector<MixEntry> mix;
  std::vector<std::vector<MixEntry>> mixOverrides;
  std::vector<IntervalSpec> intervals;
  PatternSpec pattern;
  std::vector<ExtraSample> extraSamples;  // sorted by `at`
};

// A concrete scope instance (already expanded from repeat/children).
struct ScopeInstance {
  uint32_t tid = 0;
  std::string id;
  std::string data;
  uint32_t color = 0;
  uint32_t depth = 0;
  uint64_t begin = 0, end = 0;  // ticks since trace start
  uint32_t callerModule = kUnmappedModule;
  uint32_t callerFunction = 0;
  uint64_t callerAddr = 0;  // absolute address recorded in the ScopeEventRec
  uint64_t fiber = 0;       // fiber the instance belongs to (0 = none), validated against fiber events
};

struct FiberEventInst {
  uint64_t ts = 0;  // ticks since trace start
  uint32_t tid = 0;
  uint8_t kind = 0;
  uint64_t fiber = 0;
  uint64_t other = 0;
};

struct FrameMarkInst {
  uint64_t ts = 0;
  uint32_t tid = 0;
  std::string name;
  uint64_t number = 0;
};
struct MarkerInst {
  uint64_t ts = 0;
  uint32_t tid = 0;
  std::string name, data;
  uint32_t color = 0;
};
struct PlotInst {
  uint64_t ts = 0;
  std::string name;
  double value = 0;
};
struct ThreadRename {
  uint64_t ts = 0;
  uint32_t tid = 0;
  std::string name;
};

struct Scenario {
  std::string name;
  uint64_t seed = 1;
  uint32_t platform = 2;  // 1 windows, 2 linux
  uint64_t clockHz = 10'000'000;
  uint32_t samplingHz = 8000;
  uint64_t sampleInterval = 0;  // clockHz / samplingHz
  uint64_t startTs = 0;         // absolute tick of the trace start
  uint64_t duration = 0;        // ticks
  bool partial = false;         // written without finish()
  uint64_t chunkRecords = 1'000'000;
  uint64_t eventsLost = 0;
  bool detail = false;  // emit per-thread interval lists in the expected JSON
  bool blend = false;   // emit the scope-blended sections in the expected JSON
  uint32_t pid = 0, parentPid = 0;
  std::string processName, commandLine;

  std::vector<ModuleSpec> modules;
  std::vector<StackSpec> stacks;
  std::vector<ThreadSpec> threads;
  std::vector<ScopeInstance> scopes;
  std::vector<FiberEventInst> fiberEvents;
  std::vector<FrameMarkInst> frameMarks;
  std::vector<MarkerInst> markers;
  std::vector<PlotInst> plots;
  std::vector<ThreadRename> renames;
};

// Thread-safe: pure function. Parses and expands a scenario document; `fallbackName` is used when the JSON
// has no "name".
Result<Scenario> parseScenario(const nlohmann::json& doc, std::string_view fallbackName);

// Thread-safe: pure function. Reads and parses a scenario file (name defaults to the file stem).
Result<Scenario> loadScenario(const std::string& path);

// Thread-safe: pure function. Index of the function containing `rva` in `m`, or -1.
int64_t findFunction(const ModuleSpec& m, uint64_t rva);

}  // namespace tachyon::tracegen
