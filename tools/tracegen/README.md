# tracegen: deterministic synthetic trace generator (WP-04)

`tracegen` turns a JSON *scenario* into three files with known answers:

| file | content |
|---|---|
| `<name>.tachyon` | the trace, written through `TraceWriter` (streaming, bounded memory) |
| `<name>.symbols.json` | fake symbol tables for every module (consumed by `JsonSymbolProvider`, WP-20) |
| `<name>.expected.json` | expected analysis results (`tachyon-expected-1`), used by WP-30..36 and WP-53 tests |

```
tracegen [--out-dir DIR] [--no-verify] [--manifest FILE] scenario.json...
```

Every run computes the expected results two ways and **fails** if they disagree (integers exact, floats 1e-9
relative): (a) *analytically*, from the scenario and generation bookkeeping; (b) by a *brute-force reference*
(`reference.cpp`) that re-reads the emitted `.tachyon` with `TraceReader` plus the `.symbols.json`, and applies
the Part 3.8 rules naively. On a mismatch both documents are kept as `<name>.analytic.json` and
`<name>.reference.json` for diffing. `--no-verify` skips (b).

## Reproducibility

* Same scenario (including `seed`) gives byte-identical output on Linux and Windows. The generator uses its own
  PRNG (splitmix64 seeding xoshiro256**) with integer-only range mapping, no `std::*_distribution`, no
  floating-point accumulation, and no unordered-container iteration in anything written to disk.
* Fixture binaries are not committed (no Git LFS). `tests/fixtures/tracegen/` holds the scenario JSON files and
  `manifest.json` with the SHA-256 of each generated `.tachyon`, `.expected.json` and `.symbols.json`.
  `tests/unit/tracegen/manifest_test.cpp` regenerates every scenario and checks the hashes (also on Windows CI).
* After an intentional change to the generator or a scenario, refresh the manifest:
  `tracegen --out-dir <dir> --manifest tests/fixtures/tracegen/manifest.json tests/fixtures/tracegen/{tiny,medium,scopes,fibers,frames,recursion,partial,partial_torn}.json`
  (the manifest lists exactly those eight; `large` has none).
* CMake: target `tachyon_fixtures` (part of ALL when tests are enabled) writes `tiny medium scopes fibers frames
  recursion partial partial_torn` to `${TACHYON_GENERATED_FIXTURE_DIR}` (`<build>/fixtures`). `tachyon_fixtures_large`
  (opt-in) writes `large` (64 threads x 600 s, about 300 M samples, about 650 MiB).

## Scenario JSON

All times in `*_us` / `*_ms` / `*_s` fields are integers; they are converted to ticks exactly
(`ticks = us * clock_frequency_hz / 1e6`). Times are relative to the trace start unless noted. Numbers may be
written as `"0x..."` strings. Unknown keys are ignored.

```jsonc
{
  "name": "tiny",                      // default: file stem
  "seed": 1,
  "platform": "windows",               // "windows" | "linux"  (FileHeader.platform), default windows
  "clock_frequency_hz": 10000000,      // default 10 MHz (QPC-like); 1000000000 allowed. Must be a multiple of sampling_hz
  "sampling_hz": 8000,                 // sample interval = clock / sampling_hz ticks
  "start_ts": 1000000,                 // absolute tick of the trace start (FileHeader.start_ts)
  "duration_s": 1,                     // or duration_ms / duration_us. end_ts = start_ts + duration
  "partial": {"crash_at_ms": 1300},    // optional; true | {crash_at_ms|crash_at_us}. See "partial" below
  "chunk_records": 1000000,            // flush threshold for the bulk streams (1..1048576)
  "events_lost": 0,                    // copied to CaptureInfo
  "detail": true,                      // include per-thread interval lists in expected.json
  "blend": true,                       // include the scope-blended sections in expected.json
  "process": {"pid": 4242, "parent_pid": 4000, "name": "tiny.exe", "command_line": "..."},
  "modules": [...], "stacks": [...], "stack_gen": [...], "threads": [...],
  "scopes": [...], "fibers": [...], "frame_marks": [...], "markers": [...], "plots": [...], "thread_names": [...]
}
```

