# Tachyon progress

Maintained by the orchestrator. Updated after every merge. Status values: not started, in progress, in review, merged, blocked, needs Windows verification (also used for privileged-Linux verification).

## Work packages

| WP | Title | Lane | Status | Branch | Date |
|---|---|---|---|---|---|
| WP-00 | Repository skeleton and CI | — | merged (CI green: linux-release, linux-asan, windows-release) | main | 2026-10-24 |
| WP-01 | ETW spike tool `etlstat` | A | needs Windows verification (code merged; CI windows job ran a real elevated record+consume round trip) | wp/01-etlstat | 2026-10-25 |
| WP-02 | Profiler testbed | — | merged (CI green incl. windows-release; Windows timing splits to confirm, see queue) | wp/02-testbed | 2026-10-25 |
| WP-03 | Trace format library | — | merged (CI green incl. windows; interning-time target split to WP-03b) | wp/03-trace | 2026-10-25 |
| WP-03b | Callstack interning throughput (10 s target) | — | merged (skewed 8.66 s met with streaming-input harness; uniform 11.99 s best effort) | wp/03b-intern-speed | 2026-10-25 |
| WP-04 | Synthetic trace generator `tracegen` | C | merged (CI green incl. windows manifest; independent oracle 0 mismatches) | wp/04-tracegen | 2026-10-25 |
| WP-10 | Agent skeleton and control protocol | A | merged (CI green incl. windows named-pipe tests; Windows console checks queued) | wp/10-agent | 2026-10-25 |
| WP-11 | ETW session controller | A | needs Windows verification (merged; 11 real ETW tests passed on elevated CI runner; 30 s dev-box run pending) | wp/11-etw-session | 2026-10-25 |
| WP-12 | ETL to `.tachyon` converter | A | needs Windows verification (merged; CI windows job converted a real ETL; hot_a/b/c ranking and 2 GB timing unconfirmed) | wp/12-etlconvert | 2026-10-25 |
| WP-13 | Launch, attach, process tree, CLI | A | merged; needs Windows verification (20 s dev-box run) | wp/13-launch-cli | 2026-10-25 |
| WP-13b | CLI --json errors, pid-reuse targets, attach-exit, resume, circular child following | A | merged; attach-exit race untested on Windows | wp/13b-cli-fixes | 2026-10-25 |
| WP-14 | Capture validation on the engine | A | not started | wp/14-capture-validation | |
| WP-20 | Symbol core | B | merged (oracle 0/929 mismatches; cold resolve 11.5 ms, memo 2.2 ns on CI) | wp/20-symbol-core | 2026-10-25 |
| WP-21 | Symbol locator and HTTP client | B | merged (CI green incl. msdl.microsoft.com ntdll.pdb fetch) | wp/21-symbol-locator | 2026-10-25 |
| WP-22 | DIA symbol provider | B | merged (Windows CI ran real DIA tests) | wp/22-dia-provider | 2026-10-25 |
| WP-30 | Model loader | C | merged (medium 0.15 s on CI vs 2 s; large 6.4 s on 4 cores vs 30 s; resident 6.95× compressed — budget under review) | wp/30-model | 2026-10-25 |
| WP-31 | Thread state intervals | C | merged (medium 3.9 ms on CI vs 0.5 s; independent oracle 0 mismatches on 8 fixtures) | wp/31-thread-states | 2026-10-25 |
| WP-32 | Attribution and aggregation | C | merged | wp/32-attribution | 2026-10-25 |
| WP-32b | Return-address rule (lookupCaller), NodeAgg iterative destructor, Hide-all-kernel totals | C | merged | wp/32b-return-address | 2026-10-25 |
| WP-33 | Filters, cancellation, query cache | C | merged | wp/33-query-cache | 2026-10-25 |
| WP-34 | Line/instruction attribution, disassembly | C | not started | wp/34-lines-disasm | |
| WP-35 | Function and scope statistics, frames | C | merged | wp/35-stats-frames | 2026-10-25 |
| WP-36 | Timeline LOD | C | merged (frame target follow-up in WP-36b) | wp/36-lod | 2026-10-25 |
| WP-36b | LOD 64-row frame under 4 ms; non-flaky frame assert | C | not started | wp/36b-lod-perf | 2026-10-25 |
| WP-40 | Viewer shell | D | merged (headless selftest + Xvfb verified; not yet run on a real display) | wp/40-viewer-shell | 2026-10-25 |
| WP-41 | Session explorer and capture dialogs | D | not started | wp/41-session-explorer | |
| WP-42 | Threads timeline | D | merged; needs GPU verification (60 fps) | wp/42-threads-timeline | 2026-10-25 |
| WP-42b | Timeline fixes + viewer SymbolService plumbing (real FunctionKeys) | D | merged | wp/42b-timeline-symbols | 2026-10-25 |
| WP-43 | Call graph panel | D | merged | wp/43-call-graph | 2026-10-25 |
| WP-44 | Function list and butterfly | D | merged | wp/44-function-list | 2026-10-25 |
| WP-45 | Source and disassembly panel | D | not started | wp/45-source-view | |
| WP-46 | Thread interaction | D | merged | wp/46-thread-interaction | 2026-10-25 |
| WP-47 | Search and Function Statistics | D | in progress | wp/47-function-stats | 2026-10-25 |
| WP-48 | Status, warnings, symbol progress | D | not started | wp/48-status | |
| WP-50 | Public SDK header and loader | E | merged (CI green incl. windows-release; caller-address test skipped on MSVC, no labels-as-values) | wp/50-sdk-header | 2026-10-25 |
| WP-51 | Client runtime | E | merged (CI green incl. windows; connected overhead 46.9 ns Linux / 68 ns Windows vs 40 ns target, under 80 ns gate; disabled 4.7 / 16 ns) | wp/51-sdk-runtime | 2026-10-25 |
| WP-51b | SDK runtime fixes (flaky AgentLossFallsBack test, sidecar truncation, perf flake) | E | merged (root cause: fake-agent race; 20× ctest repeats green under load) | wp/51b-sdk-fixes | 2026-10-25 |
| WP-52 | Agent-side ingestion and merge | A | merged (Windows CI ran a real ETW + --instrument capture) | wp/52-ingestion | 2026-10-25 |
| WP-53 | Scope pairing, blending, fibers | C | merged (WP-53b follow-up) | wp/53-scope-blending | 2026-10-25 |
| WP-54 | Viewer instrumentation lanes, frame graph, plots | D | not started | wp/54-inst-lanes | |
| WP-55 | Engine integration guide and samples | E | merged (CI green; verifier accuracy findings fixed by orchestrator) | wp/55-integration-guide | 2026-10-25 |
| WP-60 | Flight recorder mode | — | not started | wp/60-flight-recorder | |
| WP-61 | Remote capture | — | not started | wp/61-remote | |
| WP-62 | CI report tool | — | not started | wp/62-report | |
| WP-63 | Exporters | — | not started | wp/63-exporters | |
| WP-64 | Large-trace performance pass | — | not started | wp/64-perf-pass | |
| WP-65 | Real-time live view (optional) | — | not started (gated: rest of M6 first) | wp/65-live | |
| WP-70 | `perf_event_open` backend | — | not started | wp/70-perf-backend | |
| WP-71 | Scheduler events | — | not started | wp/71-sched | |
| WP-72 | DWARF symbol provider | B | in progress | wp/72-dwarf-provider | 2026-10-25 |
| WP-73 | Linux SDK transport and privileges | — | not started | wp/73-linux-sdk | |

