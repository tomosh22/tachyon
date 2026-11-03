## Tachyon performance report

**FAIL** - 3 regressions against the baseline (1 improvement).

Trace: 12.35 s on linux, 8000 Hz sampling, 3 threads, 98765 samples, 12 events lost.

> Warning: 12 events were lost during the capture.

> Warning: the kernel throttled sampling 2 time(s).

> Warning: spool cap hit \| twice

### Regressions

| Metric | Subject | Baseline | Current | Change |
|---|---|---:|---:|---:|
| Frame p95 | `Frame@Main` | 12.000 ms | 14.500 ms | +20.8% |
| Function exclusive | `engine.exe!Engine::fn1` | <= 1.50% | 9.50% | +8.00 pp |
| Lost events | - | 0 | 12 | +12 |

### Improvements

| Metric | Subject | Baseline | Current | Change |
|---|---|---:|---:|---:|
| Scope median | `scope:Update` | 4.000 ms | 2.500 ms | -37.5% |

> Note: frame series 'RenderFrame@Render' is not in the baseline

### Frame times

| Series | Frames | p50 ms | p95 ms | p99 ms | max ms | Over 16.67 ms | p95 vs baseline | p99 vs baseline |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `Frame@Main` | 740 | 8.250 | 14.500 | 21.125 | 50.666 | 4 | +20.8% | +0.6% |
| `RenderFrame@Render` | 740 | 8.250 | 12.000 | 13.000 | 50.666 | 4 | - | - |

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

### Instrumentation scopes

| Scope | Count | Median ms | p95 ms | Max ms | Total ms |
|---|---:|---:|---:|---:|---:|
| `Update` | 740 | 2.500 | 3.750 | 9.000 | 1900.500 |

### Lock contention

| Lock | Contentions | Total wait ms | Max wait ms |
|---|---:|---:|---:|
| `Mutex.Scene` | 17 | 4.500 | 1.250 |
| (unnamed) | 17 | 4.500 | 1.250 |

_tachyon-report, schema 1_
