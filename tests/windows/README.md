# Windows-only tests

`tests/windows/<area>/CMakeLists.txt`, built only when `WIN32` (see `tests/CMakeLists.txt`). Run them with
`ctest --preset windows-release`. Several need an elevated prompt (real ETW sessions); the GitHub Actions
`windows` job runs on an elevated runner.

| Area | What it covers |
|---|---|
| `capture/` | The real ETW session controller (`etw_capture_test.cpp`) and live ETW decoding (`live_etw_test.cpp`). |
| `cli/` | `tachyon-cli capture --launch` / attach / child following against the testbed (`launch_cli_test.cpp`, `spawner_main.cpp`). |
| `etlconvert/` | Converting a real recorded ETL (`etlconvert_real_test.cpp`). |
| `etlstat/` | `etlstat record` round trip. |
| `inst/` | SDK instrumentation ingestion into a real ETW capture. |
| `symbols/` | The DIA PDB provider, the symbol-server fetch from `msdl.microsoft.com` and WinHTTP cancellation. |
| `disasm/`, `viewer/` | Disassembly of a real PE image and the Source panel on a DIA-resolved function. |

Tests that are cross-platform live in `tests/unit`, `tests/integration` and `tests/perf`; Linux-only tests that
need the kernel (`perf_event_open`, label `linux_privileged`) live in `tests/linux`. What is still waiting for a
run on a real Windows machine is listed in [../../README.md](../../README.md#project-status).