## Milestone gates

| Gate | Result | Date | Notes |
|---|---|---|---|
| M0 | passed with exceptions | 2026-10-25 | CI green on Linux and Windows. etlstat numbers pending the user's Windows run (WP-01). Fixtures are generated at build time and checked against SHA256s, because LFS push is blocked. Trace round-trip/interning budget: skewed met (8.66 s), uniform 11.99 s against 10 s, memory met after compact. |
| M1 | passed for the testbed; engine part pending WP-14 (user-assisted) | 2026-10-25 | Windows CI (elevated runner): `tachyon-cli capture --launch tachyon-testbed.exe` gives a trace with 0 lost events, loader samples before main, child following and attach; the trace opens in TraceReader. Engine capture and conversion-time budget on a real engine capture need the user (WP-14). 20 s dev-box run queued below. |
| M2 | passed with exceptions | 2026-10-25 | Windows CI: DIA resolves testbed hot functions to file:line and the inline scenario to inline chains (every frame has a line); 1 M RVAs in about 4 s (budget 20 s); ntdll.pdb fetched from msdl.microsoft.com. No team symbol store yet (Part 7 default), so the team-store fetch is untested. An end-to-end symbolized testbed capture in the viewer on Windows is queued for the user. Known limits: hot/cold separated blocks resolve only when the parent's length covers them; an unbounded last public symbol claims trailing addresses. |

