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
| WP-34 | Line/instruction attribution, disassembly | C | merged (WP-34b merged; WP-34c call-site field) | wp/34-lines-disasm | 2026-10-25 |
| WP-35 | Function and scope statistics, frames | C | merged | wp/35-stats-frames | 2026-10-25 |
| WP-36 | Timeline LOD | C | merged (frame target follow-up in WP-36b) | wp/36-lod | 2026-10-25 |
| WP-33b | Subtree path filter; bottomUp/topDown frameless consistency | C | merged (verifier PASS) | wp/33b-subtree-path | 2026-11-03 |
| WP-41b | Elevated agent (pkexec/sudo) usable by the invoking user | A | merged (with WP-61) | wp/61-remote-capture | 2026-11-03 |
| WP-36b | LOD 64-row frame under 4 ms; non-flaky frame assert | C | merged (partial: target carried to WP-64) | wp/36b-lod-perf | 2026-10-25 |
| WP-40 | Viewer shell | D | merged (headless selftest + Xvfb verified; not yet run on a real display) | wp/40-viewer-shell | 2026-10-25 |
| WP-41 | Session explorer and capture dialogs | D | merged (verifier PASS) | wp/41-session-explorer | 2026-11-03 |
| WP-42 | Threads timeline | D | merged; needs GPU verification (60 fps) | wp/42-threads-timeline | 2026-10-25 |
| WP-42b | Timeline fixes + viewer SymbolService plumbing (real FunctionKeys) | D | merged | wp/42b-timeline-symbols | 2026-10-25 |
| WP-43 | Call graph panel | D | merged | wp/43-call-graph | 2026-10-25 |
| WP-44 | Function list and butterfly | D | merged | wp/44-function-list | 2026-10-25 |
| WP-45 | Source and disassembly panel | D | merged | wp/45-source-panel | 2026-11-03 |
| WP-46 | Thread interaction | D | merged | wp/46-thread-interaction | 2026-10-25 |
| WP-47 | Search and Function Statistics | D | merged | wp/47-function-stats | 2026-10-25 |
| WP-48 | Status, warnings, symbol progress | D | merged | wp/48-status-warnings | 2026-11-02 |
| WP-50 | Public SDK header and loader | E | merged (CI green incl. windows-release; caller-address test skipped on MSVC, no labels-as-values) | wp/50-sdk-header | 2026-10-25 |
| WP-51 | Client runtime | E | merged (CI green incl. windows; connected overhead 46.9 ns Linux / 68 ns Windows vs 40 ns target, under 80 ns gate; disabled 4.7 / 16 ns) | wp/51-sdk-runtime | 2026-10-25 |
| WP-51b | SDK runtime fixes (flaky AgentLossFallsBack test, sidecar truncation, perf flake) | E | merged (root cause: fake-agent race; 20× ctest repeats green under load) | wp/51b-sdk-fixes | 2026-10-25 |
| WP-52 | Agent-side ingestion and merge | A | merged (Windows CI ran a real ETW + --instrument capture) | wp/52-ingestion | 2026-10-25 |
| WP-53 | Scope pairing, blending, fibers | C | merged (WP-53b follow-up) | wp/53-scope-blending | 2026-10-25 |
| WP-54 | Viewer instrumentation lanes, frame graph, plots | D | merged | wp/54-inst-viewer | 2026-11-02 |
| WP-55 | Engine integration guide and samples | E | merged (CI green; verifier accuracy findings fixed by orchestrator) | wp/55-integration-guide | 2026-10-25 |
| WP-60 | Flight recorder mode | A | merged (verifier: one test bug, fixed by the orchestrator) | wp/60-flight-recorder | 2026-11-03 |
| WP-61 | Remote capture | A | merged | wp/61-remote-capture | 2026-11-03 |
| WP-62 | CI report tool | E | merged (WP-62b follow-up) | wp/62-ci-report | 2026-11-02 |
| WP-63 | Exporters | E | merged | wp/63-exporters | 2026-11-03 |
| WP-64 | Large-trace performance pass | C | merged (partial: 4 ms LOD frame, flame-on-4-rows and WP-54 median budgets not met) | wp/64-perf-pass | 2026-11-03 |
| WP-66 | Agent, remote and flight-recorder hardening (WP-60/61/73/QA1 follow-ups) | A | merged | wp/66-agent-hardening | 2026-11-03 |
| WP-67 | Viewer polish and correctness follow-ups (WP-41/48/33b/64) | C | merged | wp/67-viewer-polish | 2026-11-03 |
| WP-65 | Real-time live view (Linux + Windows; split 65a core / 65w ETW / 65b viewer; design in docs/specs/live.md) | A/W/C | in progress (65a) | wp/65a-live-core | 2026-11-08 |
| WP-68 | Viewer performance pass 2 (LOD direct zoom, flame, WP-54 draw lists, single-decode load, preview symbol reuse) | C | merged (partial: frame targets remain exceptions) | wp/68-perf-pass2 | 2026-11-08 |
| WP-69 | Report/export/doctor follow-ups and test flakes (WP-62b, symbol-setup helper, Windows doctor) | D | in progress | wp/69-tooling | 2026-11-08 |
| WP-70 | `perf_event_open` backend | A | merged; privileged tests verified in this container (CI step non-gating) | wp/70-perf-backend | 2026-10-25 |
| WP-71 | Scheduler events | A | merged (tracepoint path verified on CI's privileged step) | wp/71-sched-events | 2026-11-02 |
| WP-72 | DWARF symbol provider | B | merged (WP-72b follow-up) | wp/72-dwarf-provider | 2026-10-25 |
| WP-73 | Linux SDK transport and privileges | A | merged (verifier PASS; privileged SDK socket test ran on CI) | wp/73-linux-sdk-doctor | 2026-11-03 |

## Milestone gates

| Gate | Result | Date | Notes |
|---|---|---|---|
| M0 | passed with exceptions | 2026-10-25 | CI green on Linux and Windows. etlstat numbers pending the user's Windows run (WP-01). Fixtures are generated at build time and checked against SHA256s, because LFS push is blocked. Trace round-trip/interning budget: skewed met (8.66 s), uniform 11.99 s against 10 s, memory met after compact. |
| M1 | passed for the testbed; engine part pending WP-14 (user-assisted) | 2026-10-25 | Windows CI (elevated runner): `tachyon-cli capture --launch tachyon-testbed.exe` gives a trace with 0 lost events, loader samples before main, child following and attach; the trace opens in TraceReader. Engine capture and conversion-time budget on a real engine capture need the user (WP-14). 20 s dev-box run queued below. |
| M2 | passed with exceptions | 2026-10-25 | Windows CI: DIA resolves testbed hot functions to file:line and the inline scenario to inline chains (every frame has a line); 1 M RVAs in about 4 s (budget 20 s); ntdll.pdb fetched from msdl.microsoft.com. No team symbol store yet (Part 7 default), so the team-store fetch is untested. An end-to-end symbolized testbed capture in the viewer on Windows is queued for the user. Known limits: hot/cold separated blocks resolve only when the parent's length covers them; an unbounded last public symbol claims trailing addresses. |
| M3 | passed with exceptions | 2026-11-03 | All analysis WPs (30–36, 36b) merged with fixture expectations and query budgets met; the API changes in 3.7 are in docs/specs/analysis-query.md. Exception: the spec 3.9 frame target (64-row band+strip under 4 ms on `large`) measures 5.3–6.6 ms on the 4-vCPU dev VM (asserted at 2×, passing); medium meets it (0.8 ms CI). Carried to WP-64. LOD memory 4.6 B/sample. After WP-64: 4.8 ms worst zoom at 6.26 B/sample; the exception stands. |
| M7 | passed | 2026-11-03 | Linux testbed captures: spin leaf shares 38/29/19/10% vs expected 40/30/20/10% (WP-70 verifier, within ±5%); lock scenario on CI with sched tracepoints: 5,813/5,813 waits woken by the holder (WP-71); SDK scopes over the Unix socket with the perf backend on CI (WP-73). `tachyon-cli doctor` checks the host. |

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

## WP-72 notes

Verifier PASS. An independent llvm-symbolizer oracle on 2000 testbed and 2000 libc addresses shows 0 chain-length, function-start, file or line mismatches; the remaining differences are demangler style or an intended line fallback. Fuzzing at scale 10 under ASan is clean, TSan is clean with 8 threads, and memory is about 4–7 KB per function. 1 M RVAs resolve in 0.16 s warm (budget 20 s).

## WP-34 notes

Verifier PASS. A randomized independent oracle ran 9,600 checks with 0 mismatches: running/ready/waiting split, recursion, the return-address rule, blocking-stack attribution and filters. On medium, the sum of line runningTicks equals inclusive time for 202 functions. Disassembly agrees with objdump on 44,849 + 213 instructions (counts and boundaries identical, only cosmetic spelling differs), and readImageBytes matches `objdump -s` byte for byte. The decoder is fuzz-clean under asan. The Windows test disassembled hot_a with real DIA and sample hits landed inside its bounds. Timings: linesForFunction 22 ms (budget 100 ms); disassembling 64 KB takes 4.1 ms (budget 20 ms).

## WP-47 notes

Verifier: the core passed. Stats are byte-identical before and after the refactor over 27,636 results × all option variants, including the WP-53 port. Instances match Stats in 2,352 checks. Fuzzy ranking matches a brute-force top-200, and the bus highlight is O(1) per publish. The verifier's FAIL items were all fixed in 1e67988: the merge conflict, flaky overlay asserts, whitespace and UTF-8 matching, wide instances dropped in dense cells, highlight cleanup on panel close, and the current-instance outline. Fuzzy search over 200k names: worst keystroke 11.9 ms (budget 15 ms); the pathological all-subsequence query takes about 50 ms. Overlay with 3.2 M instances: median about 10 ms (asserted < 16 ms). Follow-ups: closing the panel from the View menu does not clear the highlight (needs an App close hook); the InstanceIndex for a huge list is built on the UI thread (about 200 ms, once).

## WP-70 notes

Verifier PASS. Privileged tests ran for real in this container (root with CAP_PERFMON): testbed spin over 3.16 s gave 24,799 target samples at 7,844/s with events_lost 0; 19–28 samples landed before main; leaf shares hot_a/spin_rest/hot_b/hot_c are 38/29/19/10% (expected 40/30/20/10). Module bases equal /proc maps (ELF rule). The kernel-leaf flag had 0 mismatches. 20k-iteration parser fuzzing under asan was clean; no fd leaks into the child; LOST and RLIMIT errors are clear. Conversion runs at 0.9–1.2 M samples/s (skewed) and 73–90 k/s (all-unique). CI has a new non-gating step that enables perf_event_paranoid=-1 and runs `-L linux_privileged`, failing the step if the tests were skipped. No PMU or tracefs here, so WP-71 sched tracepoints can only be tested on CI or a real host.

## WP-54 notes

The verifier passed it; the implementer's report was lost in a container restart. Lane intervals match pairScopes and pairScopesByFiber exactly, frame bars match frames() (max per pixel, over-budget highlighting), and plot min/max per column matches brute force, so single-sample spikes survive. Scope clicks drive Call Graph and Function Statistics. ASan lifetime tests are clean. Perf: medians run at 1.0–1.55× budget, all under the 2× asserts (1M scopes × 64 rows: 6.5–8.5 ms against a 6.0 ms budget; 100k frames: 5.1–8.5 ms against 5.5 ms; 10M plot samples: 6.7–8.5 ms against 6.5 ms). Follow-ups for WP-64: per-pixel merged items take flags from their first interval only, so a lock or unterminated scope mid-group is not marked; the pending look reserves a line in rows without scopes.

## WP-48 notes

The verifier passed it with no blocking defects. The release and ASan builds each passed 1742/1742; the Status* tests passed ×10 under ASan; banner parsing was probed against crafted CaptureInfo; a SymbolService rebuild with 20 rapid retries was ASan-clean. Orchestrator fix: Retry/Add now cancels the old SymbolService before starting a new one, because snapshots kept the old run loading providers in the background.

## WP-62 notes

The verifier passed it. Frame percentiles (lower order statistic), per-thread state totals, top functions and scope stats match the fixtures' expected.json exactly. Analysis on medium takes 0.18–0.23 s against a 2 s budget. The baseline diff exit codes, malformed-input handling and determinism were all verified: output is byte-identical across runs and working directories, with no absolute paths. Orchestrator: completed the build/test/push when the implementer stopped, and fixed an MSVC C4996 (getenv) error in a test.

## WP-63 notes

Verifier PASS. The speedscope output validates against the official schema (vendored, byte-identical to the live one) on 7 fixtures, and per-function ns equal the generator's expected ticks. Chrome traces are well formed (M/X/i/C only, ordered per track, metadata complete), scope counts and durations match expected.json, and CPU slices sum to Running ticks. Robustness covers names, ids, ranges and exit codes. On medium, speedscope takes 0.35 s and produces 472 MiB (inherent to the sampled format); Chrome takes 0.023 s for 6.4 MiB. Orchestrator fix: a failed export only deletes `--out` when it is a regular file (it deleted /dev/full in the verifier's probe).

