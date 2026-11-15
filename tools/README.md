# Developer tools

One subdirectory per tool, each with its own `CMakeLists.txt` (auto-discovered). These are for working on
Tachyon itself; end users want the programs in [../apps](../apps/README.md).

| Directory | Executable | What it is | Documentation |
|---|---|---|---|
| `tracegen/` | `tracegen` | Deterministic synthetic trace generator: turns a scenario JSON into a `.tachyon` trace, fake symbol tables and a ground-truth `expected.json`. The test fixtures (`tiny`, `medium`, `scopes`, `fibers`, `frames`, `recursion`, `partial`, `partial_torn`, and the opt-in `large`) are generated with it at build time. | [tracegen/README.md](tracegen/README.md) |
| `etlstat/` | `etlstat` | ETW spike tool. `record` captures a kernel session to an `.etl` file (Windows, elevated), `stats` analyses an `.etl` (Windows), `replay` analyses a text file of hex-encoded ETW events (any platform). It validated the ETW event layout that `libs/etlconvert` implements; the findings are in [../docs/etw-notes.md](../docs/etw-notes.md). | `etlstat help` (usage text) |

`etlstat` usage (from `etlstat help`):

```
etlstat record --out x.etl [--seconds N] [--hz 8000] [--exe path [--exe-args "..."]]    (Windows, elevated)
etlstat stats x.etl [--hz N] [--json] [--dump-hex N [--dump-out file]] [--pid N] [--image name]   (Windows)
etlstat replay x.hexevents [--hz N] [--json] [--pid N] [--image name]                  (any platform)
```

On Linux `record` and `stats` print that they need Windows; `replay` works on the files in
`tests/fixtures/etw/`. `tracegen` usage: `tracegen [--out-dir DIR] [--no-verify] [--manifest FILE]
<scenario.json>...`.

Build one with, for example, `cmake --build --preset linux-release --target tracegen`.