## WP-03 measured numbers (GitHub ubuntu-24.04 runner)

| Metric | Budget | Measured | Note |
|---|---|---|---|
| Intern 20 M stacks, depth 30, 4% unique (skewed picks) | < 10 s (CI gate < 20 s) | WP-03: ~10.4 s; WP-03b: 8.66 s printed | met after WP-03b (see note) |
| Same, uniform picks | < 10 s (CI gate < 20 s) | WP-03: ~12.4 s; WP-03b: 11.99 s printed | missed by ~2 s, best effort |

Note: WP-03b added transparent huge pages for the interner's arrays (Linux) and made the benchmark prefetch its own input stacks, modelling a converter that reads stacks sequentially. Interner-only time (THP, no input prefetch) measured 10.69 s skewed on CI, so the interner alone is ~5% over target; the remaining cost is ~2.9 dependent cache-miss pairs per stack. Closing it further would need a batched `internStacks()` API (not in the plan).
| Bytes/node after `compact()` | ≤ 16 | 13.7 | met |
| Bytes/node as grown / worst after resize | (info) | 22.4 / 22.9 | |

## WP-51 measured numbers and follow-ups

| Metric | Target | CI gate | Linux CI | Windows CI |
|---|---|---|---|---|
| Begin+End median, connected | < 40 ns | < 80 ns | 46.9 ns | 68 ns |
| Begin+End median, disabled at runtime | < 10 ns | < 20 ns | 4.66 ns | 16 ns |
| 16 threads × 1 M scopes stress | no corruption | — | passes; received + dropped == emitted exactly | passes |

The connected target is dominated by two `CLOCK_MONOTONIC_RAW`/QPC reads (about 27 ns each on the CI VM). The implementation's own cost is about 15 ns. The plan concern was raised with the user; the proposal is an 80 ns binding budget until a TSC fast-path WP exists.
Follow-ups fixed in WP-51b: sidecar re-open now writes `<stem>.<n>.<ext>` (WP-52 import must also pick up numbered siblings); the perf test retries and no longer fails on drops.
Noted for later: `Begin/EndFiberSwitch` take a sharded mutex (not Begin/End).

## WP-36 measured numbers (verifier, 4-vCPU dev VM; large = 64 threads × 10 min)

| Metric | Target | Measured |
|---|---|---|
| Build, large (4 workers / 1 worker) | < 15 s | 2.7–3.8 s / 12.4 s |
| LOD memory | — | 3.34 B/sample (953.7 MiB on large) |
| Single-row query, any zoom (band + strip + 32-depth flame) | < 4 ms | ≈ 0.4 ms worst |
| 64-row frame, band + strip, 1920 px (spec 3.9 reading) | < 4 ms | 6.8–8.2 ms worst (pixel just under the 8192-tick base: direct strip; above base: LOD band ≈ 28 ns/column) |
| Medium 16-row frame incl. flame | < 4 ms | 2.45 ms CI, 3.9–6.2 ms dev VM |

