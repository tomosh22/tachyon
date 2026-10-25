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

#ifdef __cplusplus
}
#endif
