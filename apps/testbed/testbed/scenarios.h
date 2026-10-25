// Scenarios: known-answer workloads. Each scenario runs its own named threads for a planned duration and
// reports the expected and rdtsc-measured split per thread.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "testbed/calibration.h"
#include "testbed/report.h"

// The hot functions keep exact, unmangled, externally visible names: later work packages look them up by name
// in symbolized captures. All are noinline so they appear as distinct stack frames.
extern "C" {
uint64_t hot_a(uint64_t iterations, uint64_t state);
uint64_t hot_b(uint64_t iterations, uint64_t state);
uint64_t hot_c(uint64_t iterations, uint64_t state);
uint64_t spin_rest(uint64_t iterations, uint64_t state);
}

namespace tachyon::testbed {

struct RunConfig {
  double durationS = 5.0;
  double hitchIntervalMs = 2000.0;  // frames scenario: a hitch every this many milliseconds of frame time
};

// Spin scenario shares, in the order hot_a, hot_b, hot_c, spin_rest.
inline constexpr double kSpinShares[4] = {0.40, 0.20, 0.10, 0.30};

// Frames scenario model (all milliseconds).
struct FrameModel {
  double periodMs = 1000.0 / 60.0;
  double updateMs = 3.0;
  double renderMs = 4.0;
  double hitchMs = 50.0;
  double hitchIntervalMs = 2000.0;
};

struct FrameExpectation {
  uint32_t framesPerHitch = 1;  // a hitch occurs on every Nth frame
  double cycleMs = 0.0;         // wall time of one hitch cycle
  double update = 0.0;          // shares of the frames thread's time over one hitch cycle
  double render = 0.0;
  double hitch = 0.0;
  double wait = 0.0;
};

// Lock scenario model (milliseconds): each thread holds the mutex for holdMs, then works outside for
// outsideMs.
struct LockModel {
  double holdMs = 0.5;
  double outsideMs = 0.1;
};

struct LockExpectation {
  double critical = 0.0;
  double outside = 0.0;
  double wait = 0.0;
};

// Thread-safe: pure function. Steady-state expected shares of the frames thread (update + render + hitch +
// wait == 1).
FrameExpectation frameExpectation(const FrameModel& model);

// Thread-safe: pure function. Expected shares for one of two threads strictly alternating on the mutex.
LockExpectation lockExpectation(const LockModel& model);

// Thread-safe: pure function. Scenario names in run order.
std::vector<std::string> scenarioNames();

// Thread-safe: pure function. Splits `totalS` across `names` (the frames scenario gets four times the weight
// of the others so that it can contain several hitches).
std::vector<double> planDurations(const std::vector<std::string>& names, double totalS);

// Thread-safe. Runs the spin workload on the calling thread for `durationS` seconds and returns its split
// (thread name/tid are left for the caller to fill). The calibrated loop counts enforce hot_a 40%, hot_b 20%,
// hot_c 10%, spin_rest 30%; the measured split comes from rdtsc.
ThreadReport spinWorker(const Calibration& cal, double durationS);

// Thread-safe. Runs one scenario by name; spawns and joins its own threads.
Result<ScenarioReport> runScenario(std::string_view name, const RunConfig& config, const Calibration& cal);

// Thread-safe. Runs the named scenarios one after another ("all" mode splits `totalS` with planDurations;
// with more than one scenario and hitchIntervalMs <= 0 the frames hitch interval is scaled to fit its share).
Result<RunReport> runScenarios(const std::vector<std::string>& names, double totalS, double hitchIntervalMs,
                               const Calibration& cal);

}  // namespace tachyon::testbed
