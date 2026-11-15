# Applications

One subdirectory per executable. Each has its own `CMakeLists.txt`, which the top-level build discovers
automatically. Binaries land in `<build>/bin/` (for example `build/linux-release/bin/`).

| Directory | Executable | What it is | Documentation |
|---|---|---|---|
| `agent/` | `tachyon-agent` | The capture daemon: owns the capture backend (ETW on Windows, `perf_event_open` on Linux, or a `mock` backend that replays a fixture), accepts commands from the CLI and the viewer over a local socket / named pipe (optionally TCP), ingests SDK instrumentation, and writes the `.tachyon` trace. Needs administrator rights (Windows) or `CAP_PERFMON` / root (Linux) for real captures. | [agent/README.md](agent/README.md), [agent/LINUX.md](agent/LINUX.md) |
| `cli/` | `tachyon-cli` | Command-line client of the agent (start, stop, capture, live, flush, fetch, processes) plus offline commands (`merge`, `export`, `doctor`). | [cli/README.md](cli/README.md) |
| `viewer/` | `tachyon-viewer` | The Dear ImGui + SDL2 + OpenGL GUI: timeline, call graph, function list, butterfly, thread interaction, source and disassembly, capture dialogs, live view. Built only when `TACHYON_BUILD_VIEWER` is ON (the default). | [../docs/viewer-guide.md](../docs/viewer-guide.md) |
| `report/` | `tachyon-report` | CI report tool: turns a trace into a JSON / Markdown report and compares it with a baseline. | [report/README.md](report/README.md) |
| `testbed/` | `tachyon-testbed` | A workload with known answers (hot functions, locks, sleeps, frames, fibers, jobs) for validating the profiler end to end. | [testbed/README.md](testbed/README.md) |

Shared code lives in `libs/` (trace format, model, analysis, symbols, capture backends, control protocol, SDK
ingestion, report and export libraries); `apps/*` only holds the executables' own logic.