## WP-71 notes

The verifier found a duplicate-ThreadRec defect (events after PERF_RECORD_EXIT re-created the thread). It was fixed and confirmed on CI: one ThreadRec per tid and no tid 4294967295. The fix also covers WP-70 captures. Real tracepoint numbers from CI's privileged step (lock scenario, 3 s): 5813 waits, 5813/5813 woken by the holder with unlock-path stacks in the testbed, every wait reason S (futex), 0 malformed records. Format-parser fuzzing (400k mutations, asan) was clean. Per-thread plans use the context_switch fallback (no wait reasons or wakers).

## WP-45 notes

Verifier fix round complete; CI green (Windows job re-run once for the known SdkOverheadPerf 10 ns flake, queued to WP-QA1). Search highlight is now linear: a 1 MB line scrolled far right draws in 0.02 ms (budget 2 ms). Locate-file rules are checked to resolve to the chosen file, falling back to an exact-path rule. In mixed mode, sampled post-call rows are listed under the call's line (`InstructionRow::groupLine`); a leaf sample on a post-call instruction is credited to its own line by `linesForFunction` but listed under the call's line here (documented). PathPicker takes a title and hint. 50k-line file: worker open about 10 ms, panel frame about 0.07 ms. DIA source test in tests/windows/viewer needs Windows verification.

