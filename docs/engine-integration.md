# Tachyon engine integration guide

How to wire the Tachyon SDK into a C++ game engine: vendoring, linking, build flags, symbols, and the hooks
that make jobs, fibers, locks, frames, streaming and hitches visible in the viewer. Everything here is
exercised by `sdk/examples/engine_hooks_example.cpp` (a fake engine) and `sdk/examples/dynamic_load_example.cpp`,
both built and run by CI. Code fragments below are lifted from them.

Status markers: anything marked **available after WP-xx** depends on a work package that has not merged yet.
The SDK itself (WP-50, WP-51) and the control protocol / `tachyon-cli` (WP-10) are done.

## 0. Assumptions (open questions from the plan, Part 7)

The Part 7 answers have not arrived. This guide assumes the defaults:

- **Q3, profiling macros:** the engine has no existing profiling macro layer, so this guide introduces the
  `TACHYON_*` macros. Section 5.8 shows how to map an existing `PROFILE_SCOPE`-style layer onto them.
- **Q4, fibers and jobs:** fiber ids are 64-bit values available at switch time, and job names are static
  strings (string literals or other static storage) with the dynamic part of a job (entity id, asset, chunk
  index) passed as `data`.
- The engine is C++17 or later, ships on Windows x64 first and Linux second, and the build can be changed to
  emit symbols and keep frame pointers (sections 3 and 4).

If an assumption is wrong, say so before wiring the hooks: both change the hook shape (section 5.2 and 5.3).

## 1. What you get from the SDK

`sdk/include/tachyon/tachyon.h` is a C99/C++17 header with a C ABI (API version `0x00010000`, frozen). The
runtime behind it (`sdk/src`, WP-51) gives each thread a lock-free ring buffer (64 KiB default) that a
background thread drains to the capture agent, or to a sidecar `.tachyon-inst` file when no agent is
reachable. After the first call on a thread there are no allocations and no locks on the recording path.

Contracts to keep in mind everywhere:

- Every `Begin` has a matching `End` **in the same function** (the RAII macros guarantee this). Begin records
  the caller's return address so analysis can splice the scope into the sampled callstack.
- The scope `id` is interned by **pointer identity**: it must be a string with static storage duration. `data`
  is copied (max 4096 bytes, truncated) and may be dynamic or `NULL`.
- Strings are UTF-8. Every function is thread-safe and re-entrant from any thread, including fibers.
- With `TACHYON_ENABLED == 0` all macros expand to `static_assert(true, "")`, the `tachyon::Scope` types are
  empty stand-ins, and you do not link anything. Direct calls to `Tachyon_*` functions are *not* removed, so
  wrap them (section 5.8 and the `engine::prof` layer in the example).

## 2. Getting the SDK into the engine

### 2.1 Vendor the header

Copy `sdk/include/tachyon/` (two headers, `tachyon.h` and `tachyon_loader.h`) into the engine tree, for
example `engine/third_party/tachyon/include/tachyon/`, and add the `include` directory to the include path.
The header is self-contained and its API is frozen at version 1; updating it is a plain file copy. Record the
Tachyon commit you copied from in a `VENDORED.txt` next to it.

### 2.2 Option A: link statically (recommended default)

Build `tachyon_sdk` (`add_library(tachyon_sdk STATIC ...)` in `sdk/CMakeLists.txt`) and link it into the
executable, or vendor the three sources `sdk/src/sdk_impl.cpp`, `sdk_runtime.cpp`, `sdk_platform.cpp` (they
need C++17 and a thread library) into the engine's build. With CMake from this repository:

```cmake
target_link_libraries(engine PRIVATE tachyon::sdk)            # static, ENABLED by default on Windows/Linux
```

### 2.3 Option B: link the shared library

`tachyon_sdk_shared` exports exactly the C API (on Linux a version script hides everything else). On Windows,
consumers of the DLL's import library must define **`TACHYON_SDK_USE_SHARED`** so the declarations become
`dllimport`; linking `tachyon::sdk_shared` through CMake adds it for you, but a hand-written MSVC project must
set it:

