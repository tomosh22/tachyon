<!-- Orchestrator-owned. Part 3.3, kept in sync with sdk/include/tachyon/tachyon.h and tachyon_loader.h. Changes only via orchestrator. -->

### 3.3 Public SDK API (`sdk/include/tachyon/tachyon.h`), version 1
C ABI, C99 / C++17 compatible, self-contained. Names mirror the Performance API so engine integration guides translate directly. The API is frozen (Part 0.3): later changes bump `TACHYON_API_VERSION`.
```c
#define TACHYON_API_VERSION 0x00010000
#ifndef TACHYON_ENABLED  /* default 1 when _WIN32 or __linux__ is defined, else 0 */ #endif
#define TACHYON_MAKE_COLOR(r,g,b) (((uint32_t)(r)<<24) | ((uint32_t)(g)<<16) | ((uint32_t)(b)<<8) | 0xFFu)   /* 0xRRGGBBAA */
#define TACHYON_DEFAULT_COLOR 0xFFFFFFFFu
#define TACHYON_LOCK_COLOR_SENTINEL 0x4C4F43FFu                                      /* reserved: marks a lock-acquire wait */
typedef struct TachyonSuppressTailCall { uint64_t reserved[3]; } TachyonSuppressTailCall;   /* 24 bytes, size-checked */
void Tachyon_SetCurrentThreadName(const char* utf8);
void Tachyon_SetCurrentThreadName_N(const char* utf8, uint16_t len);
void Tachyon_BeginEvent(const char* id, const char* data, uint32_t color);           /* id must be a static string; data may be dynamic or NULL */
void Tachyon_BeginEvent_N(const char* id, uint16_t idLen, const char* data, uint16_t dataLen, uint32_t color);
TachyonSuppressTailCall Tachyon_EndEvent(void);                                      /* returns 24-byte struct to defeat tail calls */
void Tachyon_RegisterFiber(uint64_t fiberId);
void Tachyon_UnregisterFiber(uint64_t fiberId);
void Tachyon_BeginFiberSwitch(uint64_t currentFiber, uint64_t nextFiber);
void Tachyon_EndFiberSwitch(uint64_t fiberId);
/* Tachyon-specific additions */
void Tachyon_FrameMark(const char* name, uint64_t frameNumber);
void Tachyon_Marker(const char* name, const char* data, uint32_t color);
void Tachyon_Plot(const char* name, double value);
void Tachyon_LockAcquireBegin(const void* lockAddr, const char* lockName);           /* BeginEvent, id = lockName or "Tachyon.LockAcquire", data = "0x<hex addr>", color = sentinel */
void Tachyon_LockAcquireEnd(const void* lockAddr);
int  Tachyon_IsConnected(void);                                                      /* 1 when an agent is draining */
int  Tachyon_RequestFlush(const char* reason);                                       /* flight recorder trigger; 1 if a flush was requested, 0 otherwise; see control-protocol.md */
typedef struct TachyonFunctions { /* exactly one function pointer per function above, in the listed order (16 members) */ } TachyonFunctions;
typedef int (*Tachyon_GetAPI_Func)(int version, TachyonFunctions* out);              /* exported as "Tachyon_GetAPI" from tachyon_sdk_shared */
int Tachyon_GetAPI(int version, TachyonFunctions* out);                              /* 1 and fills `out` only when version == TACHYON_API_VERSION, else 0 and zero-fills */
```
C++ wrapper: `tachyon::Scope` RAII type (force-inlined so the captured return address lies in the user's function), `tachyon::LockScope` (LockAcquireBegin/End), and macros `TACHYON_SCOPE(id)`, `TACHYON_SCOPE_DATA(id,data)`, `TACHYON_SCOPE_COLOR(id,color)`, `TACHYON_FUNCTION()`, `TACHYON_FRAME(name,num)`, `TACHYON_LOCK_SCOPE(lock,name)` (takes an lvalue and uses its address). With `TACHYON_ENABLED == 0` the macros expand to `static_assert(true, "")` and `Scope`/`LockScope` are no-op types.

`sdk/include/tachyon/tachyon_loader.h`: header-only `int Tachyon_LoadFrom(const char* path, TachyonFunctions* out)` (dlopen/LoadLibrary + `Tachyon_GetAPI`); returns 1 on success, zero-fills `out` on any failure; the library is never unloaded on success.

Export control: `TACHYON_SDK_API` (`__declspec(dllexport)` with `TACHYON_SDK_BUILD_SHARED`, `dllimport` with `TACHYON_SDK_USE_SHARED` on Windows when linking the DLL import library; default visibility on GCC/Clang).

Contracts: Begin/End pairs in the same function; `id` pointer identity is used as the interning key (static string requirement); strings UTF-8; maximum `data` length 4096 bytes (truncated); thread-safe; re-entrant from any thread including fibers; no allocation on the hot path after first call per thread. Lock waits are recognised by the colour sentinel (attribution-rules.md, locks).

Runtime environment (implemented in `sdk/src`, see inst-wire.md): `TACHYON_AGENT_PIPE` (agent address), `TACHYON_SIDECAR` (sidecar path or `off`), `TACHYON_DISABLE`, `TACHYON_RING_KB`. `Tachyon_RequestFlush` performs one synchronous control-channel round trip on the calling thread (after letting the flush thread drain the rings, at most 50 ms) and never runs on the recording hot path.

Measured overhead (WP-51, CI runner): Begin+End connected 46.9 ns Linux / 68 ns Windows (binding gate 80 ns; the 40 ns plan target is dominated by two clock reads), disabled at runtime 4.7 / 16 ns (gate 20 ns).

## Changelog
- 2026-10-25 (WP-50): `TachyonSuppressTailCall`, `TachyonFunctions` layout and `Tachyon_GetAPI` semantics, `TACHYON_ENABLED` default, lock event encoding and sentinel, loader, export control, API freeze.
- 2026-11-03 (WP-60): `Tachyon_RequestFlush` semantics.
- 2026-11-15: clarifications folded in; `LockScope`, runtime environment variables and measured overhead added.
