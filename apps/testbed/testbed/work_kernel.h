// The CPU burn kernel shared by every scenario and by calibration.
#pragma once

#include <cstdint>

#include "core/platform.h"
#include "testbed/platform.h"

namespace tachyon::testbed {

// Thread-safe. Publishes a work result through a per-thread volatile so that work functions are never
// considered side-effect free: the compiler may then neither delete them nor move them across the surrounding
// rdtsc reads.
inline void publishResult(uint64_t value) {
  static thread_local volatile uint64_t t_sink;
  t_sink = value;
  static_cast<void>(t_sink);
}

// Thread-safe: pure function. Runs `iterations` of a dependent xorshift chain (cannot be folded by the
// compiler). `Salt` only perturbs the initial state so every caller gets distinct machine code (defeats
// identical-code folding) without changing the per-iteration cost.
template <uint64_t Salt>
TACHYON_FORCEINLINE uint64_t burn(uint64_t x, uint64_t iterations) {
  x ^= Salt;
  for (uint64_t i = 0; i < iterations; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  publishResult(x);
  return x;
}

// Thread-safe: pure function. Burns until the TSC reaches `deadlineTsc`; granularity is a few hundred
// nanoseconds.
template <uint64_t Salt>
TACHYON_FORCEINLINE uint64_t burnUntil(uint64_t deadlineTsc, uint64_t state) {
  do {
    state = burn<Salt>(state, 128);
  } while (readTsc() < deadlineTsc);
  return state;
}

}  // namespace tachyon::testbed