```
cl /DTACHYON_SDK_USE_SHARED ... tachyon_sdk_shared.lib
```

Do not define it when linking the static library, and never define `TACHYON_SDK_BUILD_SHARED` outside the
SDK's own shared-library target. Two caveats apply to the DLL: it must stay loaded until process exit (the
runtime's flush thread runs from it and is stopped on exit), and the first SDK call must not come from
`DllMain` or a static initializer that runs under the loader lock.

### 2.4 Option C: load at run time

`tachyon_loader.h` is a header-only loader (C99/C++17):

```cpp
#include "tachyon/tachyon_loader.h"

TachyonFunctions g_tachyon = {};
bool ok = Tachyon_LoadFrom("libtachyon_sdk_shared.so", &g_tachyon) == 1;   // call from main(), not DllMain
if (ok) g_tachyon.BeginEvent("Example.Work", nullptr, TACHYON_DEFAULT_COLOR);
```

`Tachyon_LoadFrom` returns 1 on success; on any failure (missing file, missing symbol, version mismatch) it
returns 0 and zero-fills the table, so every pointer must be null-checked (see `DynScope` in
`dynamic_load_example.cpp`). On success the library is deliberately never unloaded. Wrap the table in an
`always_inline` RAII class like `tachyon::Scope`, otherwise the return address Begin captures points into your
wrapper instead of the profiled function.

### 2.5 Static or dynamic?

| | Static link (A) | Shared link (B) | Run-time load (C) |
|---|---|---|---|
| Cost per scope | direct call | call through the PLT/IAT, a few ns more | indirect call plus a null check |
| Ship profiler code in release builds | yes, always present (set `TACHYON_DISABLE=1` or `TACHYON_ENABLED=0`) | only if you ship the DLL | no: absent unless the file is next to the exe |
| Several modules (exe + DLLs) | each module that links the static library gets **its own runtime**: avoid | one runtime, shared by all modules | one runtime |
| Failure mode | none | process fails to start if the DLL is missing | profiling silently off |
| Best for | single-executable engines, dev builds | engines with many plugin DLLs | shipping builds where you want "profiler absent unless I drop the DLL in" |

Engines made of several DLLs should use B or C, or link the static library into exactly one module and expose
the engine's own `prof::` wrappers (section 5.8) across module boundaries. Per-module static copies would
each open their own agent connection and rings.

## 3. Build flags

The profiler needs symbols and frame pointers; it cannot recover scopes or callstacks without them.

### 3.1 MSVC (Windows)

```
/Zi        # PDB debug info (use /Z7 only if you do not want a PDB; Tachyon needs a PDB)
/Zo        # richer debug info for optimized code (inlined frames)
/Oy-       # keep frame pointers; MSVC ignores /Oy- on x64 (ETW walks x64 stacks via unwind data), so it only matters for x86
/DEBUG:FULL        # linker: full PDB, not /DEBUG:FASTLINK
/OPT:REF /OPT:ICF  # fine to keep; they do not remove symbols
```

Emit PDBs for every configuration you want to profile, including Release and the shipping-candidate
configuration. `/DEBUG:FASTLINK` PDBs depend on the intermediate object files and break once those are
cleaned: use `/DEBUG:FULL` for anything you archive.

### 3.2 Clang and GCC (Linux)

```
-g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
-Wl,--build-id        # linker: embed a GNU build-id note (Clang's driver often does this; be explicit)
```

Linux capture (**available after WP-70**; scheduler events after WP-71) uses `perf_event_open` with frame-pointer callchains, so *every* function (including leaf
functions and the system libraries you control) must keep its frame pointer. Do not strip release binaries
before archiving them (section 4.3).

### 3.3 CMake sketch

```cmake
if(MSVC)
  target_compile_options(engine PRIVATE /Zi /Zo /Oy-)
  target_link_options(engine PRIVATE /DEBUG:FULL)
else()
  target_compile_options(engine PRIVATE -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
  target_link_options(engine PRIVATE -Wl,--build-id)
endif()
```

## 4. Symbols in CI

