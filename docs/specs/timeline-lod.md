<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.9. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.9 Timeline level-of-detail (`libs/analysis/lod.h`)
For each thread, precompute at load: (a) state bands at levels L0..Lk where level L buckets have width `2^L × 64 ticks`, storing per bucket the fraction of Running/Ready/Waiting and the dominant wait reason; (b) sample "mipmaps": for each level and bucket, the most frequent callstack id and its share, so the collapsed row can draw a representative color; (c) for expanded flame charts, per-depth run-length encoded spans built from consecutive samples sharing the same callstack prefix, merged when narrower than one pixel at the current zoom. Target: render any zoom level of a 10-minute, 64-thread capture in under 4 ms of CPU per frame.

## Clarifications added with WP-36b (2026-11-03, binding)

- Fine strip level: with `LodOptions::fineStrip` (default on; off when `forceBaseLevel` is set, the base level is L0, or the model has 2^24 or more callstack nodes) the build also stores, per half-base bucket (level L_base − 1), the packed sample-strip data (24-bit dominant leaf, 4-bit share, 4-bit coverage). In Auto mode `sampleStrip` answers pixel spans in [base/2, base) from it. Those answers are LOD approximations like every stored level; Direct mode and pixel spans below base/2 stay exact. Band and flame queries do not use it. A thread whose LOD is not built yet is answered by the exact direct path.
- Memory: about 4.6 B/sample on `large` with the fine level (3.3 B/sample without).
- Frame target measurement: thread CPU time, best of 5 per zoom, worst zoom, asserted at 2× the 4 ms target. On the 4-vCPU dev VM the 64-row band + strip frame on `large` is 5.3–6.6 ms (was 6.7–12.2 ms); the remaining gap is carried by WP-64.
