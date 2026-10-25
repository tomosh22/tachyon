// Platform shims: rdtsc, thread identity/naming, and fibers (Windows fibers / ucontext).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/platform.h"
#include "core/time.h"

#if TACHYON_PLATFORM_WINDOWS
#include <intrin.h>
#elif TACHYON_PLATFORM_LINUX
#if TACHYON_ARCH_X64
#include <x86intrin.h>
#endif
#endif

namespace tachyon::testbed {

// Thread-safe. Raw time-stamp counter (falls back to the trace clock on non-x64).
inline uint64_t readTsc() {
#if TACHYON_ARCH_X64
  return __rdtsc();
#else
  return core::nowTicks();
#endif
}

// Thread-safe. OS process id.
uint64_t currentProcessId();

// Thread-safe. OS thread id of the calling thread (GetCurrentThreadId / gettid).
uint64_t currentThreadId();

// Thread-safe. Names the calling thread (SetThreadDescription / pthread_setname_np; Linux truncates to 15
// chars).
void setCurrentThreadName(const std::string& name);

// Thread-safe. Platform name used in the JSON ("windows" or "linux").
const char* platformName();

// Not thread-safe; owned and used by exactly one thread. A set of cooperative fibers sharing one thread:
// the owning thread resume()s a fiber and the fiber yield()s back.
class FiberGroup {
 public:
  using Entry = void (*)(void*);

  FiberGroup();
  ~FiberGroup();
  FiberGroup(const FiberGroup&) = delete;
  FiberGroup& operator=(const FiberGroup&) = delete;

  // True when the group was set up successfully (Windows: thread converted to a fiber).
  bool ok() const;
  // Creates a fiber that will run entry(arg) on first resume. Entry must never return. Returns its index or
  // -1.
  int add(Entry entry, void* arg);
  // Called from the owning thread (not from a fiber): switches to fiber `index` until it yields.
  void resume(int index);
  // Called from inside a fiber: switches back to the thread's main context.
  void yield();

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace tachyon::testbed
