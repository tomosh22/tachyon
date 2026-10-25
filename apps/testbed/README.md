# tachyon-testbed

A workload with known answers for validating profiler attribution end to end. It runs scenarios, names its
threads, and prints on stdout a JSON document stating what a profiler should see (`expected`) and what the
workload itself measured with `rdtsc` (`measured`). Diagnostics go to stderr; stdout is JSON only.

```
tachyon-testbed [--scenario <name[,name...]|all>] [--duration <seconds>] [--hitch-interval-ms <ms>] [--instrument] [--pretty] [--list]
```

Built with frame pointers (`-fno-omit-frame-pointer` / `/Oy-`), debug info, and no sibling-call optimization on the
testbed targets only. Work functions are `noinline`, `extern "C"` (unmangled names), and each has distinct machine
code so identical-code folding cannot merge them.

## Scenarios

| name | threads | known answer |
|---|---|---|
| `spin` | `tb-spin` | `hot_a` 40%, `hot_b` 20%, `hot_c` 10%, `spin_rest` 30% (calibrated loop counts, 1 ms cycle) |
| `sleep` | `tb-sleep` | `sleep_busy_work` 70% (running), `sleep_wait` 30% (sleeping, 10 ms cycle) |
| `lock` | `tb-lock-0`, `tb-lock-1` | one mutex; the holder is in `lock_critical_section`, the waiter in `lock_wait`; nominal 50/40/10 with `lock_outside_work` |
| `io` | `tb-io` | loops reading an 8 MiB temp file in 64 KiB chunks: `io_read_file` / `io_checksum` (nominal) |
| `recursion` | `tb-recursion` | `recurse_level` 50 deep to `recurse_leaf`; leaf ~100% |
| `inline` | `tb-inline` | `inline_hot_loop` with force-inlined `inline_a` 50%, `inline_b` 30%, `inline_c` 20% |
| `fibers` | `tb-fibers` | 4 fibers (Windows fibers / ucontext) round-robin on one thread; `fiber_work_0..3` 25% each |
| `jobs` | `tb-job-0..7`, `tb-job-prod` | 8 workers on one condition variable; `job_execute` ~25%, `job_wait` ~75% each |
| `frames` | `tb-frames` | 60 Hz loop: `frame_update` 3 ms, `frame_render` 4 ms, `frame_wait` rest; a 50 ms `frame_hitch` every 2 s |

`inline_a/b/c` are `TACHYON_FORCEINLINE` and appear only as inline frames in an optimized build (never as real
frames). Their share is still measured with `rdtsc` between the inlined bodies.

## `--instrument` (WP-52)

With `--instrument` the testbed also emits Tachyon SDK events, to the agent named by `TACHYON_AGENT_PIPE` (a
`tachyon-agent` launching it sets that) or, without an agent, to a sidecar `<exe>-<pid>.tachyon-inst` file in the
working directory (`TACHYON_SIDECAR=<path>` moves it, `TACHYON_SIDECAR=off` disables it). Without the flag the SDK
runtime is never started. What is emitted:

| scenario | events |
|---|---|
| all | thread names (`tb-*`), marker `testbed.scenario.begin/end` (data = scenario name) |
| `spin` | scope `spin_cycle` around `hot_a`, `hot_b`, `hot_c`, `spin_rest` scopes |
| `lock` | lock-wait scope (`testbed.lock`, lock sentinel colour) around `lock_wait`, scope `lock_critical_section` |
| `jobs` | scopes `job_wait` and `job_execute` on each worker |
| `frames` | scope `frame` around `frame_update`, `frame_render`, `frame_hitch`, `frame_wait`; frame mark `testbed.frame`, plot `testbed.frame_ms`, marker `testbed.hitch` |
| `fibers` | fiber register / unregister, switch begin / end between the worker context (id 100) and fibers 1..4, scope `fiber_slice` |

## Running several scenarios (`all`)

Scenarios run **sequentially**, not concurrently, so each one owns the CPU it needs and its `rdtsc` split is not
distorted by the others. `--duration` is the total for the whole run: it is divided equally across the scenarios,
except `frames`, which gets four times the weight so it can contain hitches. `--scenario all --duration 5` takes
about 5.2 s (calibration adds roughly 0.15 s). With several scenarios and no `--hitch-interval-ms`, the frames hitch
interval is scaled to half of the frames scenario's time (at most 2000 ms); a single `--scenario frames` uses 2000 ms.
The interval used is recorded in `details.hitch_interval_ms`.

## JSON schema (`"schema": "tachyon-testbed/1"`)

```
{
  "schema": "tachyon-testbed/1",
  "tool": "tachyon-testbed",
  "platform": "linux" | "windows",
  "pid": <uint>,
  "mode": "sequential",
  "requested_duration_s": <number>,        // the --duration value
  "clock_frequency_hz": <uint>,            // trace clock (QPC / CLOCK_MONOTONIC_RAW) frequency
  "tsc_frequency_hz": <uint>,              // measured rdtsc frequency; "tsc_ticks" below are in this unit
  "kernel_iters_per_second": <number>,     // calibrated burn-kernel speed
  "scenarios": [
    {
      "name": <string>,                    // one of the scenario names above
      "description": <string>,
      "planned_duration_s": <number>,
      "actual_duration_s": <number>,
      "threads": [                         // at least one; the threads that carry the split
        {
          "name": <string>,                // OS thread name (SetThreadDescription / pthread_setname_np)
          "tid": <uint>,                   // OS thread id, non-zero
          "enforced": <bool>,              // true: split is enforced by construction (loop counts / deadlines)
          "tolerance": <number>,           // advisory absolute per-function share tolerance for consumers
          "expected": [ { "function": <string>, "share": <number> } ],   // shares in [0,1], sum to 1
          "measured": [ { "function": <string>, "share": <number>, "tsc_ticks": <uint> } ]  // sum to 1
        }
      ],
      "details": { ... }                   // scenario specific object (see below); includes a "checksum"
    }
  ]
}
```

`expected` and `measured` list the same functions in the same order. `function` is the symbol name a profiler
should report (for `inline` and `fibers` see the table). For `enforced: false` threads `expected` is nominal and
`tolerance` is wide; use `enforced: true` threads for tight checks. `measured` shares are computed from `rdtsc`
stamps taken between the work calls on the thread itself, so they include any preemption suffered; a profiler that
samples only on-CPU time may legitimately differ on a loaded machine.

`details` keys of note: `lock`: `holder_function`, `waiter_function`, `acquisitions`; `jobs`: `producer`
(`name`, `tid`), `jobs_run`; `frames`: `frame_period_ms`, `hitch_interval_ms`, `frames_per_hitch`, `frames`,
`hitches`, `max_frame_ms`; `recursion`: `depth`; `fibers`: `fiber_count`; `io`: `bytes_read`.

## Layout and tests

* `testbed/` is the `tachyon::testbed_core` library (calibration, report/JSON, scenarios, platform shims) so unit
  tests can link it without running the full workload.
* `testbed/hooks.h` holds the instrumentation hooks (`TB_SCOPE`, `InstLockScope`, `onScenarioBegin/End`,
  `onFrameEnd`, `onThreadNamed`, fiber switches, ...). They are no-ops unless `--instrument` is given, in which case they
  call the public SDK (`sdk/include/tachyon/tachyon.h`) only.
* Tests: `tests/unit/testbed/` (`testbed_tests` label `unit`, `testbed_integration_tests` label `integration`, which
  runs `tachyon-testbed --scenario all --duration 5`).