### 4.1 PDB retention and the symbol store (Windows)

Keep every PDB of every build that may be profiled. Publish them into a symbol store at the end of the build
job, keyed by the same GUID+age the exe/DLL carries:

```
symstore add /r /f <build-output-dir>\*.pdb /s \\symbols\store /t "MyEngine" /v "%BUILD_ID%"
```

`/r` recurses, `/f` selects files, `/s` is the store root, `/t` the product name. Also add the `.exe` and
`.dll` files (`/f <dir>\*.exe`, `/f <dir>\*.dll`) so the viewer can find binaries for disassembly. Keep the
store for as long as you want to open old captures.

The viewer finds PDBs by the `ModuleRec` identity (PDB GUID+age, or PE timestamp+size when no CodeView record
exists) through a local cache, then your symbol server. Pointing the viewer at the store over HTTP and the
two-tier cache arrive with the symbol server client: **available after WP-21**. Until then, keep the PDBs next
to the executables in the machine that opens the capture (the symbol locator searches next to the module path first: **available after WP-21/WP-22**).

### 4.2 Build ids (Linux)

With `-Wl,--build-id` every ELF carries `.note.gnu.build-id`; the converter records it in the module table
(`id_kind` 2) and the DWARF provider uses it to find debug files. Check a build:

```
readelf -n build/engine | grep 'Build ID'
```

### 4.3 Separate debug info, `.gnu_debuglink` and debuginfod

For shipping-sized binaries split the debug info off, keep it, and let the binary point at it:

```
objcopy --only-keep-debug engine engine.debug
objcopy --strip-debug --add-gnu-debuglink=engine.debug engine
# store engine.debug as   <store>/.build-id/ab/cdef....debug   (first byte as directory, rest as filename)
```

Serve that directory with `debuginfod` (`debuginfod -F <store>`) and give clients `DEBUGINFOD_URLS`. The
`.gnu_debuglink`/debuginfod lookup in the viewer is part of the symbol locator: **available after WP-21**
(libdw/DWARF provider: WP-72; DIA/PDB provider: WP-22).

## 5. Instrumenting the engine

All fragments come from `engine_hooks_example.cpp`. Colors are `0xRRGGBBAA`, built with
`TACHYON_MAKE_COLOR(r, g, b)`.

### 5.1 Thread naming at thread start