### modules
```jsonc
{ "name": "game.exe", "path": "game.exe",          // path defaults to name
  "base": "0x140000000",                           // default: 0x140000000 + 0x10000000*index (windows)
  "id_kind": "pdb" | "gnu", "id": "<hex>", "age": 1, // default: derived from the seed (pdb: 16 bytes, gnu: 20 bytes)
  "size": 65536,                                   // default: end of last function + 0x1000, aligned
  "functions": [
    { "name": "Engine::update", "rva": "0x1200", "size": 768,   // rva default: after the previous function, 16-aligned
      "file": "src/engine.cpp", "line": 100,
      "lines":   [ {"rva_offset": 0, "size": 128, "line": 100}, ... ],   // default: auto-tiled ranges
      "inlines": [ {"rva_offset": 144, "size": 32,
                    "chain": [ {"function": "Vec3::length", "file": "...", "line": 42},   // innermost first
                               {"function": "Entity::distance", ...} ]} ] } ],
  "functions_generated": { "count": 300, "min_size": 64, "max_size": 2048, "name_prefix": "Engine::fn",
                           "files": 12, "inline_pct": 30 }   // appended after the explicit functions
}
```
The module index (`ModuleIdx`) is the position in `modules`; it is the order of the `ModuleRec`s in the trace.

### stacks
Stacks are named, root first. A frame is `"module!function"`, `"module!function+0x20"` (offset into the function;
default `+0x10` or smaller for tiny functions), or an unmapped absolute address: `{"abs": "0x7ffe12345678"}` /
`"abs:0x..."`. Unmapped frames are written with `module = kInvalidIdx` and the absolute address.

`stack_gen`: `[{"prefix": "gs", "count": 64, "min_depth": 6, "max_depth": 24, "root": "engine.exe!main",
"modules": ["engine.exe", ...]}]` creates stacks `gs0..gs63` from random functions of the listed modules (default
all), at random offsets.

### threads
```jsonc
{ "tid": 101, "name": "Main",            // or "count": 16, "tid_base": 2000, "name": "worker{i}" ({i}, {tid} expand)
  "cpu": 0,                              // cpu id in context switches / samples; default index % 8
  "from_start": true,                    // running at trace start, no switch-in record: Unknown until its first event
                                         // (see "State rules"); omit it and use start_us 0 for an explicit switch-in
  "start_us": 150000,                    // otherwise the first switch-in time (state Unknown before it)
  "mix": [ {"stack": "update", "weight": 40}, ... ],       // sample stack shares (relative weights)
  "mix": {"gen": {"prefix": "gs", "pick": 8}},             // or: pick 8 generated stacks, weights 1000/(rank+1)
  "intervals": [ ... ],                  // explicit sequence from the thread's start; {"group": [...], "repeat": N}
                                         // elements expand to N copies of the listed intervals
  "pattern": { ... },                    // generated continuation after the explicit intervals
  "extra_samples": [ {"at_us": 41000, "stack": "draw"} ],  // stray samples (must fall outside Running intervals)
  "fiber": { "initial": 1, "initial_us": 0, "switches": [ {"at_us": 100000, "to": 2, "gap_us": 20} ] } }
```
* `intervals` alternate: `running`, then `waiting` or `ready`, then `running`, ... The first must be `running`.
  Each has `dur_us` (omit only on the last one: it then lasts to the end of the trace). The last interval always
  extends to the end of the trace. `waiting`/`ready` take `wait_reason`, `wait_mode`, `priority`,
  `blocking_stack` (the callstack recorded in the switch-out record), and (waiting only) `ready_after_us`,
  `waker_tid`, `waker_stack` (a `ReadyRec` at that offset). `ready` means the thread was preempted
  (`old_state = 1`). `running` may carry `mix` to override the thread mix for that interval.