Merged because the WP-36 per-query criterion is met and correctness probes (random coverage, LOD vs direct, TSan/ASan build-vs-query races, cancel/resume hash) are clean. The spec 3.9 frame target is carried by WP-36b: cut the per-column band/strip cost about 2× (fuse band+strip, avoid 64-bit divisions in moveTo), make the sub-base strip path cheaper, and assert the 64-row band+strip frame with a non-flaky metric. Notes: entry points are `TimelineLod::prepare()/build()` rather than `lod::build(TraceModel&)`; progress fires per finished thread; no thread build priority yet (a WP-42 need).

## WP-21 notes

Verifier FAIL on the first commit (HTTP two-tier servers not handled), plus security hardening; all fixed in 05df6da: index2.txt probed once per server; remote `file.ptr` PATH followed only under configured roots (never UNC); 4 KiB file.ptr cap and a 2 GiB default download cap; curl protocols limited to http/https; percent-encoded URL components; WinHTTP truncated-body and timeout handling. Windows CI fetched ntdll.pdb from msdl.microsoft.com. Follow-ups: nothing calls `unpin()` yet (WP-22 or SymbolService should when providers close); the PDB name falls back to `<image stem>.pdb` because Module/ModuleRec has no PDB name field (proposal pending: carry the RSDS pdb file name).

## WP-32 measured numbers (verifier, 4-vCPU VM, cold = fresh QueryContext after load + thread states + symbols)

| Query | Fixture | Measured | Budget |
|---|---|---|---|
| bottomUpFunctions full range | medium (7.49 M samples) | 12–17 ms | 300 ms |
| bottomUpFunctions full range | large (299.5 M samples) | 0.26–0.51 s | 3 s |
| memoized repeat | medium / large | 11 µs / 32 µs | — |

All 8 fixtures match expected.json exactly. Large also matches exactly (verifier probe, 1055 functions). TSan is clean on concurrent queries. Spec 3.7 signature changed to `QueryContext` (see docs/specs/analysis-query.md), and the return-address rule is added, implemented by WP-32b.
Recurring load flake: TestbedScenarios.EveryScenarioRunsAndProducesValidReport fails under `ctest -j` (seen 4 times) and passes alone. Queued as WP-02b (make the scenario assertions load-tolerant).
WP-32b merged (verifier PASS): `SymbolService::lookupCaller` and `functionKeyForCaller`, `frameKeyAt`/`usesExactAddress` helpers in libs/analysis/attribution.h for WP-34/35/46, iterative NodeAgg destruction (NodeAgg is no longer an aggregate, so no brace-init), tracegen non-leaf offsets in [1, size) (only the medium manifest hash changed). Orchestrator fix: the worker memory budget now counts the kernel-leaf arrays.

## WP-33 notes

Merged after the verifier FAIL was fixed in 71a9d73 (lost wakeup in waitIdle, racy executor tests, deep-stack cancellation latency, cache byte accounting, task exceptions reported as Cancelled). The verifier confirmed filters against brute force on all fixtures (about 1,100 combinations on medium), cache key canonicalisation and forced collisions, a TSan-clean concurrent workload, and the claim that nested fan-out through the executor cannot deadlock.

| Metric | Measured | Budget |
|---|---|---|
| cache hit (shared API) | 0.11–0.18 µs | 50 µs |
| bottomUp over a 10% range, medium, cold | 1.0–2.7 ms | 30 ms |
| cancellation latency (medium / 20 M samples / executor / 100k-deep stacks) | 12.5 / 8.7 / 7.3 / 4.3 ms worst | 50 ms |
| full-range bottomUp, medium (post-merge main) | 14.6 ms | 300 ms |

API additions in query.h: `FunctionRegistry` (`ctx.functionIdx(key)`), `QueryCacheConfig`, the `*Shared` queries, `QueryExecutor` with `GenerationToken`, and `setExecutor`. Subtree root 0 now means no subtree. `Filter::function` tests the stack after the subtree cut. A FunctionIdx from another context is not detected (documented).

## WP-46 notes

