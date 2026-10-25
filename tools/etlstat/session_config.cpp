#include "session_config.h"

#include <algorithm>

namespace tachyon::etlstat {

BufferSettings computeBufferSettings(uint32_t logicalCores) {
  BufferSettings s;
  s.bufferSizeKb = 1024;
  s.minimumBuffers = std::max<uint32_t>(64, 4 * logicalCores);
  s.maximumBuffers = 4 * s.minimumBuffers;
  return s;
}

uint32_t profileIntervalUnits(uint32_t hz) {
  if (hz == 0) hz = 1;
  if (hz >= 8000) return kMinProfileIntervalUnits;
  return std::max<uint32_t>(kMinProfileIntervalUnits, 10'000'000u / hz);
}

double intervalUnitsToHz(uint32_t units) {
  return units == 0 ? 0.0 : 10'000'000.0 / static_cast<double>(units);
}

}  // namespace tachyon::etlstat
