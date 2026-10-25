// Result accumulator shared by the analytic bookkeeping (generator) and the brute-force reference. Both fill
// a Tally from independent derivations; Tally::toJson() renders the documented `expected.json` schema.
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tachyon::tracegen {

// FunctionKey (Part 3.7): (module, function start RVA); unmapped frames use (kInvalidIdx, absolute address);
// instrumentation scopes use the scope id.
struct FnKey {
  uint8_t kind = 0;  // 0 mapped function, 1 unmapped address, 2 instrumentation scope
  uint32_t module = 0;
  uint64_t startRva = 0;
  std::string scope;
  auto operator<=>(const FnKey&) const = default;
};

struct FnStat {
  uint64_t inclTicks = 0, exclTicks = 0, inclSamples = 0, exclSamples = 0;
  bool operator==(const FnStat&) const = default;
};

enum StateIdx : uint8_t { kStateRunning = 0, kStateReady = 1, kStateWaiting = 2, kStateUnknown = 3 };

struct IntervalLog {
  uint8_t state = 0;  // StateIdx
  uint64_t start = 0, end = 0;
  std::vector<std::string> blocking;    // function names, leaf first
  std::vector<std::string> unblocking;  // function names, leaf first
  uint32_t wokeBy = 0;
};

struct ThreadTally {
  std::string name;
  uint64_t samples = 0, zeroWeight = 0, clipped = 0, sampleTicks = 0;
  uint64_t switchesOut = 0, switchesIn = 0, readyEvents = 0;
  std::array<uint64_t, 4> stateTicks{};
  std::map<FnKey, FnStat> fns;
  std::map<FnKey, FnStat> blended;
  std::vector<IntervalLog> intervals;
};

struct Placement {
  uint64_t samples = 0, ticks = 0;
};
struct PlacementKey {
  std::string scope;
  bool matched = false;
  FnKey parent;
  auto operator<=>(const PlacementKey&) const = default;
};

struct FrameThreadTally {
  uint64_t runningTicks = 0, sampleTicks = 0;
};
struct FrameTally {
  uint32_t tid = 0;
  std::string name;
  uint64_t number = 0, start = 0, end = 0;
  std::map<uint32_t, FrameThreadTally> threads;
};

struct FiberSegment {
  uint32_t tid = 0;
  std::string scope;
  uint32_t depth = 0;
  uint64_t scopeBegin = 0, begin = 0, end = 0;
};

struct PlotTally {
  uint64_t count = 0;
  double min = 0, max = 0;
  uint64_t firstTs = 0, lastTs = 0;
};

struct Tally {
  std::string scenario, generator;
  uint64_t seed = 0, clockHz = 0, samplingHz = 0, sampleInterval = 0;
  uint64_t startTs = 0, endTs = 0;
  bool partial = false, blend = false, detail = false;
  uint64_t nSamples = 0, nSwitches = 0, nReady = 0, nScopeEvents = 0, nFiberEvents = 0, nFrameMarks = 0,
           nMarkers = 0, nPlots = 0, nThreadNames = 0;

  struct ModuleEntry {
    std::string name;
    uint64_t base = 0;
  };
  std::vector<ModuleEntry> modules;
  std::map<FnKey, std::string> names;
  std::map<uint32_t, ThreadTally> threads;
  std::map<FnKey, FnStat> fns;      // overall
  std::map<FnKey, FnStat> blended;  // overall, scopes spliced in
  std::map<std::string, std::vector<uint64_t>> scopeDurations;
  std::map<PlacementKey, Placement> placements;
  std::vector<FrameTally> frames;
  std::map<uint64_t, std::vector<FiberSegment>> fibers;
  std::map<std::string, PlotTally> plots;

  // Applies rules 2 and 3 of 3.8 to one stack (root to leaf): inclusive once per distinct key, exclusive to
  // the leaf. Adds into the overall and per-thread maps selected by `useBlended`.
  void addStack(uint32_t tid, const std::vector<FnKey>& rootToLeaf, bool useBlended, uint64_t ticks,
                uint64_t samples);

  // Thread-safe: pure function. Renders the expected.json document.
  nlohmann::json toJson() const;
};

// Thread-safe: pure function. Compares two JSON documents: integers/strings exactly, floating point values
// with a relative tolerance of 1e-9. Returns an empty string when equal, else the path and values of the
// first difference.
std::string compareJson(const nlohmann::json& a, const nlohmann::json& b);

}  // namespace tachyon::tracegen