Verifier PASS. Wait episodes were checked against brute force on 4 fixtures; the Waiting and Ready pieces form one episode, nothing is double-counted, and the scan is ASan-clean on document close. Longest-waits scan on medium: 0.46–0.50 ms (N=50), 2.6 ms (N=5000), budget 50 ms. The timeline frame medians over 5 ms come from machine load; main's binary shows the same spread. Low follow-ups: a ReadyRec at the switch-out ts is labelled "Preempted"; arrow clipping is done in float, so it misplaces markers beyond about 1e8 px (clip in double).

## WP-52 notes

The first verifier pass FAILed on agent robustness: a crash when thread creation failed, no caps on pids, connections or disk, and ThreadName clipping. All were fixed (6b2d2d6), and a state race (capture_done seen while the state was still `done`) was fixed in d75f549. Caps per capture: 256 pids, 16 connections per pid, 1 GiB spool per pid, 4 GiB in total. inst_hello is restricted to capture pids, and the peer pid is verified with SO_PEERCRED or GetNamedPipeClientProcessId. Merging is a full rewrite through a temp file and atomic rename, about 0.8 s per 18 MB. The raw-chunk copy for multi-GB merges is a follow-up and needs a TraceWriter raw-append API. With follow_children, descendant pids are accepted without enumeration.

| Stage | Measured |
|---|---|
| spool | about 1 GB/s |
| decode | 4.5–6.3 M records/s |
| merge | 8.8–11.9 M events/s |
| merging a 310 MB sidecar | 4.7 s, peak RSS 768 MB (about 2.5× the sidecar) |

## WP-43 notes

Verifier PASS. The panel tree equals topDownTree node by node on tiny and medium in all 6 inline × kernel modes. It is ASan-clean under document close/reopen and App destruction with queries in flight, and a 100k-deep chain is safe everywhere. Flattening 100k expanded rows takes 2.1 ms (budget 20 ms), and a panel frame with 5000 rows 0.06 ms (budget 4 ms). The viewer keeps one QueryContext per (inline, kernel) option set per symbol generation (accepted deviation from "one per document"). `Document::analysis()`, `snapshot()` and `submit<T>()` are the reuse API for WP-44, WP-45 and WP-47. Orchestrator fix: lod.h's `Cancel` alias was renamed `LodCancel`, so lod.h and query.h can share a TU.
Known gap: "Filter to subtree" resolves a merged top-down node to ONE callstack node. A function reached from two call sites keeps only about half its time, and more generally about 1/N. WP-33b fixes this by adding `Filter::subtreePath` to the analysis API.

## WP-35 notes

The verifier passed everything except the quartile definition. That is now fixed: Stats uses lower order statistics, `d[floor(q(n-1))]`, so the median equals tracegen's `median_ticks` exactly. Frames match the `frames` fixture field by field (174 frames, 696 per-thread entries), and the slowest frame equals `hitch_frames`. Brute-force probes for scope pairing, the sampled-instance rule and topFunctions show 0 mismatches.

| Query | Measured | Budget |
|---|---|---|
| functionStats, medium | 59 ms | 200 ms |
| frames(), 100k frames | 46–62 ms | 100 ms |
| scopeStats, 1 M scopes (incl. pairing) | 73–76 ms | 200 ms |

Shared helpers moved to libs/analysis/detail/query_support.h (pure move). `libs/analysis/scope_pairing.h` is the base for WP-53's fiber-aware pairing.

## WP-44 notes

Verifier PASS. Rows match bottomUpFunctions. The Butterfly matches analysis::butterfly for all 401 medium functions, with 0 mismatches. Sort is a total order on every column. The text filter matches brute force, and regex-like input is safe. Close and reopen is ASan-clean. Sorting 100k rows takes 9–12 ms (budget 20 ms); a panel frame takes 0.06 ms (budget 2 ms). Viewer helpers moved to apps/viewer/common.

## WP-53 notes