* `pattern`: `{"run_us": [lo,hi], "wait_us": [lo,hi], "wait_reasons": [..], "blocking_stacks": [..], "ready_pct": 80,
  "preempt_pct": 5, "waker_tids": [..], "waker_stack": "x", "latency_us": [lo,hi]}` generates alternating run /
  wait segments until the end of the trace. `ready_pct` is the share of waits that get a `ReadyRec`
  `latency_us` before the switch-in; `preempt_pct` the share of off-cpu intervals that are preemptions.
* Samples are taken on a fixed per-thread grid (`sample_interval` ticks, phase derived from seed and tid) while
  the thread is running; the stack of each sample comes from a low-discrepancy (Weyl) sequence over the mix
  weights, so shares are close to exact. A `from_start` thread's first running interval has no switch-in; its
  first event is the switch-out that ends it, so that whole prefix is `Unknown` and its samples get weight 0.

### scopes, fibers, frames, markers, plots
```jsonc
"scopes": [ { "tid": 301, "id": "Frame", "caller": "game.exe!main+0x10",   // caller_addr = module base + rva (inside that function)
              "begin_us": 0, "dur_us": 18000, "data": "", "color": "0xff808080",
              "repeat": {"count": 99, "period_us": 20000, "dur_jitter_us": 1000},
              "fiber": 1,                                    // optional; validated against the fiber events
              "children": [ {"id": "Update", "caller": "...", "begin_us": 1000, "dur_us": 6000, "children": [...]} ] } ]
```
`caller` may also be `"abs:0x..."` (an unmapped caller). Children begin relative to their parent's begin and must
end within it. Scope events are written as Begin/End `ScopeEventRec`s (End: id/data/color/caller zero).

```jsonc
"fibers": [ {"id": 1, "tid": 401, "register_us": 0, "unregister_us": 900000} ]
```
Fiber event semantics used by tracegen (the format leaves them open): `register`/`unregister` carry `fiber_id`.
For a switch on thread T: `switch_begin` at time `a` has `fiber_id` = the fiber being left and `other_fiber_id` =
the fiber being entered; `switch_end` at `a + gap` has `fiber_id` = the now running fiber and `other_fiber_id` =
the fiber left. A thread's `fiber.initial` is announced by a `switch_end` with `other_fiber_id = 0` at
`initial_us`. A scope listed with `"fiber": F` must begin and end while F runs on its thread.

```jsonc
"frame_marks": [ {"tid": 501, "name": "Frame", "start_us": 0, "period_us": 16666, "count": 117, "jitter_us": 300,
                  "hitches": [ {"frame": 59, "extra_us": 34000} ]} ]   // frame 59 lasts period + 34 ms; later marks shift
"markers":      [ {"tid": 101, "at_us": 100000, "name": "LevelLoaded", "data": "forest", "color": "0xff00ff00"} ]
"plots":        [ {"name": "fps", "start_us": 0, "period_us": 100000, "count": 10, "kind": "noise"|"ramp", "min": 30, "max": 60} ]
"thread_names": [ {"tid": 102, "at_us": 500000, "name": "Worker-renamed"} ]
```
Frame mark numbers start at 0. Plot values are multiples of 0.25 (exactly representable doubles).

### partial
`"partial": {"crash_at_ms": N}` (or `true`) writes the trace *without* `finish()`: header flag `partial` set,
`footer_offset == 0`, no footer chunk, and nothing after the crash time (the scenario duration is replaced by
`crash_at`; `end_ts` in the header is `start_ts + crash_at`). Every chunk before the crash is complete, so a
reader recovers everything and `expected.json` describes exactly that. Partial scenarios cannot contain scopes or
fibers.