## WP-73 notes

Verifier PASS. `tachyon-cli doctor [--json] [--hz N] [--agent PATH] [--socket PATH]` runs 15 Linux checks with PASS/WARN/FAIL and fix hints (exit 0 / 1 on FAIL / 3 usage); Windows prints a note. Docs: apps/agent/LINUX.md, apps/cli/README.md. CI's privileged step ran SdkSocketPerf for real (real agent with the perf backend + instrumented testbed: events_lost 0, balanced scopes, no sidecar). Accepted deviation: `hasCapturePrivileges()` also accepts CAP_SYS_ADMIN and perf_event_paranoid <= 0. Open: the agent still refuses unprivileged users at paranoid 1–2 although the backend could do per-process captures (current behavior documented). Follow-ups: (done by the orchestrator: the agent ENOPRIV message lists every accepted privilege and points to `tachyon-cli doctor`); doctor treats a relative --socket as an unwritable directory and does not check the socket dir is 0700; ptrace_scope=1 PASS wording is optimistic; Windows doctor checks not implemented.

## WP-41 notes

Verifier PASS. Run→capture→stop→auto-open with the real agent (`--backend mock`) is tested (CaptureApp.RunCaptureStopOpensTheTrace); parked launch + Resume, killed agent → Lost + reconnect, Attach lists this pid, ASan-clean App destruction with a request in flight. Linux launching uses posix_spawnp with argv (no shell); Windows quoting follows CommandLineToArgvW. Orchestrator fixes: Delete refuses a directory named `*.tachyon`; viewer test TempDirs include the pid. Known gaps: an agent started with "Start with pkexec" runs as root and its 0600 socket (and the SDK's private inst-socket directory) is unreachable for the user → WP-41b; capture_done-only warnings (spool caps, refused connections, failed merges) appear as toasts, not in the banner (needs a hook in status/warnings_banner.*); `\\server\share` in the Run args line collapses one backslash; the Windows process list shows the image path, not the command line; Session Explorer capture time is the file mtime (CaptureInfo has no timestamp); `--selftest` makes one failing agent connect attempt (set autoConnectAgent=false there). Needs Windows verification: ShellExecuteExW runas (UAC), Toolhelp32 process list and user lookup, `explorer /select`.

## WP-36b notes

Orchestrator review. Column-wise band pass (one write per column) and a fine strip level for pixel spans in [base/2, base); perf test uses thread CPU time, best of 5 per zoom. Dev VM (interleaved A/B, large, 1920 px, thread CPU): 64-row band+strip worst 7.3–12.2 → 5.3–6.6 ms; +flame on 4 rows 7.5–11.3 → 5.0–5.7 ms; medium band+strip CI 1.62 → 0.82 ms; medium flame on every row CI 4.98 → 3.68 ms; viewer TimelinePerf medium ×4 CI 4.19/4.50 → 2.77/2.97 ms. Costs: 3.34 → 4.59 B/sample (+37%, over the +25% allowance; accepted because it removes the raw-sample scan that was the worst zoom; `fineStrip=false` disables it), large build 4 workers 3.1–3.5 → 3.8–4.9 s. Strip answers in [base/2, base) are now approximate (documented in lod.h and the spec).

## WP-33b notes

Verifier PASS. Call Graph "Filter to subtree" now filters by the key path, so a function reached via several callstack nodes keeps its full time (acceptance test on medium); the main-thread subtree resolver is gone. bottomUp has a "<no stack>" row, so Function List totals equal the Call Graph root in all 6 modes. Cost of a path vs a root filter on medium (7.5 M samples): cold 0.89–1.03×, warm within 0.1 ms. Pie chart has a "self" slice; a selected row beyond the 100k-row cap is reported. Orchestrator fix: the "Subtree filter cleared" notice was wiped by the next query result (test failed 2/20); it now persists until the next filter change (40/40). Follow-ups: `NodeChildIndex`/`AnalysisContext::childIndex()` are dead code; selecting "<no stack>" in the Function List may show no butterfly (butterfly_panel.cpp resets kRootKey); lines/instructions path matching is O(depth) per node, not memoized; the SubtreePath perf test uses small subtrees.

## WP-60 notes

Verifier: design and implementation pass; one blocking test bug (range-for over a temporary `Result` in the privileged test, an ASan stack-use-after-scope) fixed by the orchestrator, plus Windows device names in flush names. Acceptance on Linux (CI privileged step and locally, release and ASan): 7 flushes in 20 s, every one holds the 57 ms hitch frame and the `testbed.hitch` marker within its last 10 s, scopes balanced, full-span flushes have target samples over 9.98–9.99 s of the 10 s window (about 34k samples). Windows CI: hitch-triggered flushes under real ETW pass. Specs updated (control-protocol, capture-backend, trace-format) and engine-integration 5.9 rewritten (RequestFlush returns at once now). Follow-ups: StringDef prefix cap (8 MiB) has no test or user-visible warning, and a prefix near the cap makes segments rotate on every frame; a failed segment open leaves the ring spool permanently in error; perf segments with `lastTs == 0` are never released by flushes, and one huge `seconds` raises H for the rest of the capture (both bounded by circularMb); a coalesced trigger during a manual flush before any trigger returns an empty path; fiber threads are not balanced at window edges; the viewer cannot tell whether a capture started elsewhere is circular; ETW flush gap (restart per flush).

## WP-61 + WP-41b notes

Verifier FAIL, fixed by the orchestrator, then merged. (1) The handshake timeout was per read, so a client dribbling bytes without a newline held the 16 unauthenticated slots forever (remote DoS of the listener): `FramedConnection::readLine` now bounds the whole line (regression test TcpAuth.DribblingBytesDoesNotExtendTheHandshake; the reviewer's reproduction now drops all 16 at 5 s). (2) `applySocketOwner` used `chown` by path in a directory the target user owns (symlink swap → root chowns an arbitrary file): it now `lstat`s, requires a socket owned by the agent and uses `lchown`. Held under attack: no data or events before auth, connection caps, rate limit, fetch confined to produced files (`..`, absolute, symlinks, NUL, swapped-in symlink all refused), remote `start` output confined to `--remote-out-dir`, remote launch and `inst_hello` refused, token never logged or saved. Measured: fetching 72 MiB grows RSS by 3.0 MiB. Protocol in docs/specs/control-protocol.md. Needs Windows verification: Winsock path beyond CI loopback tests; token file permissions are not checked on Windows. Not exercised anywhere: IPv6.

## WP-64 notes

Verifier FAIL on one defect, fixed by the orchestrator, then merged: `fusedLevelPass` underflowed for a window entirely before the trace start and wrote past the output columns (heap overflow under ASan; not reachable from the viewer, which clamps its window). Regression test LodRowColumns.AWindowEntirelyBeforeTheTraceIsEmpty. The verifier fuzzed 80,744 fused queries against separate band+strip queries (7 fixtures, levelStep 1 and 2, after trimMemory too): all equal. Delivered: fused `rowColumns`, levelStep 2→1, streaming load (5% preview for files ≥ 64 MiB, then swap), memory ceiling with staged degradation, `tachyon-viewer --bench`, the two WP-54 follow-ups. Measured on the shared 4-vCPU VM (`large`): 64-row band+strip at 1920 px 7.2 → 4.8 ms worst zoom (direct strip), LOD-served zooms ~4.2 ms, +flame on 4 rows 4.8 ms (target 4 ms, asserted 8 ms); LOD memory 4.59 → 6.26 B/sample (1788 MiB); load 6.5–11 s (budget 60 s at 4 cores); first preview 1.1–1.9 s; cold bottomUp 0.66 s; resident 4744 MiB = 7.3× compressed, 16.6 B/sample. Viewer `--bench` on `large`, 64 rows: steady 2.0 / pan 4.3 / zoom 4.4 ms CPU p50 (p95 2.6 / 6.0 / 6.1). Not met: WP-54 medians (cost is ImGui draw-list building, e.g. 1M scopes 6.5–7.5 ms vs 6.0). Top offenders in the viewer's own capture: ZSTD decompression 24.6% (sample chunks decoded twice at load), LOD build ~31%, load scatter 8.6%. Needs Windows verification: memory reading via GlobalMemoryStatusEx.

## WP-67 notes

Verifier FAIL on one defect, fixed by the orchestrator, then merged: the document got its new SymbolService while the search-path scan worker ran, so analysis queries on executor threads could call const lookups while `resolveAllAsync` set the service up on the UI thread (TSan data race at library level). The service now reaches the document only right before `resolveAllAsync`; the document has none meanwhile. Also: the agent's "instrumentation: <w>" copy of a merge warning no longer shows next to the trace's "Instrumentation: <w>" (case-insensitive repeat check). Delivered: capture_done-only warnings in the banner keyed by trace path, lost/throttle free text deduplicated against the structured lines; symbol search-path scan on a worker, capped at 10,000 entries, unreadable directories reported; Windows argument splitting follows CommandLineToArgvW (`\\server\share` kept), POSIX style unchanged; `--selftest`/`--bench` never connect to an agent; a cancelled stream never shows its preview; selection and timeline row flags remapped by (pid, tid) on the preview swap; "<no stack>" gets a totals-only Butterfly; dead `NodeChildIndex` removed. Not done: reusing preview symbol results across the swap (needs a libs/symbols API; documented in app.cpp). Follow-ups: free-text dedupe matches "throttled" broadly (drops the perf agent's "lower sampling_hz" advice when the structured line exists); `remapThreads` keeps the highlight generation when it shrinks the list; PolishSymbolScan.AnUnreadableDirectoryIsReported skips as root (the verifier exercised the path as `nobody`). Needs Windows verification: native argument splitting in the Run dialog.

## WP-66 notes

Verifier PASS (no blocking issues). Orchestrator change from its findings: the trace writer still opens its output by name, so in a group/world-writable non-sticky `--remote-out-dir` a local user could swap the reserved file for a symlink during a capture and have the root agent write through it (N1), or get a swapped path recorded as produced (N2); the agent now refuses `--listen` for such a directory instead of only warning. Delivered: O_EXCL|O_NOFOLLOW reservation of remote and flush outputs with an identity re-check; token file opened once and checked on the fd (owner too); fetch compares the opened fd with the produced file's identity and caches the SHA-256; per-address connect deadlines; `capture --fetch` also fetches flushed traces; spool rotation floor, StringDef-cap warning, failed segment open recovered with a warning, new perf retention rule (5-minute horizon memory, untimed segments released), coalesced trigger into a manual flush returns its path; doctor (relative --socket, private socket dir WARN, ptrace_scope wording); testbed accepts a starved thread with a warning and the integration-test retry is gone (verifier: 5 × 34 integration tests, 0 failures). Specs: control-protocol and capture-backend WP-66 sections. Follow-ups: `TraceWriter::open` should open with O_NOFOLLOW or take an fd (then the directory refusal could relax); `noteProduced` should lstat the reserved path rather than canonicalize; the doctor socket-dir check WARNs for the /tmp fallback; Windows token file has no owner/mode check and the Windows merge temp file is opened by name. Needs Windows verification: safe_file.cpp (CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, GetFileInformationByHandle).

## WP-68 notes

Verifier FAIL on one defect, fixed by the orchestrator, then merged: the adopt source kept the preview's SymbolService alive past the preview→complete swap, so its still-running workers read the freed preview model's frame table (heap-use-after-free); the swap now cancels (joins) the preview's run before the preview document is released. Regression test StreamingApp.APreviewSymbolRunStillLoadingIsStoppedBeforeThePreviewIsFreed fails under ASan without the fix. Delivered: `SymbolService::adoptRun(previous)` with a shared string table and shared providers (finished Resolved/NotFound/Failed modules of an identical module table are taken over; on the swap 0 modules are loaded again); the load count pass decodes only the ts/tid columns of each Samples chunk (streaming zstd, validating fallback): `large` load wall 11.27 → 10.50 s (−7%), CPU 21.28 → 19.47 s (−8.5%), medians of 3 interleaved. Rejected: a quarter-width fine LOD level (4.8 → 4.72 ms, within noise, at 7.51 B/sample and 17.2 s build). Not done (no profiler in the container: `perf` is not installed): flame on 4 rows and the WP-54 draw-list budgets stay exceptions. Follow-up: if `onDocumentLoaded` runs twice before resolution starts, the adopt source is replaced by a service that never ran (safe, loses the optimisation).

## Queued follow-ups

- Shared symbol-setup helper: libs/report/analyze.cpp and apps/cli/export_command.cpp duplicate the locator, override, provider and resolve setup.
- WP-QA1: MERGED (tests only). Root causes: per-pid inst socket names collided between concurrent test processes (testAddress now uses a private per-process dir); perf sample floors assumed wall seconds instead of CPU seconds (now 0.5 × hz × measured CPU seconds, rings 8 MiB in tests); cancellation latency now measured as the query thread's CPU time (thread_cpu_time.h); integration tests take frame/wait expectations from the testbed report, assert no dropped markers, and retry only on the testbed's "measured shares sum to 0". CancelTest.ResolveAllAsync not reproduced (temp root hardened). Remaining flake items: (starved thread + retry: done in WP-66); the Windows `tachyon-inst-<pid>` pipe name is machine-global across test processes; SdkOverheadPerf.RuntimeDisabledBeginEndPairUnder10ns hit its 20 ns gate once on Windows; SdkSocketPerf failed once in about 80 verifier runs (cause not captured).

- WP-62b: the baseline diff compares top-N function shares only, so it misses a function rising from about 1% to about 4% in a flat profile. Fix: carry a larger internal function list in the baseline (all functions with at least 0.5% share), and compare absolute exclusive_ms and total CPU with thresholds. Also add notes for new or removed scopes and frame series, and a note when the current trace is partial.

- Viewer polish: DONE in WP-67 (banner dedupe, bounded background symbol scan, unreadable directory reported).

- CI: on 2026-11-02 the privileged perf step really ran its 6 tests on GitHub's runner (24,907 samples in 3.13 s, 12 before main). The kernel throttled sampling 138 times; that is expected at 8 kHz on a shared runner. The step stays non-gating for now.

- WP-70b: MERGED (orchestrator review; CI green). Original item: `warnings` in CaptureResult (capture_backend.h, appended at the end of the struct) plumbed into capture_done; block SIGPIPE around the launch release write; pre-check threads × cpus fds against RLIMIT_NOFILE (raise soft to hard, and name the count in the error); "no ETW capture is running" wording in the shared lifecycle.
- WP-34c: MERGED (orchestrator review; CI green). The DIA call site is best effort (last own-file line row at or before the address). Original item: add `callSiteFile`/`callSiteLine` (the physical function's call site of the outermost inline) to ResolvedAddress, filled by DWARF (`sites[n-1]`) and DIA; then `linesForFunction` uses it for exact inline call-site lines. (WP-34b found that providers put callee-internal positions on outer frames, so `inlines.back()` is NOT the physical call site.)

- WP-34b: use the exact inline call-site line (`ra.inlines.back()` when its file is the function's file; providers already put call-site lines on outer frames) instead of the nearest-preceding-row approximation; handle a same-file inlined callee; clamp `readImageBytes` size so a hostile image cannot cause a huge allocation (bad_alloc); bound the SourceLocator cache.
- SdkOverheadPerf on the Windows runner now consistently measures 95–110 ns (it was 68 ns on 2026-10-25; the SDK code is unchanged, so the runner is slower). The orchestrator set the Windows gate to 160 ns, which is 2× the proposed 80 ns binding budget, until the user decides the 40 ns plan concern. Linux stays at 80 ns.

- Test hygiene (orchestrator, done): the testbed share checks skip when the load average exceeds the core count; the cancellation-latency tests assert at 2× their 50 ms budget. Both only failed when parallel agents overloaded the 4-core dev box.

- WP-72b: MERGED. Piece-based bounds (every .text byte is inside its own bounds); DIA-style names; LRU line cache, 64 MiB per provider (libdw's own decoded copy is not bounded). 0 mismatches against llvm-symbolizer on 14,016 addresses. Original item: `functionBounds(rva)` returns the piece that contains rva (split functions); names are stripped of parameter lists to match DIA (prefer the DIE qualified name, and strip symtab names); cap and evict per-CU line rows.

- WP-53b: MERGED. Cold fiber-aware cancel now takes 10–33 ms (was 134–212 ms). Absolute blend budgets are met (warm bottomUp 45–70 ms, topDown 55–130 ms, butterfly 64 ms; cold 0.2 s and 0.4 s fiber-aware). caller_addr uses `lo < caller <= hi`. Orchestrator change: the latency test asserts at 2× its 50 ms budget, like the other perf budgets. Original item: poll cancellation in the fiber piece computation and the blend-index sorts (cold fiber-aware cancel took 134–212 ms); replace the 3× ratio with absolute budgets (warm blended bottomUp/topDown/butterfly ≤ 100 ms, assert 200 ms; cold ≤ 1 s, assert 2 s, on the 5 M / 1 M model); match mapped frames with `lo < caller_addr <= hi`, since caller_addr is a return address; thread-safety comment on lockSummaryShared.

- WP-33b (also): empty-stack samples are counted in topDown's root exclusive but dropped by bottomUp under KernelFrames::Show/Group. Result: Function List % ≠ Call Graph % on traces with frameless samples (repro: 80% vs 100%). Fix in libs/analysis with a `<no stack>` row (kRootKey) in bottomUp, or by excluding the samples from topDown, consistently in both.
- Viewer polish (with WP-48): DONE (hints and % cell note were on main; confirmed by WP-67).

- WP-33b: `Filter::subtreePath` (FunctionKey path prefix, applied after flatten; hashed in QueryCache), then switch the Call Graph "Filter to subtree" to it and drop the main-thread subtree resolver. Also: pie chart gets a "self" slice; selected-row deep-cap message.

- WP-02b: done by the orchestrator. testbed_tests are RUN_SERIAL (the CPU-share checks failed when ctest -j oversubscribed the cores; the verifier reproduced this on pre-existing binaries). LocatorTest.ConcurrentDownloadsAreBounded now asserts only the bound (≤ 2), not that the limit is reached. CancellationTest.TwentyMillion measured 42.6 ms against 50 ms under load average 10; watch it.
- WP-21b: MERGED (verifier PASS). Cancel or destroy with 4 stalled transfers now takes 18–130 ms (was 16 s); curl idle CPU is 0.15%; pins are balanced. Low follow-ups: catch thread creation failure in the WinHTTP CancelWatcher; pins taken via setLocate(makeDefaultLocate(...)) are never unpinned; the remembered-model overload of resolveAllAsync needs the model to outlive waitAll. Original item: make SymbolService/SymbolLocator downloads cancellable mid-transfer, so closing a document with symbol servers configured doesn't block the UI (measured 16 s with a slow server). Call `unpin()` when providers close.
- `SymbolService::resolveAllAsync(const TraceModel&)`: DONE (already on main; confirmed by WP-67).
- WP-61: the channel is unencrypted (no TLS dependency approved): trusted network or `ssh -L`. Rate limit keys on peer IP (a NAT shares a bucket). A root agent outside pkexec/sudo needs `--socket-owner`. Symbols for a remote trace do not resolve via `.gnu_debuglink` names. `lstat`+`lchown` has a hardlink window where `fs.protected_hardlinks=0`. The rest (token file, remote output, fetch TOCTOU, hash caching, connect deadlines, `capture --fetch` flushes) was done in WP-66.
- WP-64: (a) the direct-strip zoom (2–3 samples per pixel) is mispredict-bound: a second fine level needs the LodFine exactness test relaxed; (b) flame frames on 4 rows; (c) WP-54 median budgets: band/strip rect merging and per-column colour cost in draw-list building; (d) load decodes sample chunks twice (decode only the first bytes in the counting pass, ~40% of it); (e) a preview restarts symbol resolution when the complete document replaces it (needs a libs/symbols API for adopting a finished run); (f), (g) DONE in WP-67; (h) the 3× resident target (WP-30) is still unasserted (7.3× measured).

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