Name every thread first thing in its entry point, with **both** the OS API (debuggers, ETW, `perf`, crash
dumps) and Tachyon (the viewer's track labels, which also work on sidecar-only captures):

```cpp
void setThreadName(const char* name) {
#if defined(_WIN32)
  wchar_t wide[64] = {};
  MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 63);
  SetThreadDescription(GetCurrentThread(), wide);            // Windows 10 1607+
#else
  char shortName[16] = {};                                   // Linux limit: 15 characters
  std::strncpy(shortName, name, sizeof(shortName) - 1);
  pthread_setname_np(pthread_self(), shortName);
#endif
  Tachyon_SetCurrentThreadName(name);                        // full UTF-8 name (up to 256 bytes)
}
```

Name pooled workers with an index (`"Worker 3"`), and rename a thread whenever the engine reassigns its role.

### 5.2 Job system: one scope per job

Use the **job name as the scope id** and put per-instance information in `data`. The job descriptor holds a
static name, so pointer interning works and the viewer groups all instances of a job:

```cpp
struct JobDesc { const char* name; uint32_t color; void (*fn)(uint32_t instance); };   // name is static

void runJob(const JobDesc& job, uint32_t instance) {
  char data[48];
  std::snprintf(data, sizeof(data), "instance=%u", instance);
  TACHYON_SCOPE_DATA(job.name, data);          // id = static job name, data = copied
  job.fn(instance);
}
```

`TACHYON_SCOPE_DATA` accepts any expression for `id`, as long as the pointer it yields is static.
**Never** pass `std::string::c_str()` or a `snprintf` buffer as the id: it will intern a dangling pointer. If
a job name is built at run time, intern it once into a permanent table and pass that pointer.

Format `data` only when profiling is compiled in (`#if TACHYON_ENABLED`, as the example does) and keep it
short: it is copied into the ring buffer on every call.

**Worker wake-up markers.** Put a marker right after the worker returns from its condition-variable or
semaphore wait. In the viewer it lines up "work was submitted" with "a worker woke up", which is where job
latency hides:

```cpp
m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
Tachyon_Marker("JobSystem.WorkerWake", nullptr, kColorMarker);   // name must be static
```

### 5.3 Fiber scheduler

Rules (the wire format and the SDK's per-fiber bookkeeping depend on them):

1. `Tachyon_RegisterFiber(id)` when a fiber is created, `Tachyon_UnregisterFiber(id)` when it is destroyed. Also
   register the "scheduler/thread" context if your design switches to one.
2. Around every switch, the code leaving fiber `current` calls `Tachyon_BeginFiberSwitch(current, next)`
   immediately before the switch primitive.
3. The code that starts running `next` after the switch (possibly on a different thread) calls
   `Tachyon_EndFiberSwitch(next)` as the first thing it does.

```cpp
void switchTo(uint64_t next) {
  Tachyon_BeginFiberSwitch(m_current, next);
  // SwitchToFiber(next) / swapcontext(...) / engine assembly switch goes here.
  m_current = next;                           // runs on the resumed side
  Tachyon_EndFiberSwitch(next);
}
```

In a real scheduler the two calls are in different stacks: Begin before the primitive on the old fiber's side,
End right after the primitive returns (which, from the new fiber's perspective, is the start of its resumed
code, or the entry function for a brand-new fiber). The example fake scheduler runs both sides on one thread
because it has no real stacks. The fiber id can be any value unique among live fibers (a pointer or a pool
index, assumption Q4). Scopes open at a switch are saved and restored per fiber, so a job that yields mid-scope
shows up as one continuous scope that migrates across threads.

### 5.4 Locks

Wrap the engine's mutex so the *wait* is a lock event that carries the lock's address and name:

```cpp
void ProfiledMutex::lock() {
  TACHYON_LOCK_SCOPE(m_mutex, m_name);    // lvalue whose address identifies the lock; static name
  m_mutex.lock();
}                                         // the scope ends once we own the lock: hold time is not profiled
```

Lock waits are recognised through the reserved color `TACHYON_LOCK_COLOR_SENTINEL` (`0x4C4F43FF`) which the
SDK sets for you: do not use that color elsewhere. If `lockName` is `NULL` the event is called
`Tachyon.LockAcquire`. Wrap only locks that can actually block (contended mutexes, `SRWLOCK`, futex-based
locks); a `try_lock` fast path does not need an event. Combined with context-switch data, the viewer then shows
who held the lock (the callstack that unblocked the waiter; on Linux this needs scheduler events, **available after WP-71**).

### 5.5 Frame marks on main and render threads

```cpp
TACHYON_FRAME("Main", frameNumber);       // first statement of the main-thread frame
TACHYON_FRAME("Render", frameNumber);     // first statement of the render-thread frame (its own thread)
```

Use one name per pipeline stage and the same `frameNumber` for the same logical frame, so the viewer can pair
a frame's main-thread and render-thread parts. Call the mark at the *start* of the frame; the next mark closes
it. Names must be static.

### 5.6 Streaming and I/O scopes

One scope per request, asset path as `data`:

```cpp
void streamAsset(const std::string& path) {
  TACHYON_SCOPE_DATA("IO.ReadAsset", path.c_str());
  readAndDecompress(path);
}
```

Use one static id per kind of operation (`IO.ReadAsset`, `IO.Decompress`, `IO.Upload`) and the asset path as
data. Because a scope must begin and end in one function on one thread, split asynchronous I/O into the part
that runs on the I/O thread (scope it) and the completion callback (scope that separately), and link them
through the same path in `data`.

### 5.7 Plots, markers and GPU timings

`Tachyon_Plot(name, value)` records a numeric series (not per thread); `Tachyon_Marker(name, data, color)` an
instant event. Typical uses: frame time, queue depths, memory budget, level-load events.

**GPU timestamp calibration (known gap in API v1).** The trace format has a `ClockCalibrationRec`
(CPU trace timestamp paired with another clock: TSC or GPU), but the frozen v1 API has no call to emit one, and
this guide does not work around that with a private channel. For v1, record GPU frame times as plots and
correlate by markers:

```cpp
Tachyon_Plot("GPU.FrameMs", gpuFrameMs);                         // resolved on the CPU when the query returns
Tachyon_Marker("GPU.FrameSubmitted", frameLabel, kColorRender);  // CPU time the frame was submitted
```

The viewer shows them against the CPU timeline at the time the plot sample was *recorded*, so they are
accurate to the latency of your query readback, not to GPU timestamp resolution. A future API version (see
the report for WP-55) should add a calibration call.

### 5.8 Mapping an existing profiling layer

If the engine already has macros such as `PROFILE_SCOPE("Name")`, `PROFILE_FUNCTION()`,
`PROFILE_FRAME()`, redefine them instead of editing call sites:

```cpp
#define PROFILE_SCOPE(name)         TACHYON_SCOPE(name)
#define PROFILE_SCOPE_COLOR(n, c)   TACHYON_SCOPE_COLOR(n, c)
#define PROFILE_SCOPE_DATA(n, d)    TACHYON_SCOPE_DATA(n, d)
#define PROFILE_FUNCTION()          TACHYON_FUNCTION()
#define PROFILE_FRAME(n, i)         TACHYON_FRAME(n, i)
```

Conditions: the old `name` argument must be a string literal (static); if call sites pass a `std::string`,
keep that API but make it go through `TACHYON_SCOPE_DATA("Dynamic", name.c_str())`. Functions that do not have
a macro equivalent (markers, plots, fibers, flush) go through one small engine-owned layer, `engine::prof`
in the example, with every function compiled out when `TACHYON_ENABLED == 0`; this is also how you keep the
engine building without the SDK. Do **not** declare the `TACHYON_*` macros in your own headers: they are part
of the frozen header.

### 5.9 Hitch trigger and flight recorder

```cpp
const double frameMs = msSince(frameStart);
if (frameMs > kBudgetMs && sinceLastFlushMs >= kCooldownMs) {
  Tachyon_Marker("Hitch", "frame over budget", kColorSync);
  Tachyon_RequestFlush("hitch");
  sinceLastFlushMs = 0.0;
}
```

`Tachyon_RequestFlush(reason)` is a synchronous round trip on the **calling thread** to the agent (it sends the
control protocol `flush` command with `name = reason`, `seconds = 10`). It returns 1 if the agent answered
`ok`, else 0, and **it can block for up to about 200 ms when no agent is reachable** (3 s if an agent accepted
but stalled). So call it from the main thread only at a hitch, with a cooldown, never per frame. The agent
side needs the circular capture started with `--circular-mb`; the end-to-end flight recorder (circular
capture, dumping the window around the trigger, viewer support) is **available after WP-60**. Until then the
call is safe and returns 0 or 1 according to what the agent supports.

### 5.10 Console commands (start, stop, flush)

Flushing from the engine uses the SDK. Start and stop are agent commands, so the engine either shells out to
`tachyon-cli` or speaks the control protocol itself.

**Shelling out to `tachyon-cli`** (simplest; used in the example):

```cpp
int runCli(const std::string& args) {
  const char* exe = std::getenv("TACHYON_CLI");                       // optional path override
  const int status = std::system(((exe ? exe : "tachyon-cli") + std::string(" ") + args).c_str());
#if defined(_WIN32)
  return status;                                                   // already the exit code on Windows
#else
  return status == -1 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : 1);  // <sys/wait.h>
#endif
}

if (line == "prof.flush")  return Tachyon_RequestFlush("console") ? 0 : 1;
if (line == "prof.start")  return runCli("start --mode attach --pid " + std::to_string(pid) +
                                         " --out capture.tachyon");
if (line == "prof.stop")   return runCli("stop --wait");
if (line == "prof.status") return runCli("status");
```

`tachyon-cli` subcommands: `status`, `start --mode launch|attach|system [--exe E] [--args ...] [--pid N]
[--hz N] [--circular-mb N] [--out PATH] [--wait]`, `stop [--wait]`, `flush --name N --seconds S`, `cancel`;
global options `--socket PATH | --pipe NAME`, `--json`, `--timeout S`. Exit status is non-zero on errors.
Starting a capture needs an agent that is running with the privilege the backend requires (an elevated agent on
Windows for ETW). Capturing with ETW sampling and context switches in the agent: **available after WP-11 (ETW), WP-13 (launch/attach) and WP-52 (ingestion)**
on the Windows backend; sidecar-only instrumentation works today.

**Speaking the control protocol directly** (JSON lines, one object per line, `\n`-terminated; every request has
an `id` that the response echoes). Linux socket path: `/run/user/<uid>/tachyon-agent.sock`, falling back to
`/tmp/tachyon-agent.sock`; Windows named pipe `\\.\pipe\tachyon-agent`; `TACHYON_AGENT_PIPE` overrides both:

```cpp
// Linux; returns the agent's one-line JSON reply, or "" on failure. Not thread-safe per socket.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <string>

std::string agentCall(const std::string& path, const std::string& requestLine) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  std::string reply;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    const std::string line = requestLine + "\n";
    if (::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size())) {
      // Unsolicited {"event":...} lines may arrive first: skip lines that carry no "id".
      for (;;) {
        reply.clear();
        char c;
        while (::read(fd, &c, 1) == 1 && c != '\n') reply.push_back(c);
        if (reply.empty() || reply.find("\"id\"") != std::string::npos) break;
      }
    }
  }
  ::close(fd);
  return reply;
}
// agentCall(path, R"({"id":1,"cmd":"flush","name":"console","seconds":10})");
// agentCall(path, R"({"id":2,"cmd":"stop"})");
```

On Windows open the pipe with `CreateFileA` and use `WriteFile`/`ReadFile` in byte mode; the framing is the
same. Responses are `{"id":N,"ok":true,...}` or `{"id":N,"ok":false,"error":"...","code":"ENOPRIV|EBUSY|ENOTFOUND|EINTERNAL"}`;
`stop`/`flush` while idle give `ENOTFOUND`, a second `start` gives `EBUSY`. The agent also pushes unsolicited
`{"event":"state",...}` and `{"event":"capture_done",...}` lines on the same connection, so a robust client
skips lines without a matching `id`. Full definition: `docs/specs/control-protocol.md`.

## 6. Recommended default colors per subsystem

Use the same color for the same subsystem everywhere so the timeline reads at a glance. Pass them as the `color`
argument (`TACHYON_SCOPE_COLOR(id, kColorRender)`); omit it (`TACHYON_DEFAULT_COLOR`) for generic scopes.
Avoid `0x4C4F43FF`, which marks lock waits.

```cpp
constexpr uint32_t kColorJobs     = TACHYON_MAKE_COLOR(0x4C, 0xAF, 0x50);  // green    job system, gameplay update
constexpr uint32_t kColorFibers   = TACHYON_MAKE_COLOR(0x00, 0xBC, 0xD4);  // cyan     fiber scheduler
constexpr uint32_t kColorRender   = TACHYON_MAKE_COLOR(0x21, 0x96, 0xF3);  // blue     render thread, draw submission
constexpr uint32_t kColorPhysics  = TACHYON_MAKE_COLOR(0x79, 0x55, 0x48);  // brown    physics
constexpr uint32_t kColorAnimation= TACHYON_MAKE_COLOR(0x9C, 0x27, 0xB0);  // purple   animation, skinning
constexpr uint32_t kColorAudio    = TACHYON_MAKE_COLOR(0x3F, 0x51, 0xB5);  // indigo   audio
constexpr uint32_t kColorIo       = TACHYON_MAKE_COLOR(0xFF, 0x98, 0x00);  // orange   streaming, file I/O, decompression
constexpr uint32_t kColorNetwork  = TACHYON_MAKE_COLOR(0x00, 0x96, 0x88);  // teal     networking
constexpr uint32_t kColorScript   = TACHYON_MAKE_COLOR(0xCD, 0xDC, 0x39);  // lime     gameplay scripting
constexpr uint32_t kColorSync     = TACHYON_MAKE_COLOR(0xF4, 0x43, 0x36);  // red      waits, stalls, hitch markers
constexpr uint32_t kColorMarker   = TACHYON_MAKE_COLOR(0xE9, 0x1E, 0x63);  // pink     instant markers
```

## 7. Environment variables

Read once, at first use of the SDK in the process:

| Variable | Effect |
|---|---|
| `TACHYON_AGENT_PIPE` | Agent control address (Windows pipe name, Linux socket path). Default: `\\.\pipe\tachyon-agent`; `/run/user/<uid>/tachyon-agent.sock`, else `/tmp/tachyon-agent.sock` |
| `TACHYON_SIDECAR` | Path of the sidecar `.tachyon-inst` file used when no agent is connected; `off` disables the sidecar. Default `<cwd>/<exe-name>-<pid>.tachyon-inst` |
| `TACHYON_DISABLE` | `1`, `true`, `TRUE` or `on` turns the runtime off: every call returns after one relaxed atomic load |
| `TACHYON_RING_KB` | Per-thread ring buffer size in KiB (default 64). Raise it if the viewer shows `tachyon.dropped` markers |

If the agent is not reachable at the first call the runtime writes the sidecar instead, and retries the agent
about once a second; when the agent appears, the stream switches to it. Dropped records are counted and shown
as `tachyon.dropped` markers. The example's ctest sets `TACHYON_SIDECAR` to a path in the build directory, so
it never writes into the source tree. Separately, `tachyon-cli` honours `TACHYON_AGENT_PIPE` like the SDK, and
the example's `prof.*` console commands honour `TACHYON_CLI` (path to the executable).

## 8. Overhead expectations

Measured on the CI VM by the SDK perf test (WP-51), Begin+End pair, median:

| Case | Linux | Windows |
|---|---|---|
| Recording, agent connected | 46.9 ns | 68 ns |
| Runtime-disabled (`TACHYON_DISABLE=1` or no sink) | 4.7 ns | 16 ns |
| Compiled out (`TACHYON_ENABLED=0`) | 0 | 0 |

The cost is dominated by the two clock reads (Begin and End timestamps), not by the ring write.

**100 000 scopes per 16.6 ms frame** (the M5 gate is "overhead under 1% at 100 k scopes/frame with agent
connected"):

| | Cost per frame | Share of a 16.6 ms frame |
|---|---|---|
| Linux, recording | 100 000 x 46.9 ns = 4.69 ms | 28.3% |
| Windows, recording | 100 000 x 68 ns = 6.80 ms | 41.0% |
| Linux, runtime-disabled | 0.47 ms | 2.8% |
| Windows, runtime-disabled | 1.60 ms | 9.6% |

**Plainly: at the measured per-scope cost, 100 k scopes per 16.6 ms frame does not meet a 1% overhead target.**
It costs about 28% on Linux and 41% on Windows, and even a runtime-disabled SDK costs 2.8% / 9.6%. Meeting
1% (166 us per frame) allows about 3 500 scopes per frame on Linux (166 us / 46.9 ns) and about 2 400 on
Windows (166 us / 68 ns). The per-pair cost is a floor set by two clock reads, so the way to hit the budget is
granularity, not micro-optimisation:

- **Scope work that lasts at least ~5-10 us.** A 47 ns pair is 1% of a 4.7 us scope; on Windows (68 ns) the
  break-even is 6.8 us. Shorter scopes cost more than they tell you: the sampler already shows what happens
  inside them.
- **Instrument the structure, let sampling find the leaves.** Scopes for jobs, systems, passes, I/O requests,
  lock waits, frame marks; no scopes in inner loops, per-entity or per-component code, accessors, or small
  math helpers.
- **Aim for hundreds to low thousands of scopes per frame** in always-on builds; allow tens of thousands only
  in deliberate "instrumented" captures where 10-40% overhead is acceptable and the viewer's sampled data is
  kept for the real hot spots.
- **Gate verbose categories** behind your own runtime flag (`if (profileVerbose) TACHYON_SCOPE(...)`), or a
  compile-time level macro, so the 100 k-scope capture is a conscious choice.
- Ship builds define `TACHYON_ENABLED=0` (zero cost) or leave the runtime-disabled path (`TACHYON_DISABLE=1`).
- `Tachyon_RequestFlush`, `Tachyon_SetCurrentThreadName` and the first call on each thread are not hot-path
  calls: the first call per thread allocates its ring.

Treat the M5 gate as a statement about what the runtime can sustain without loss (ring size, drain rate, dropped
counters), not as an overhead target for typical frames: re-measure with the real engine and `tachyon-cli`
after WP-52 lands.

## 9. Checklist

Build and symbols
- [ ] `tachyon/` headers vendored (and the commit recorded) or the SDK sources added to the build.
- [ ] Linking mode chosen (static, shared with `TACHYON_SDK_USE_SHARED` on Windows, or run-time load); only
      one SDK runtime per process.
- [ ] MSVC: `/Zi /Zo /Oy- /DEBUG:FULL`; Clang/GCC: `-g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`
      and `-Wl,--build-id`.
- [ ] CI keeps PDBs / debug files for every profilable build and runs `symstore add /r /f <dir> /s <store>
      /t <product>` (Windows) or archives `.debug` files by build-id (Linux).
- [ ] `TACHYON_ENABLED=0` configuration builds; shipping builds choose compile-time off or `TACHYON_DISABLE=1`.

Instrumentation
- [ ] Every thread names itself at start with the OS API and `Tachyon_SetCurrentThreadName`.
- [ ] Job system: one scope per job, static job name as id, instance info as `data`; wake-up marker after
      worker waits.
- [ ] Fibers: Register on create, Unregister on destroy, `BeginFiberSwitch` before and `EndFiberSwitch` after
      every switch; ids unique among live fibers.
- [ ] Contended locks go through `TACHYON_LOCK_SCOPE` with a static name.
- [ ] `TACHYON_FRAME` on the main and render threads with matching frame numbers.
- [ ] I/O and streaming scopes carry the asset path in `data`.
- [ ] Hitch trigger calls `Tachyon_RequestFlush("hitch")` rate-limited (needs agent circular mode, WP-60).
- [ ] Console commands for start/stop (via `tachyon-cli` or the control protocol) and flush.
- [ ] All scope ids are static strings; no scopes shorter than a few microseconds; scope count per frame
      reviewed against section 8.
- [ ] Colors follow section 6; `0x4C4F43FF` is not used manually.

Verification
- [ ] Run the engine with `TACHYON_SIDECAR=/tmp/test.tachyon-inst`: the file is non-empty after exit.
- [ ] `Tachyon_IsConnected()` is 1 while an agent is running (after WP-52 the agent ingests the stream).
- [ ] No `tachyon.dropped` markers at normal scope rates; if present, raise `TACHYON_RING_KB` or reduce scopes.
- [ ] Symbols resolve for the engine's modules in the viewer (symbol server: after WP-21).

## 10. Dependencies on unfinished work packages

| Feature | Status |
|---|---|
| SDK, sidecar, `Tachyon_RequestFlush` client side, control protocol, `tachyon-cli` | available (WP-50, WP-51, WP-10) |
| Agent ingests the instrumentation stream and merges it into a `.tachyon` capture | available after WP-52 |
| Windows ETW capture, 8 kHz sampling, context switches | available after WP-11 |
| Flight recorder end to end (circular buffer dump on `flush`) | available after WP-60 |
| Symbol server client, `.gnu_debuglink`/debuginfod lookup, local cache | available after WP-21 (DWARF provider WP-72, DIA provider WP-22) |
| Linux capture (`perf_event_open` sampling, scheduler events) | available after WP-70 / WP-71 |
| GPU clock calibration records | needs an SDK API addition (not in API version 1) |