`"partial": {"crash_at_ms": N, "torn_tail": true}` additionally cuts the last chunk of the file in half (header
intact, payload incomplete), so readers exercise truncated-chunk recovery. In this mode the final flush writes the
Samples chunk last, so the torn chunk is always a Samples chunk with known contents: `expected.json` describes the
records of the complete chunks only (all context switches and ready events, minus the samples of the last batch).
`torn_tail` scenarios cannot use `blend` or frame marks.

## State rules used by `expected.json` (clarifications of 3.8)

* **Context-switch records.** tracegen writes one record per thread transition with the other side being the
  idle thread (`tid 0`): a switch-out has `old_tid = T, new_tid = 0`, `old_state` 5 (waiting) or 1 (ready/preempted),
  `old_thread_stack` = the blocking stack; a switch-in has `old_tid = 0, new_tid = T`, `old_thread_stack = kInvalidIdx`.
  Readers must handle records with both sides non-zero as well. A `ReadyRec` carries `readying_stack` (or
  `kInvalidIdx`).
* **Unknown** (normative: `docs/specs/attribution-rules.md`, "Orchestrator clarifications"). A thread's state
  is `Unknown` from the trace `start_ts` (or thread start) until its first context-switch event. A thread whose
  first event is a switch-out is *not* inferred to have been running; that prefix stays `Unknown` and its
  samples have weight 0 (rule 1). A thread without any context-switch record is `Unknown` for the whole trace.
  To make a thread `Running` from the very start, give it an explicit switch-in at the trace start
  (`start_us` 0 without `from_start`).
* **Intervals.** The last interval of a thread lasts until the header `end_ts`. A `ReadyRec` while the thread is
  `Waiting` splits the wait into `Waiting [t, tr)` and `Ready [tr, switch-in)`; both pieces report the same
  `blocking_stack`, `unblocking_stack` and `woke_by_tid` (rule 4 clarification in the spec). A preempted wait (`old_state` 1 or 3) is `Ready` for its
  whole length without unblocking information. Intervals are half-open: `[start, end)`.
* **Sample weight (rule 1).** `min(sample_interval, end_of_enclosing_Running - ts)`; weight 0 when no `Running`
  interval contains `ts`. Zero-weight samples still count in `samples` and in sample counts of functions, but not
  in `*_ticks`. `clipped_samples` counts samples whose weight was reduced.
* **Scope blending (rule 5), made precise** (same as the monotonic-placement clarification in
  `docs/specs/attribution-rules.md`). For a sample with open scopes `S0..Sk` (outermost first) and stack
  frames `f0..fn` (root first): scope `Si` matches the deepest (largest index) frame whose function address range
  contains `caller_addr`; an unmatched scope uses slot 0 (directly above the root). Slots are made monotonic with
  depth: `slot(Si) = max(match(Si), slot(Si-1))`. The blended stack is `f0, [scopes with slot 0 in order], f1,
  [scopes with slot 1], ...`. "Above" means towards the leaf: the synthetic frame becomes the child of the matched
  frame and the parent of its callee (or the new leaf). Instances open at `ts` satisfy `begin <= ts < end`.
* **Fibers.** A scope instance belongs to the fiber that is current on its thread when it begins. While a fiber is
  switched out (between its `switch_begin` and its next `switch_end`) its open scopes accrue no time; segments
  are emitted per continuous run, and empty segments are dropped.
* **Frames.** Frame `i` of a (thread, name) series is `[mark_i, mark_{i+1})`; the last mark starts no frame.

## expected.json (`tachyon-expected-1`)

Keys are sorted; all times are absolute trace-clock ticks; all counts and ticks are integers.

