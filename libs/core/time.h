// Trace clock access (Part 1.4): QPC on Windows, CLOCK_MONOTONIC_RAW (ns) on Linux.
#pragma once

#include <cstdint>

namespace tachyon::core {

// Thread-safe. Current trace-clock tick count.
uint64_t nowTicks();

// Thread-safe. Trace-clock frequency in Hz (QPC frequency on Windows, 1e9 on Linux). Cached after first call.
uint64_t ticksFrequency();

// Thread-safe: pure function. Converts ticks at frequencyHz to nanoseconds without 64-bit overflow.
constexpr uint64_t ticksToNs(uint64_t ticks, uint64_t frequencyHz) {
  if (frequencyHz == 1'000'000'000ull) return ticks;
  const uint64_t whole = ticks / frequencyHz;
  const uint64_t rem = ticks % frequencyHz;
  return whole * 1'000'000'000ull + (rem * 1'000'000'000ull) / frequencyHz;
}

// Thread-safe: pure function. Converts nanoseconds to ticks at frequencyHz without 64-bit overflow.
constexpr uint64_t nsToTicks(uint64_t ns, uint64_t frequencyHz) {
  if (frequencyHz == 1'000'000'000ull) return ns;
  const uint64_t whole = ns / 1'000'000'000ull;
  const uint64_t rem = ns % 1'000'000'000ull;
  return whole * frequencyHz + (rem * frequencyHz) / 1'000'000'000ull;
}

// Thread-safe: pure function. Ticks to seconds as double, for UI edges only.
constexpr double ticksToSeconds(uint64_t ticks, uint64_t frequencyHz) {
  return static_cast<double>(ticks) / static_cast<double>(frequencyHz);
}

}  // namespace tachyon::core
