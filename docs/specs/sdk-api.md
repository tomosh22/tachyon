<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.3. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.3 Public SDK API (`sdk/include/tachyon/tachyon.h`), version 1
C ABI. Names mirror the Performance API so engine integration guides translate directly.
```c
#define TACHYON_API_VERSION 0x00010000
#ifndef TACHYON_ENABLED  /* default 1 on Windows/Linux, 0 elsewhere */ #endif
#define TACHYON_MAKE_COLOR(r,g,b) ((uint32_t)(r)<<24 | (uint32_t)(g)<<16 | (uint32_t)(b)<<8 | 0xFF)
#define TACHYON_DEFAULT_COLOR 0xFFFFFFFFu
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
void Tachyon_LockAcquireBegin(const void* lockAddr, const char* lockName);           /* implemented as BeginEvent with data = hex addr, kind flag */
void Tachyon_LockAcquireEnd(const void* lockAddr);
int  Tachyon_IsConnected(void);                                                      /* 1 when an agent is draining */
int  Tachyon_RequestFlush(const char* reason);                                       /* flight recorder trigger; see control protocol */
typedef struct { /* function pointers in the order above */ } TachyonFunctions;
typedef int (*Tachyon_GetAPI_Func)(int version, TachyonFunctions* out);              /* exported as "Tachyon_GetAPI" from tachyon_sdk_shared */
```
C++ wrapper: `tachyon::Scope` RAII type and macros `TACHYON_SCOPE(id)`, `TACHYON_SCOPE_DATA(id,data)`, `TACHYON_SCOPE_COLOR(id,color)`, `TACHYON_FUNCTION()`, `TACHYON_FRAME(name,num)`, `TACHYON_LOCK_SCOPE(lock,name)`. All compile to nothing when `TACHYON_ENABLED == 0`.
Contracts: Begin/End pairs in the same function; `id` pointer identity is used as the interning key (static string requirement); strings UTF-8; maximum `data` length 4096 bytes (truncated); thread-safe; re-entrant from any thread including fibers; no allocation on the hot path after first call per thread.

## Orchestrator clarifications (binding; merged with WP-50, 2026-10-25)
- `TachyonSuppressTailCall` is `struct { uint64_t reserved[3]; }` (24 bytes), size-checked with a C99-compatible negative-array typedef.
- `TachyonFunctions` holds exactly one pointer per function above, in the listed order (16 members). `Tachyon_GetAPI(version, out)` returns 1 and fills `out` only when `version == TACHYON_API_VERSION`; otherwise it returns 0 and zero-fills `out`.
- `TACHYON_ENABLED` defaults to 1 when `_WIN32` or `__linux__` is defined, else 0. Disabled macros expand to `static_assert(true, "")`.
- Lock events: `Tachyon_LockAcquireBegin` is a BeginEvent with `id = lockName` (static) or the reserved id `"Tachyon.LockAcquire"` when NULL, `data = "0x<hex lock address>"`, and color `TACHYON_LOCK_COLOR_SENTINEL` (`0x4C4F43FF`), which is the kind flag analysis uses to recognize lock waits. `TACHYON_LOCK_SCOPE(lock, name)` takes an lvalue and uses its address.
- `tachyon_loader.h`: header-only `int Tachyon_LoadFrom(const char* path, TachyonFunctions* out)`; returns 1 on success, zero-fills on any failure.
- Export control: `TACHYON_SDK_API`; define `TACHYON_SDK_USE_SHARED` on Windows when linking the DLL import library.
- The public API is frozen as of this merge (Part 0.3); later changes bump `TACHYON_API_VERSION`.
