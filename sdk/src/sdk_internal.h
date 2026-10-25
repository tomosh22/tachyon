// Internal (not installed) declarations shared by SDK sources and tests. WP-51 replaces the bodies in
// sdk_impl.cpp without touching the public header.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reserved id used by Tachyon_LockAcquireBegin when no lock name is supplied.
extern const char kTachyonLockEventId[];

// Test-only: the caller return address recorded by the most recent Begin on the calling thread (or NULL).
// Thread safe: thread-local. Visible from tachyon_sdk (static) only; not exported from the shared library.
const void* Tachyon_Internal_LastBeginCaller(void);
// Test-only: clears the calling thread's recorded state. Thread safe: thread-local.
void Tachyon_Internal_ResetThreadState(void);
// Test-only: color / data of the most recent Begin on this thread (data NUL-terminated, "" for NULL).
uint32_t Tachyon_Internal_LastBeginColor(void);
const char* Tachyon_Internal_LastBeginData(void);

// Test-only runtime control (static library only; not exported from the shared library).
// Waits until the runtime has chosen its sink: returns 1 when streaming to an agent, 2 for the sidecar file,
// 0 when there is no sink (disabled) or on timeout. Starts the runtime if needed.
int Tachyon_Internal_WaitReady(int timeoutMs);
// Blocks until everything recorded so far by finished writes was drained into the sink.
void Tachyon_Internal_Flush(void);
// Runs the exit-time shutdown now (final drain, close sink); safe while other threads are still emitting.
void Tachyon_Internal_Shutdown(void);
// Shuts the runtime down (final drain, close sink) and re-arms lazy creation from the current environment.
// Every other thread that emitted must have exited; the calling thread starts over with a fresh ring.
void Tachyon_Internal_Restart(void);
// Path of the sidecar file the runtime wrote/is writing ("" if none). Valid until the next call on this
// thread.
const char* Tachyon_Internal_SidecarPath(void);
// Records dropped so far on the calling thread / its ring size in bytes (0 before the first event).
uint64_t Tachyon_Internal_ThreadDropCount(void);
uint64_t Tachyon_Internal_RingBytes(void);

#ifdef __cplusplus
}
#endif