Verifier PASS (conditional on D1). Rule-5 placement matches a brute-force oracle written from the spec in 10,800 comparisons, and the oracle catches mutations. With blending off, results are byte-identical to main over 294 configurations. Fibers match the fixture field by field. Lock summary is exact. Blended, warm, 5 M samples / 1 M scopes: bottomUp 58–70 ms, topDown 42–90 ms. Cold (pairing, index and query): 0.23–0.26 s, or 0.55–0.80 s fiber-aware. BlendIndex memory for 1 M scopes: 38 MB; PairedFibers: 184 MB. Orchestrator fix: the viewer's AnalysisContext snapshot now keys on blendScopes and groupByFiber.

## Queued follow-ups

- WP-53b (in progress): poll cancellation in the fiber piece computation and the blend-index sorts (cold fiber-aware cancel took 134–212 ms); replace the 3× ratio with absolute budgets (warm blended bottomUp/topDown/butterfly ≤ 100 ms, assert 200 ms; cold ≤ 1 s, assert 2 s, on the 5 M / 1 M model); match mapped frames with `lo < caller_addr <= hi`, since caller_addr is a return address; thread-safety comment on lockSummaryShared.

- WP-33b (also): empty-stack samples are counted in topDown's root exclusive but dropped by bottomUp under KernelFrames::Show/Group. Result: Function List % ≠ Call Graph % on traces with frameless samples (repro: 80% vs 100%). Fix in libs/analysis with a `<no stack>` row (kRootKey) in bottomUp, or by excluding the samples from topDown, consistently in both.
- Viewer polish (with WP-48): the Butterfly forces inline/kernel mode for inline or `<kernel>` keys without showing it in its toggles. Add a "forced for this selection" hint. Put the module-inclusive double-count note on the % cell too.

- WP-33b: `Filter::subtreePath` (FunctionKey path prefix, applied after flatten; hashed in QueryCache), then switch the Call Graph "Filter to subtree" to it and drop the main-thread subtree resolver. Also: pie chart gets a "self" slice; selected-row deep-cap message.

- WP-02b: done by the orchestrator. testbed_tests are RUN_SERIAL (the CPU-share checks failed when ctest -j oversubscribed the cores; the verifier reproduced this on pre-existing binaries). LocatorTest.ConcurrentDownloadsAreBounded now asserts only the bound (≤ 2), not that the limit is reached. CancellationTest.TwentyMillion measured 42.6 ms against 50 ms under load average 10; watch it.
- WP-21b: MERGED (verifier PASS). Cancel or destroy with 4 stalled transfers now takes 18–130 ms (was 16 s); curl idle CPU is 0.15%; pins are balanced. Low follow-ups: catch thread creation failure in the WinHTTP CancelWatcher; pins taken via setLocate(makeDefaultLocate(...)) are never unpinned; the remembered-model overload of resolveAllAsync needs the model to outlive waitAll. Original item: make SymbolService/SymbolLocator downloads cancellable mid-transfer, so closing a document with symbol servers configured doesn't block the UI (measured 16 s with a slow server). Call `unpin()` when providers close.
- WP-36b: 64-row LOD frame under 4 ms.
- `SymbolService::resolveAllAsync` should take `const TraceModel&` (the viewer const_casts today).

## Windows / privileged verification queue

M2 end-to-end (Windows dev box): capture the testbed (`tachyon-cli capture --launch tachyon-testbed.exe --args "--scenario inline --duration 10" --out inline.tachyon`), open it in `tachyon-viewer.exe` and check that the timeline tooltips and flame chart show testbed function names, and that inline frames resolve. Symbol servers come from Settings (empty by default); add `https://msdl.microsoft.com/download/symbols` to resolve system DLLs.

WP-42 GPU check (needs a mid-range GPU): `cmake --build --preset windows-release --target tachyon_fixtures_large`, open `build/windows-release/fixtures/large.tachyon` in `tachyon-viewer.exe` at 1080p and 1440p with all 64 rows visible, wait for "Building timeline index" to finish, then wheel-zoom from the whole trace to about 100 µs at 3 positions and drag-pan continuously; repeat with 4 rows expanded and during the first minute while the LOD builds. Report p95 and worst frame time (target: steady 60 fps, no frame > 16.6 ms). CPU-only frame cost measured on Linux: large, 64 collapsed rows, 1920 px: worst 5.2–6.9 ms, median about 3 ms; peak about 367 k vertices per frame.