```
schema, scenario, seed, generator ("tracegen 1.0.0"), clock_frequency_hz, sampling_hz, sample_interval_ticks
trace:   { start_ts, end_ts, partial }
counts:  { samples, context_switches, ready_events, scope_events, fiber_events, frame_marks, markers,
           plot_samples, thread_names }
modules: [ { index, name, base } ]                       // index == ModuleIdx
functions: [                                              // sorted by FunctionKey
  { kind: "function"|"unmapped", module, start_rva, name,  // FunctionKey = (module, start_rva); unmapped: module 4294967295,
    inclusive_ticks, exclusive_ticks, inclusive_samples, exclusive_samples,   //   start_rva = absolute address
    per_thread: [ { tid, inclusive_ticks, exclusive_ticks, inclusive_samples, exclusive_samples } ] } ]
threads: [ { tid, name, samples, zero_weight_samples, clipped_samples, sample_ticks,
             state_ticks: { running, ready, waiting, unknown },     // sum == end_ts - start_ts
             context_switches_out, context_switches_in, ready_events,
             intervals: [ { state: "running"|"ready"|"waiting"|"unknown", start, end,
                            blocking_stack: [name, ...leaf first], unblocking_stack: [...], woke_by_tid } ] } ]  // only if "detail"
scopes:  [ { id, instances, min_ticks, median_ticks, max_ticks, total_ticks } ]   // wall durations; median = element (n-1)/2
blended: {                                                // only if "blend"
  functions: [ same as `functions`, plus { kind: "scope", id, name: "scope:<id>" } entries ],
  placements: [ { scope, matched, parent: {kind, module, start_rva|id, name}, samples, ticks } ] }   // parent = frame directly below the synthetic frame
frames:  [ { mark_tid, name, number, start, end, threads: [ { tid, running_ticks, sample_ticks } ] } ]
hitch_frames: [ { mark_tid, name, number, start, end, duration_ticks } ]   // frames lasting >= 2x their series median
fibers:  [ { fiber_id, segments: [ { tid, scope, depth, scope_begin, begin, end } ], scope_ticks: [ { scope, ticks } ] } ]
plots:   [ { name, count, min, max, first_ts, last_ts } ]
```
Function names are `"<module>!<function>"` or `"<unmapped>!0x<addr>"`. Inclusive time counts once per sample even
when a function repeats in the stack (rule 2); exclusive time goes to the leaf frame (rule 3, physical function; inline
expansion is not part of `functions`).

## symbols.json (`tachyon-symbols-1`)

```
{ schema, scenario,
  modules: [ { module_idx, name, path, base, size, id_kind: "pdb"|"gnu-build-id", id (hex), age,
               functions: [ { name, start_rva, size, file, line,
                              lines:   [ { rva, size, line } ],                       // tile the function exactly
                              inlines: [ { rva, size, chain: [ { function, file, line } ] } ] } ] } ] }   // chain innermost first
```
Functions are sorted by `start_rva` and do not overlap. `module_idx` equals the `ModuleIdx` of the trace.

## Fixtures

| name | what it exercises |
|---|---|
| `tiny` | 2 threads, 1 s, `detail`. Stray (zero-weight) samples, clipped samples, recursion, an unmapped frame, a thread running at start with no switch-in (thread 101: Unknown for its first 40 ms, 321 zero-weight samples), an Unknown prefix for a late-starting thread, a Ready-split wait, a preemption, thread rename, markers, a plot |
| `medium` | 16 threads, 60 s, 8 kHz (about 7.5 M samples), generated module tables with inlines, 64 generated stacks |
| `scopes` | nested / repeated scopes, matched / unmatched / unmapped callers, `blend` |
| `fibers` | fiber switches with scopes split across them, two threads |
| `frames` | two frame-mark series, four threads, per-frame running / sample ticks, and a planted hitch: frame 59 of the main thread's `Frame` series lasts 50.666 ms (506,660 ticks) instead of 16.666 ms, listed in `hitch_frames` |
| `recursion` | direct, identical-frame and mutual recursion at 1 GHz clock (Linux platform) |
| `partial` | unfinalized file, small chunks (`chunk_records` 4096), crash at 1.3 s of 3 s |
| `partial_torn` | like `partial`, plus `torn_tail`: the last Samples chunk is cut in half |
| `large` | 64 threads x 600 s, about 300 M samples, streamed with bounded memory (not part of the default build) |
