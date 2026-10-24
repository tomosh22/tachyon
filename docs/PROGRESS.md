# Tachyon progress

Maintained by the orchestrator. Updated after every merge. Status values: not started, in progress, in review, merged, blocked, needs Windows verification (also used for privileged-Linux verification).

## Work packages

| WP | Title | Lane | Status | Branch | Date |
|---|---|---|---|---|---|
| WP-00 | Repository skeleton and CI | — | merged (CI green: linux-release, linux-asan, windows-release) | main | 2026-10-24 |
| WP-01 | ETW spike tool `etlstat` | A | not started | wp/01-etlstat | |
| WP-02 | Profiler testbed | — | in progress | wp/02-testbed | 2026-10-24 |
| WP-03 | Trace format library | — | in progress | wp/03-trace | 2026-10-24 |
| WP-04 | Synthetic trace generator `tracegen` | C | not started | wp/04-tracegen | |
| WP-10 | Agent skeleton and control protocol | A | not started | wp/10-agent | |
| WP-11 | ETW session controller | A | not started | wp/11-etw-session | |
| WP-12 | ETL to `.tachyon` converter | A | not started | wp/12-etlconvert | |
| WP-13 | Launch, attach, process tree, CLI | A | not started | wp/13-launch-cli | |
| WP-14 | Capture validation on the engine | A | not started | wp/14-capture-validation | |
| WP-20 | Symbol core | B | not started | wp/20-symbol-core | |
| WP-21 | Symbol locator and HTTP client | B | not started | wp/21-symbol-locator | |
| WP-22 | DIA symbol provider | B | not started | wp/22-dia | |
| WP-30 | Model loader | C | not started | wp/30-model | |
| WP-31 | Thread state intervals | C | not started | wp/31-thread-states | |
| WP-32 | Attribution and aggregation | C | not started | wp/32-attribution | |
| WP-33 | Filters, cancellation, query cache | C | not started | wp/33-filters | |
| WP-34 | Line/instruction attribution, disassembly | C | not started | wp/34-lines-disasm | |
| WP-35 | Function and scope statistics, frames | C | not started | wp/35-stats | |
| WP-36 | Timeline LOD | C | not started | wp/36-lod | |
| WP-40 | Viewer shell | D | not started | wp/40-viewer-shell | |
| WP-41 | Session explorer and capture dialogs | D | not started | wp/41-session-explorer | |
| WP-42 | Threads timeline | D | not started | wp/42-timeline | |
| WP-43 | Call graph panel | D | not started | wp/43-call-graph | |
| WP-44 | Function list and butterfly | D | not started | wp/44-function-list | |
| WP-45 | Source and disassembly panel | D | not started | wp/45-source-view | |
| WP-46 | Thread interaction | D | not started | wp/46-thread-interaction | |
| WP-47 | Search and Function Statistics | D | not started | wp/47-function-stats | |
| WP-48 | Status, warnings, symbol progress | D | not started | wp/48-status | |
| WP-50 | Public SDK header and loader | E | in progress | wp/50-sdk-header | 2026-10-24 |
| WP-51 | Client runtime | E | not started | wp/51-sdk-runtime | |
| WP-52 | Agent-side ingestion and merge | — | not started | wp/52-ingestion | |
| WP-53 | Scope pairing, blending, fibers | C | not started | wp/53-scopes | |
| WP-54 | Viewer instrumentation lanes, frame graph, plots | D | not started | wp/54-inst-lanes | |
| WP-55 | Engine integration guide and samples | E | not started | wp/55-integration-guide | |
| WP-60 | Flight recorder mode | — | not started | wp/60-flight-recorder | |
| WP-61 | Remote capture | — | not started | wp/61-remote | |
| WP-62 | CI report tool | — | not started | wp/62-report | |
| WP-63 | Exporters | — | not started | wp/63-exporters | |
| WP-64 | Large-trace performance pass | — | not started | wp/64-perf-pass | |
| WP-65 | Real-time live view (optional) | — | not started (gated: rest of M6 first) | wp/65-live | |
| WP-70 | `perf_event_open` backend | — | not started | wp/70-perf-backend | |
| WP-71 | Scheduler events | — | not started | wp/71-sched | |
| WP-72 | DWARF symbol provider | B | not started | wp/72-dwarf | |
| WP-73 | Linux SDK transport and privileges | — | not started | wp/73-linux-sdk | |

## Milestone gates

| Gate | Result | Date | Notes |
|---|---|---|---|
| M0 | not run | | |

## Windows / privileged verification queue

Commands for the user to run on a Windows (or privileged Linux) machine are listed here per WP once the code is merged.

### WP-00
```
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```
(Run from a "x64 Native Tools Command Prompt for VS 2022". The GitHub Actions `windows` job runs the same.)

## Open questions (Part 7) and assumed defaults

1. Platform order Windows → Linux, consoles only with SDK access — assumed yes.
2. C++ / MSVC 2022 / Clang — assumed yes; engine at C++17 or later, so the SDK header targets C99 and C++17.
3. Existing profiling macro layer — assumed none; WP-55 adds `TACHYON_*` macros and shows how to map an existing layer.
4. Fiber ids are 64-bit at switch time; job names are static strings — assumed yes.
5. Symbol store — assumed none yet; WP-21 tested against a mock server and msdl.microsoft.com; no credentials in repo.
6. Repo location / SDK consumption — this repo; engine vendors `sdk/include/tachyon/` by copy.
7. Viewer GPU — discrete GPUs; budgets measured as CPU ms per frame.
8. Names — keep `tachyon`.

## Plan concerns raised

- WP-03 bytes/node budget: with 8-byte nodes and 4-byte hash slots at load factor ≤ 0.7 and doubling growth, worst case right after a resize is ≈ 8 + 4/0.35 ≈ 19.4 bytes/node. Proposed: assert ≤ 20 bytes/node at worst-case table size, or allow a non-doubling growth factor. Proceeding with the plan as written; the WP-03 report must state the measured worst case.

## Environment notes

- Linux dev container: Clang 18.1.3, CMake 3.28, Ninja; libdw/libelf, libcurl, SDL2 2.30.0, libGL, libclang-rt-18 installed via apt.
- Network: GitHub release assets and git clones work; `github.com/<o>/<r>/archive/...` source tarballs are blocked by the proxy, so dependencies use release archives (with SHA256) or pinned git commits.
