# tachyon-report

CI report tool (WP-62). Turns a `.tachyon` trace into a JSON and/or Markdown report, optionally compares it with
the report of a previous run, and exits non-zero on a regression. The logic is in `libs/report`
(`tachyon::report`); this directory holds `main.cpp`, the JSON schema and this document.

```
tachyon-report <trace.tachyon> [--baseline base.json] [--out report.json|report.md]...
               [--format json|md] [--top N] [--frame-series NAME] [--frame-budget-ms MS]
               [--thresholds thresholds.json] [--symbols PATH...] [--symbol-server URL...]
               [--symbol-cache DIR] [--no-blend] [--fail-on-lost]
```

| Option | Meaning |
|---|---|
| `--baseline FILE` | A report.json written by an earlier run. The diff is added to the output as `baseline_diff` and to the Markdown. |
| `--out FILE` | Write the report; the format follows the extension (`.json`, `.md`/`.markdown`). Repeatable. Without any `--out` the report goes to stdout. |
| `--format json\|md` | Stdout format when no `--out` is given (default `json`). |
| `--top N` | Functions, scopes and locks listed (default 20). |
| `--frame-series NAME` | Report only the frame series with this name (default: every series, at most 16). |
| `--frame-budget-ms MS` | Frame time budget for `over_budget_frames` (default 16.67). |
| `--thresholds FILE` | Baseline thresholds, see below. |
| `--symbols PATH...` | `.json` symbol files, or directories searched for symbol files. Several values may follow one option (the trace path must come first). `<trace stem>.symbols.json` next to the trace is picked up automatically. |
| `--symbol-server URL...` | Symbol servers (HTTP, debuginfod, local stores). |
| `--symbol-cache DIR` | Local symbol cache. |
| `--no-blend` | Do not blend instrumentation scopes into the function table. |
| `--fail-on-lost` | Exit 4 when the trace is partial or lost events. |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Report written, no regression against the baseline. |
| 1 | Regression against the baseline. The reports are still written. |
| 2 | Usage error: bad option, unreadable-as-JSON or invalid thresholds, unknown `--frame-series`, missing symbol path. |
| 3 | The trace, baseline or an output file cannot be read, written or parsed (including a baseline with another `schema_version`). |
| 4 | `--fail-on-lost` and the trace is partial or `events_lost > 0` (a regression takes precedence and gives 1). |

## Report JSON (schema version 1)

`report.schema.json` is the JSON Schema (draft 7) of the document; the unit tests validate real reports against
it. Keys appear in the order below. Times are milliseconds rounded to 1e-6, percentages are rounded to 1e-4.

- `schema_version` (1), `tool` (`"tachyon-report"`).
- `trace`: `duration_ms`, `platform`, `clock_frequency_hz`, `sampling_hz`, `events_lost`, `partial`
  (unfinalized or torn file), `throttle_events`, `thread_count`, `sample_count`, `has_instrumentation`,
  `warnings` (CaptureInfo `warnings`, at most 32), `total_cpu_ms` (running time summed over all threads; WP-69,
  absent in older reports).
- `settings`: `top_n`, `frame_budget_ms`, `blend_scopes` (effective: requested and the trace has scopes),
  `frame_series` (the filter or `null`).
- `frames`: `series_total`, `series[]` with `label` (`name@thread`, the baseline matching key), `name`, `thread`
  (`{tid, name}` or `null`), `default` (the series with the most marks), `mark_count`, `frame_count`, `p50_ms`,
  `p95_ms`, `p99_ms`, `max_ms`, `mean_ms`, `over_budget_frames`, `slowest_frame_number`. Percentiles use the lower
  order statistic `d[floor(q*(n-1))]` of the sorted frame durations (the rule of the 3.7 query clarifications,
  computed in integer arithmetic). Frames come from `frames()`: n marks of a series give n-1 frames.
- `threads[]`: `tid`, `name`, `running_ms`, `ready_ms`, `waiting_ms`, `unknown_ms` (from `threadStateTotals` over
  the whole trace) and `sample_count`, sorted by running time.