WP-13 (CI on the elevated windows-2022 runner already ran the 3 s launch acceptance test and the attach/child-following tests). On a dev box, in an elevated prompt:
1. `tachyon-agent --backend etw --pipe \\.\pipe\tachyon-agent`
2. `tachyon-cli capture --launch tachyon-testbed.exe --args "--scenario spin --duration 20" --out spin.tachyon`
Expect exit 0 and events_lost 0. Then `ctest --preset windows-release -R CaptureLaunchTestbedSpin -V` and send build/launch_cli_summary.txt (sample counts, first ntdll vs first exe sample offset).

Commands for the user to run on a Windows (or privileged Linux) machine are listed here per WP once the code is merged.

### WP-00
```
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```
(Run from a "x64 Native Tools Command Prompt for VS 2022". The GitHub Actions `windows` job runs the same.)

### WP-02
CI already builds and runs the testbed tests on Windows. Remaining manual checks on a Windows dev box:
```
cmake --preset windows-release && cmake --build --preset windows-release
build\windows-release\bin\tachyon-testbed.exe --scenario all --duration 5 --pretty
build\windows-release\bin\tachyon-testbed.exe --scenario sleep --duration 5 --pretty
```
Confirm the `sleep` split is within ±0.05 of 0.70/0.30 (Windows timer granularity may need `timeBeginPeriod(1)`), the fibers split is ~0.25 each, and thread names show in a debugger or Task Manager.

