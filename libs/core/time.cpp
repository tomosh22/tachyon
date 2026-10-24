#include "core/time.h"

#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace tachyon::core {

uint64_t nowTicks() {
#if TACHYON_PLATFORM_WINDOWS
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return static_cast<uint64_t>(value.QuadPart);
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

uint64_t ticksFrequency() {
#if TACHYON_PLATFORM_WINDOWS
  static const uint64_t frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
  }();
  return frequency;
#else
  return 1'000'000'000ull;
#endif
}

}  // namespace tachyon::core
