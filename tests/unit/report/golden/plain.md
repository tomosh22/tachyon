## Tachyon performance report

**No baseline** - informational report.

Trace: 12.35 s on linux, 8000 Hz sampling, 3 threads, 98765 samples, 0 events lost.

> Warning: the trace is partial (the capture did not finish cleanly).

### Frame times

No frame marks in the trace.

### Top functions by exclusive time

| Function | Module | Exclusive ms | Exclusive % | Inclusive ms |
|---|---|---:|---:|---:|
| `Engine::fn0` | `render.dll` | 1000.000 | 10.00 | 2000.000 |
| `Engine::fn1` | `engine.exe` | 950.000 | 9.50 | 1950.000 |
| `Engine::fn2` | `render.dll` | 900.000 | 9.00 | 1900.000 |
| `operator\|(int)` | `engine.exe` | 850.000 | 8.50 | 1850.000 |
| `scope:Update` | - | 800.000 | 8.00 | 1800.000 |
| `Engine::fn5` | `engine.exe` | 750.000 | 7.50 | 1750.000 |
| `Engine::fn6` | `render.dll` | 700.000 | 7.00 | 1700.000 |
| `Engine::fn7` | `engine.exe` | 650.000 | 6.50 | 1650.000 |
| `Engine::fn8` | `render.dll` | 600.000 | 6.00 | 1600.000 |
| `Engine::fn9` | `engine.exe` | 550.000 | 5.50 | 1550.000 |

### Threads by running time

| Thread | Running ms | Ready ms | Waiting ms |
|---|---:|---:|---:|
| Main (501) | 9000.500 | 10.250 | 3000.125 |
| tid 502 | 4000.000 | 0.000 | 8000.000 |

_tachyon-report, schema 1_