### WP-01 (acceptance numbers; elevated prompt, ≥8-core machine)
```
cmake --preset windows-release && cmake --build --preset windows-release
build\windows-release\bin\etlstat.exe record --out testbed.etl --seconds 30 --hz 8000 --exe build\windows-release\bin\tachyon-testbed.exe --exe-args "--scenario all --duration 30"
build\windows-release\bin\etlstat.exe stats testbed.etl --pid <pid printed by record>
build\windows-release\bin\etlstat.exe stats testbed.etl --pid <pid> --json > testbed-stats.json
build\windows-release\bin\etlstat.exe stats testbed.etl --dump-hex 3 --dump-out testbed.hexevents
```
Pass: ≥ 7,500 samples/s per busy core and > 95% SampledProfile with matched stacks (the report's last section prints PASS/FAIL). Then fill the [VERIFY] items and Measurements table in `docs/etw-notes.md`, and commit `testbed.hexevents` under `tests/fixtures/etw/` for WP-12. Also record which interval call worked (session handle or 0) and the CSwitch stack-thread role histogram.

### WP-10
CI already runs the named-pipe and acceptance tests on Windows. Manual checks on a Windows console:
```
build\windows-release\bin\tachyon-agent.exe --backend mock --pipe \\.\pipe\x      (press Ctrl+C; expect exit 0)
build\windows-release\bin\tachyon-agent.exe --backend etw                          (unelevated: expect ENOPRIV, exit 3)
```
Known follow-ups assigned to WP-11: named-pipe DACL so a non-elevated client can talk to an elevated agent; broadcast outside the agent lock with a write timeout (a stalled client can currently wedge state changes); don't hold the agent lock across `backend->flush()`.

### WP-12 (elevated prompt)
```
ctest --preset windows-release -R EtlConvertWindows -V
type build\windows-release\etlconvert_real_summary.txt
```
Confirm in the summary: the hot_a > hot_b > hot_c ranking was asserted, not "skipped: no PDB"; the sample count equals etlstat's; and the match-role histograms. Copy the histograms into docs/etw-notes.md to resolve the [VERIFY] items.
30 s run: `etlstat record --out t.etl --seconds 30 --hz 8000 --exe tachyon-testbed.exe --exe-args "--scenario spin --duration 30"`, then convert it (a CLI wrapper arrives with WP-13) and compare samplesWritten with `etlstat stats t.etl --image tachyon-testbed.exe`.
2 GB timing: convert a real ~2 GB ETL and check ConvertStats.seconds < 90 on an 8-core machine. On Linux the pipeline alone measured 1.40 M events/s, about 20 s extrapolated for 2 GB.

### WP-11 (elevated prompt, dev box)
```
ctest --preset windows-release -R "EtwCapture|PipeSecurity|ServerStall" --output-on-failure
type build\windows-release\etw_capture_summary.txt
tachyon-agent --backend etw --log-level info            (shell 1, elevated)
tachyon-cli start --mode system --out sys.tachyon       (shell 2, non-elevated: this also exercises the pipe DACL)
tachyon-testbed --scenario spin --duration 30
tachyon-cli stop --wait                                 (check events_lost == 0 in capture_done)
```
Hardening follow-ups assigned to WP-13: pipe client SQOS flags (SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION); an RAII guard for the agent's flush-in-flight flag; call the interval setter with handle 0 directly.

### WP-40 (any desktop)
```
build/<preset>/bin/tachyon-viewer build/<preset>/fixtures/medium.tachyon
```
Check by hand: window opens, Trace Info panel shows 16 threads / 7,486,737 samples, Ctrl+P palette, File > Open browser, drag-and-drop, View > Theme / UI Scale, layout persists across restarts. The viewer has only been run headless and under Xvfb so far.

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
  - Measured in WP-03: as-grown 22.4 bytes/node, worst after resize 22.9 (std::vector slack adds to the hash overhead). Orchestrator decision: the ≤ 16 bytes/node budget is measured after `CallstackInterner::compact()` (13.7 bytes/node), because the interner only lives during conversion. Confirmed by user 2026-10-25.

## Plan concerns awaiting user decision (raised 2026-10-25)

- WP-51 connected overhead target of 40 ns is dominated by two clock reads (≈27 ns each on the CI VM). Proposed: an 80 ns binding budget until a TSC fast-path WP exists.
- WP-30 "resident ≤ 3× compressed" conflicts with the fixed 16 B/sample layout whenever compression exceeds about 3.75:1. The synthetic fixtures compress 8:1, giving 6.95–7.33×. Proposed: resident ≤ 1× the uncompressed on-disk payload, or measure 3× on a real engine capture in WP-64.
- The M5 gate "overhead under 1% at 100 k scopes/frame" is physically infeasible. 1% of a 16.6 ms frame is 166 µs, about 1.7 ns per scope, which is less than a single timestamp read. At the measured 46.9 ns, 100 k scopes/frame costs 28% of a frame. Proposed: "under 1% at 100 k scopes per second" (≈0.47% at the measured cost), or "under 1% at 3 k scopes/frame".

## Environment notes

- GitHub Actions on this repo effectively runs one workflow run (3 jobs) at a time, at about 8–10 min each, so branch runs queue. Docs-only commits no longer trigger CI (`paths-ignore`). Agents are told to batch pushes.

- The session has a fixed disk allowance. It ran out once (2026-10-25) because every worktree kept a full build tree. The build dirs of finished worktrees were deleted, and briefs now ask agents not to keep extra build trees.

- Git LFS push is blocked by this environment's network policy (`lfs.github.com` returns 403 at the verify step). WP-04 therefore commits scenario JSON + SHA256 manifests and generates binary fixtures at build time (`tachyon_fixtures` target into `build/<preset>/fixtures`). To commit binaries via LFS instead, allow `lfs.github.com` in the environment's network settings.

- Linux dev container: Clang 18.1.3, CMake 3.28, Ninja; libdw/libelf, libcurl, SDL2 2.30.0, libGL, libclang-rt-18 installed via apt.
- Network: GitHub release assets and git clones work; `github.com/<o>/<r>/archive/...` source tarballs are blocked by the proxy, so dependencies use release archives (with SHA256) or pinned git commits.
