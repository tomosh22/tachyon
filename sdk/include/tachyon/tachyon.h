/*
 * Tachyon profiler public SDK, API version 1 (C99 / C++17 compatible, self-contained).
 *
 * Contracts (mirroring the Performance API):
 *  - Every Begin must be matched by an End on the same thread, in the same function. The caller's return
 *    address is captured by Begin and used by analysis to splice the scope above the matching frame.
 *  - `id` is interned by pointer identity: it MUST be a string with static storage duration (a literal).
 *    `data` may be dynamic or NULL; it is copied. Maximum data length is 4096 bytes; longer data is
 * truncated.
 *  - All strings are UTF-8. The *_N variants take explicit byte lengths and need no terminator.
 *  - Every function is thread-safe, re-entrant from any thread including fibers, and performs no allocation
 *    on the hot path after the first call on a given thread.
 *  - Tachyon_EndEvent returns a 24-byte struct purely so the compiler cannot turn the call into a tail call.
 *  - Colors are 0xRRGGBBAA; use TACHYON_MAKE_COLOR or TACHYON_DEFAULT_COLOR.
 *
 * Lock events: Tachyon_LockAcquireBegin is implemented as Tachyon_BeginEvent whose `data` is the lock address
 * formatted as hex (for example "0x7ffd1234abcd"), whose `id` is `lockName` (must be static) or, if NULL, the
 * reserved id "Tachyon.LockAcquire", and whose color is the reserved sentinel TACHYON_LOCK_COLOR_SENTINEL
 * (0x4C4F43FF). Analysis recognises lock waits by that color sentinel. Tachyon_LockAcquireEnd ends the event.
 *
 * The API is frozen: do not rename or reorder anything below.
 */
#ifndef TACHYON_SDK_TACHYON_H
#define TACHYON_SDK_TACHYON_H

#include <stdint.h>

#define TACHYON_API_VERSION 0x00010000

#ifndef TACHYON_ENABLED
#if defined(_WIN32) || defined(__linux__)
#define TACHYON_ENABLED 1
#else
#define TACHYON_ENABLED 0
#endif
#endif

#define TACHYON_MAKE_COLOR(r, g, b) \
  (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | 0xFFu)
#define TACHYON_DEFAULT_COLOR 0xFFFFFFFFu
/* Reserved color marking a lock-acquire wait event (see header comment). */
#define TACHYON_LOCK_COLOR_SENTINEL 0x4C4F43FFu

/* Export/import control. Define TACHYON_SDK_BUILD_SHARED when building the shared library, and
 * TACHYON_SDK_USE_SHARED on Windows when linking against its import library. */
#ifndef TACHYON_SDK_API
#if defined(_WIN32)
#if defined(TACHYON_SDK_BUILD_SHARED)
#define TACHYON_SDK_API __declspec(dllexport)
#elif defined(TACHYON_SDK_USE_SHARED)
#define TACHYON_SDK_API __declspec(dllimport)
#else
#define TACHYON_SDK_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define TACHYON_SDK_API __attribute__((visibility("default")))
#else
#define TACHYON_SDK_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 24 bytes, returned by Tachyon_EndEvent to defeat tail-call optimisation. */
typedef struct TachyonSuppressTailCall {
  uint64_t reserved[3];
} TachyonSuppressTailCall;

/* C99-compatible compile-time size check (negative array size on failure). */
typedef char
    tachyon_static_assert_suppress_tail_call_is_24_bytes[(sizeof(TachyonSuppressTailCall) == 24) ? 1 : -1];

TACHYON_SDK_API void Tachyon_SetCurrentThreadName(const char* utf8);
TACHYON_SDK_API void Tachyon_SetCurrentThreadName_N(const char* utf8, uint16_t len);
/* id must be a static string; data may be dynamic or NULL. */
TACHYON_SDK_API void Tachyon_BeginEvent(const char* id, const char* data, uint32_t color);
TACHYON_SDK_API void Tachyon_BeginEvent_N(const char* id, uint16_t idLen, const char* data, uint16_t dataLen,
                                          uint32_t color);
TACHYON_SDK_API TachyonSuppressTailCall Tachyon_EndEvent(void);
TACHYON_SDK_API void Tachyon_RegisterFiber(uint64_t fiberId);
TACHYON_SDK_API void Tachyon_UnregisterFiber(uint64_t fiberId);
TACHYON_SDK_API void Tachyon_BeginFiberSwitch(uint64_t currentFiber, uint64_t nextFiber);
TACHYON_SDK_API void Tachyon_EndFiberSwitch(uint64_t fiberId);
/* Tachyon-specific additions. */
TACHYON_SDK_API void Tachyon_FrameMark(const char* name, uint64_t frameNumber);
TACHYON_SDK_API void Tachyon_Marker(const char* name, const char* data, uint32_t color);
TACHYON_SDK_API void Tachyon_Plot(const char* name, double value);
TACHYON_SDK_API void Tachyon_LockAcquireBegin(const void* lockAddr, const char* lockName);
TACHYON_SDK_API void Tachyon_LockAcquireEnd(const void* lockAddr);
/* Returns 1 when an agent is draining events, else 0. */
TACHYON_SDK_API int Tachyon_IsConnected(void);
/* Flight-recorder trigger. Returns 1 if a flush was requested, 0 otherwise. */
TACHYON_SDK_API int Tachyon_RequestFlush(const char* reason);

