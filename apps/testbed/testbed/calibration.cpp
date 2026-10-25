#include "testbed/calibration.h"

#include <algorithm>
#include <cmath>

#include "core/time.h"
#include "testbed/platform.h"
#include "testbed/work_kernel.h"

namespace tachyon::testbed {

namespace {

TACHYON_NOINLINE uint64_t calibrationKernel(uint64_t state, uint64_t iterations) {
  return burn<0xCA11B8A7ull>(state, iterations);
}

uint64_t measureTscHz(double windowSeconds, uint64_t traceHz) {
  const uint64_t traceStart = core::nowTicks();
  const uint64_t tscStart = readTsc();
  const uint64_t traceTarget =
      traceStart + core::nsToTicks(static_cast<uint64_t>(windowSeconds * 1e9), traceHz);
  uint64_t traceNow = traceStart;
  while ((traceNow = core::nowTicks()) < traceTarget) {
  }
  const uint64_t tscEnd = readTsc();
  const double elapsedSeconds = core::ticksToSeconds(traceNow - traceStart, traceHz);
  return static_cast<uint64_t>(static_cast<double>(tscEnd - tscStart) / elapsedSeconds);
}

}  // namespace

Calibration calibrate(double tscWindowSeconds, double trialSeconds) {
  Calibration cal;
  cal.traceFrequencyHz = core::ticksFrequency();
  cal.tscHz = measureTscHz(tscWindowSeconds, cal.traceFrequencyHz);

  const uint64_t trialTsc = std::max<uint64_t>(secondsToTsc(cal, trialSeconds), 1000);
  uint64_t state = 0x9E3779B97F4A7C15ull;
  uint64_t iterations = 1 << 12;
  // Grow the batch until one run takes at least a trial; keep the fastest rate seen afterwards.
  double best = 0.0;
  int measuredTrials = 0;
  while (measuredTrials < 3) {
    const uint64_t t0 = readTsc();
    state = calibrationKernel(state, iterations);
    const uint64_t dt = std::max<uint64_t>(readTsc() - t0, 1);
    if (dt < trialTsc) {
      const double scale = std::clamp(static_cast<double>(trialTsc) / static_cast<double>(dt), 1.5, 16.0);
      iterations = static_cast<uint64_t>(static_cast<double>(iterations) * scale);
      continue;
    }
    best = std::max(best, static_cast<double>(iterations) / static_cast<double>(dt));
    ++measuredTrials;
  }
  cal.itersPerTsc = best;
  return cal;
}

uint64_t secondsToTsc(const Calibration& cal, double seconds) {
  return static_cast<uint64_t>(seconds * static_cast<double>(cal.tscHz));
}

double tscToSeconds(const Calibration& cal, uint64_t tsc) {
  return static_cast<double>(tsc) / static_cast<double>(cal.tscHz);
}

std::vector<uint64_t> loopCountsForShares(std::span<const double> shares, uint64_t cycleTsc,
                                          double itersPerTsc) {
  std::vector<uint64_t> counts;
  counts.reserve(shares.size());
  for (double share : shares) {
    const double iters = share * static_cast<double>(cycleTsc) * itersPerTsc;
    counts.push_back(iters <= 0.0 ? 0 : static_cast<uint64_t>(std::llround(iters)));
  }
  return counts;
}

}  // namespace tachyon::testbed
