// Calibration: TSC frequency and burn-kernel speed, plus the pure loop-count arithmetic that turns a
// requested split into iteration counts.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace tachyon::testbed {

struct Calibration {
  uint64_t traceFrequencyHz = 0;  // core::ticksFrequency()
  uint64_t tscHz = 0;             // rdtsc ticks per second
  double itersPerTsc = 0.0;       // burn-kernel iterations per rdtsc tick (best of several trials)

  // Thread-safe: pure function.
  double itersPerSecond() const { return itersPerTsc * static_cast<double>(tscHz); }
};

// Thread-safe. Measures the TSC frequency over `tscWindowSeconds` against the trace clock, then the
// burn-kernel speed (best of three trials of about `trialSeconds` each). Blocks for roughly the sum of those
// times.
Calibration calibrate(double tscWindowSeconds = 0.05, double trialSeconds = 0.02);

// Thread-safe: pure function.
uint64_t secondsToTsc(const Calibration& cal, double seconds);
// Thread-safe: pure function.
double tscToSeconds(const Calibration& cal, uint64_t tsc);

// Thread-safe: pure function. Iteration counts per share for one cycle of `cycleTsc` TSC ticks:
// round(share * cycleTsc * itersPerTsc). Shares are expected to sum to 1.
std::vector<uint64_t> loopCountsForShares(std::span<const double> shares, uint64_t cycleTsc,
                                          double itersPerTsc);

}  // namespace tachyon::testbed
