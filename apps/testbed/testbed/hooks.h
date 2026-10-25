// Scenario hooks. No-ops today; the post-WP-50 `--instrument` mode emits Tachyon scopes and frame marks from
// here without touching the scenario bodies.
#pragma once

#include <cstdint>

namespace tachyon::testbed {

// Thread-safe (no-op). Called on the driver thread immediately before a scenario's threads start.
inline void onScenarioBegin(const char* /*scenarioName*/) {}
// Thread-safe (no-op). Called on the driver thread after a scenario's threads have been joined.
inline void onScenarioEnd(const char* /*scenarioName*/) {}
// Thread-safe (no-op). Called by the frames thread at the start of each simulated frame.
inline void onFrameBegin(uint64_t /*frameIndex*/) {}
// Thread-safe (no-op). Called by the frames thread at the end of each simulated frame.
inline void onFrameEnd(uint64_t /*frameIndex*/) {}

}  // namespace tachyon::testbed