- `functions`: `total_exclusive_ms`, `function_count`, `top[]` (by exclusive time, ties by name then module):
  `name`, `module`, `kind` (`function` or `scope`), `exclusive_ms`, `inclusive_ms`, `exclusive_pct` (of
  `total_exclusive_ms`), `samples`. Functions with no exclusive time are not listed. With `blend_scopes`
  instrumentation scopes appear as `kind: "scope"` rows. WP-69 adds the baseline function set: `significant[]`
  (same row fields; every function with `exclusive_pct >= 0.5`, at most 2000, same order) and
  `significant_floor_pct` (0.5, or the smallest listed share when the cap cut the list). A function not in
  `significant` has a share below the floor. Both are absent in reports written before WP-69; the diff then
  falls back to the top N.
- `scopes`: `scope_count`, `top[]` (by total time): `name`, `count`, `min_ms`, `median_ms`, `p95_ms`, `max_ms`,
  `total_ms`, `mean_ms`, `unterminated`, `orphan_ends` (`scopeStats`; `p95_ms` from `scopeInstances`).
- `locks`: `lock_count`, `unparsed_waits`, `top[]` (by total wait): `name`, `address`, `contentions`,
  `total_wait_ms`, `max_wait_ms`, `unterminated`, `top_threads[]` (`lockSummary`).
- `baseline_diff` (only with `--baseline`): `verdict` (`pass` or `regression`), counts, `thresholds` used,
  `regressions[]` / `improvements[]` (`category`, `subject`, `baseline`, `current`, `value_unit`, `change`,
  `change_unit`), `notes[]`.

## Baseline diff

Frame series are matched by `label`, functions by (kind, module, name), scopes by name; FunctionKeys are not
stable across builds. A thresholds file is a JSON object with any of the keys below (unknown keys are an error):

| Key | Default | Rule |
|---|---|---|
| `frame_p95_rel_pct` | 10 | Frame p95 regresses when it grows by more than this percent **and** more than `frame_abs_floor_ms`. |
| `frame_p99_rel_pct` | 15 | Same for p99. |
| `frame_abs_floor_ms` | 0.5 | Absolute floor for the frame percentiles. |
| `function_exclusive_pp` | 2 | A function regresses when its share of the total exclusive time grows by more than this many percentage points (improves when it shrinks by more). |
| `scope_median_rel_pct` | 25 | A scope's median regresses when it grows by more than this percent **and** more than `scope_abs_floor_ms`. |
| `scope_abs_floor_ms` | 0.05 | Absolute floor for scope medians. |
| `lost_events_allowed` | 0 | Lost events regress when `current > baseline + this`. |
| `function_exclusive_rel_pct` | 25 | A function listed exactly in both reports (and not already flagged by share) regresses when its `exclusive_ms` grows by more than this percent **and** more than `function_exclusive_abs_floor_ms` (category `function_exclusive_ms`). |
| `function_exclusive_abs_floor_ms` | 5 | Absolute floor for per-function exclusive time. |
| `total_cpu_rel_pct` | 10 | `trace.total_cpu_ms` (all threads) regresses when it grows by more than this percent **and** more than `total_cpu_abs_floor_ms` (category `total_cpu`). Skipped when either report has no total. |
| `total_cpu_abs_floor_ms` | 10 | Absolute floor for total CPU. |

Functions are compared through the baseline function set (`functions.significant`, every function with at
least 0.5% exclusive share), so a function rising from 1% to 4% in a flat profile is flagged even when it is
outside the top N. A function missing from a report's set is assumed to be below `significant_floor_pct`; such
values are marked `*_is_upper_bound`. A report without the set (written before WP-69) is compared on its top N
only: a missing function is assumed to have at most the share of that report's smallest listed function (0 when
the list was not full). Scopes are compared by their top N. Notes (not failures) list scopes that are new or
removed, frame series missing from either report, a partial current or baseline trace, and traces whose
durations differ by more than 25% (absolute milliseconds are then not like for like). Old baselines still load. Findings are ordered by category, then by the size of the change, then
by subject. Any regression gives exit code 1.

## Markdown

The Markdown report is meant for a pull-request comment: a verdict line, regressions and improvements, a frame
table, the top functions and short thread, scope and lock tables. It is deterministic and contains no absolute
paths, lock addresses or other machine-specific data.

## Determinism

The same trace, symbols and options give byte-identical output: every list is sorted with ties broken by name,
numbers are rounded, and nothing depends on the worker thread count.