/* One function pointer per API function, in the order above. */
typedef struct TachyonFunctions {
  void (*SetCurrentThreadName)(const char* utf8);
  void (*SetCurrentThreadName_N)(const char* utf8, uint16_t len);
  void (*BeginEvent)(const char* id, const char* data, uint32_t color);
  void (*BeginEvent_N)(const char* id, uint16_t idLen, const char* data, uint16_t dataLen, uint32_t color);
  TachyonSuppressTailCall (*EndEvent)(void);
  void (*RegisterFiber)(uint64_t fiberId);
  void (*UnregisterFiber)(uint64_t fiberId);
  void (*BeginFiberSwitch)(uint64_t currentFiber, uint64_t nextFiber);
  void (*EndFiberSwitch)(uint64_t fiberId);
  void (*FrameMark)(const char* name, uint64_t frameNumber);
  void (*Marker)(const char* name, const char* data, uint32_t color);
  void (*Plot)(const char* name, double value);
  void (*LockAcquireBegin)(const void* lockAddr, const char* lockName);
  void (*LockAcquireEnd)(const void* lockAddr);
  int (*IsConnected)(void);
  int (*RequestFlush)(const char* reason);
} TachyonFunctions;

/* Exported as "Tachyon_GetAPI" from tachyon_sdk_shared. Returns 1 and fills `out` when version equals
 * TACHYON_API_VERSION exactly; otherwise returns 0 and zero-fills `out`. */
typedef int (*Tachyon_GetAPI_Func)(int version, TachyonFunctions* out);
TACHYON_SDK_API int Tachyon_GetAPI(int version, TachyonFunctions* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ------------------------------------------------------------------------------------------------------ */
/* C++ wrapper                                                                                             */
/* ------------------------------------------------------------------------------------------------------ */
#ifdef __cplusplus

#if defined(_MSC_VER) && !defined(__clang__)
#define TACHYON_DETAIL_FORCEINLINE __forceinline
#else
#define TACHYON_DETAIL_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace tachyon {

#if TACHYON_ENABLED
/* RAII scope. The constructor is force-inlined so the captured return address lies in the user's function. */
class Scope {
 public:
  TACHYON_DETAIL_FORCEINLINE Scope(const char* id, const char* data = nullptr,
                                   uint32_t color = TACHYON_DEFAULT_COLOR) {
    Tachyon_BeginEvent(id, data, color);
  }
  TACHYON_DETAIL_FORCEINLINE ~Scope() { (void)Tachyon_EndEvent(); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

/* RAII lock-wait scope: LockAcquireBegin on construction, LockAcquireEnd on destruction. */
class LockScope {
 public:
  TACHYON_DETAIL_FORCEINLINE LockScope(const void* lockAddr, const char* name) : m_addr(lockAddr) {
    Tachyon_LockAcquireBegin(lockAddr, name);
  }
  TACHYON_DETAIL_FORCEINLINE ~LockScope() { Tachyon_LockAcquireEnd(m_addr); }
  LockScope(const LockScope&) = delete;
  LockScope& operator=(const LockScope&) = delete;

 private:
  const void* m_addr;
};
#else
/* No-op stand-ins so code naming the types still compiles when disabled. */
class Scope {
 public:
  Scope(const char*, const char* = nullptr, uint32_t = TACHYON_DEFAULT_COLOR) {}
};
class LockScope {
 public:
  LockScope(const void*, const char*) {}
};
#endif

}  // namespace tachyon

#define TACHYON_DETAIL_CAT2(a, b) a##b
#define TACHYON_DETAIL_CAT(a, b) TACHYON_DETAIL_CAT2(a, b)

#if TACHYON_ENABLED
#define TACHYON_SCOPE(id) ::tachyon::Scope TACHYON_DETAIL_CAT(tachyonScope_, __LINE__)(id)
#define TACHYON_SCOPE_DATA(id, data) ::tachyon::Scope TACHYON_DETAIL_CAT(tachyonScope_, __LINE__)(id, data)
#define TACHYON_SCOPE_COLOR(id, color) \
  ::tachyon::Scope TACHYON_DETAIL_CAT(tachyonScope_, __LINE__)(id, nullptr, color)
#define TACHYON_FUNCTION() TACHYON_SCOPE(__FUNCTION__)
#define TACHYON_FRAME(name, num) Tachyon_FrameMark((name), (num))
/* `lock` is an lvalue whose address identifies the lock; `name` should be a static string. */
#define TACHYON_LOCK_SCOPE(lock, name) \
  ::tachyon::LockScope TACHYON_DETAIL_CAT(tachyonLock_, __LINE__)(static_cast<const void*>(&(lock)), (name))
#else
#define TACHYON_SCOPE(id) static_assert(true, "")
#define TACHYON_SCOPE_DATA(id, data) static_assert(true, "")
#define TACHYON_SCOPE_COLOR(id, color) static_assert(true, "")
#define TACHYON_FUNCTION() static_assert(true, "")
#define TACHYON_FRAME(name, num) static_assert(true, "")
#define TACHYON_LOCK_SCOPE(lock, name) static_assert(true, "")
#endif

#endif /* __cplusplus */

#endif /* TACHYON_SDK_TACHYON_H */
