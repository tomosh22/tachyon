// Scenario hooks. With `--instrument` (setInstrumentation(true)) they emit Tachyon SDK events through the
// public SDK header only: scopes around the work functions, thread names, frame marks, markers, plots, lock
// waits and fiber switches. Without it every hook is a cheap no-op and the SDK runtime is never started (so
// no sidecar file is created).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "core/platform.h"
#include "tachyon/tachyon.h"

namespace tachyon::testbed {

// Thread-safe. The instrumentation switch, a function-local atomic so the header stays self-contained.
inline std::atomic<bool>& instrumentationFlag() {
  static std::atomic<bool> flag{false};
  return flag;
}
// Thread-safe. Turns SDK event emission on (`--instrument`) or off.
inline void setInstrumentation(bool on) {
  instrumentationFlag().store(on, std::memory_order_relaxed);
}
// Thread-safe.
inline bool instrumentationEnabled() {
  return instrumentationFlag().load(std::memory_order_relaxed);
}

// Scope that begins/ends a Tachyon event when instrumentation is on. The constructor is force-inlined so the
// SDK captures a return address inside the caller, like tachyon::Scope. `id` must be a string literal.
class InstScope {
 public:
  TACHYON_FORCEINLINE explicit InstScope(const char* id) : m_on(instrumentationEnabled()) {
    if (m_on) Tachyon_BeginEvent(id, nullptr, TACHYON_DEFAULT_COLOR);
  }
  TACHYON_FORCEINLINE ~InstScope() {
    if (m_on) (void)Tachyon_EndEvent();
  }
  InstScope(const InstScope&) = delete;
  InstScope& operator=(const InstScope&) = delete;

 private:
  bool m_on;
};

// Lock-wait scope (Tachyon_LockAcquireBegin / End) when instrumentation is on.
class InstLockScope {
 public:
  TACHYON_FORCEINLINE InstLockScope(const void* lock, const char* name)
      : m_lock(lock), m_on(instrumentationEnabled()) {
    if (m_on) Tachyon_LockAcquireBegin(m_lock, name);
  }
  TACHYON_FORCEINLINE ~InstLockScope() {
    if (m_on) Tachyon_LockAcquireEnd(m_lock);
  }
  InstLockScope(const InstLockScope&) = delete;
  InstLockScope& operator=(const InstLockScope&) = delete;

 private:
  const void* m_lock;
  bool m_on;
};

}  // namespace tachyon::testbed

#define TB_CAT2(a, b) a##b
#define TB_CAT(a, b) TB_CAT2(a, b)
// Scope for the rest of the enclosing block; `id` must be a string literal.
#define TB_SCOPE(id) ::tachyon::testbed::InstScope TB_CAT(tbScope_, __LINE__)(id)

namespace tachyon::testbed {

// Thread-safe. Called on the driver thread immediately before a scenario's threads start.
inline void onScenarioBegin(const char* scenarioName) {
  if (instrumentationEnabled())
    Tachyon_Marker("testbed.scenario.begin", scenarioName, TACHYON_MAKE_COLOR(80, 200, 120));
}
// Thread-safe. Called on the driver thread after a scenario's threads have been joined.
inline void onScenarioEnd(const char* scenarioName) {
  if (instrumentationEnabled())
    Tachyon_Marker("testbed.scenario.end", scenarioName, TACHYON_MAKE_COLOR(200, 120, 80));
}
// Thread-safe. Called by the frames thread at the start of each simulated frame.
inline void onFrameBegin(uint64_t /*frameIndex*/) {}
// Thread-safe. Called by the frames thread at the end of each simulated frame.
inline void onFrameEnd(uint64_t frameIndex) {
  if (instrumentationEnabled()) Tachyon_FrameMark("testbed.frame", frameIndex);
}
// Thread-safe. Called by the frames thread with the length of the frame that just ended.
inline void onFrameTime(double milliseconds) {
  if (instrumentationEnabled()) Tachyon_Plot("testbed.frame_ms", milliseconds);
}
// Thread-safe. Called by the frames thread when a frame contains the hitch.
inline void onHitch(uint64_t frameIndex) {
  if (instrumentationEnabled()) {
    const std::string text = "frame " + std::to_string(frameIndex);
    Tachyon_Marker("testbed.hitch", text.c_str(), TACHYON_MAKE_COLOR(255, 64, 64));
  }
}
// Thread-safe. Called on a scenario thread right after it was named (also set at the OS level).
inline void onThreadNamed(const std::string& name) {
  if (instrumentationEnabled()) Tachyon_SetCurrentThreadName(name.c_str());
}
// Thread-safe. Fiber lifecycle and switches (the fibers scenario).
inline void onFiberRegister(uint64_t fiberId) {
  if (instrumentationEnabled()) Tachyon_RegisterFiber(fiberId);
}
inline void onFiberUnregister(uint64_t fiberId) {
  if (instrumentationEnabled()) Tachyon_UnregisterFiber(fiberId);
}
inline void onFiberSwitchBegin(uint64_t current, uint64_t next) {
  if (instrumentationEnabled()) Tachyon_BeginFiberSwitch(current, next);
}
inline void onFiberSwitchEnd(uint64_t fiberId) {
  if (instrumentationEnabled()) Tachyon_EndFiberSwitch(fiberId);
}

}  // namespace tachyon::testbed
