# Test fixtures

Binary fixtures are **not** committed (Git LFS is not used). Everything here is either small text or an input
from which binaries are generated at build time.

| Directory | Content |
|---|---|
| `tracegen/` | Scenario JSON files for `tools/tracegen` (`tiny`, `medium`, `scopes`, `fibers`, `frames`, `recursion`, `partial`, `partial_torn`, `large`) and `manifest.json`, the SHA-256 of every generated `.tachyon`, `.expected.json` and `.symbols.json`. `tests/unit/tracegen/manifest_test.cpp` regenerates each scenario and checks the hashes. |
| `etw/` | Hex-encoded ETW event dumps (`*.hexevents`) that `etlstat replay` and the ETL converter tests read on any platform. |
| `perf/sched/` | Captured `sched_switch` / `sched_waking` tracepoint format descriptions and raw record bytes for the Linux scheduler-event decoder tests. |
| `export/` | The speedscope file-format JSON schema (MIT, see `SOURCE.txt`) used to validate the speedscope exporter. |

The generated traces are written to `<build>/fixtures/` by the `tachyon_fixtures` target (part of the default
build when tests are enabled); `tachyon_fixtures_large` (about 300 M samples, 650 MiB) is opt-in. They make good
sample traces for the viewer: `build/linux-release/fixtures/medium.tachyon`. See
[../../tools/tracegen/README.md](../../tools/tracegen/README.md).
