// Timeline level of detail (WP-36, Part 3.9, docs/specs/timeline-lod.md).
//
// TimelineLod precomputes, per thread, a pyramid of fixed-width time buckets and answers three per-frame
// queries (state bands, sample strip, flame chart) in time proportional to the pixel width, whatever the
// zoom.
//
// Conventions
//   * Trace range: [header.start_ts, header.end_ts). Bucket i of a level with width w covers
//     [start + i*w, start + (i+1)*w) clipped to the trace end ("effective width"). Level L has width 64 << L
//     ticks (Part 3.9: 2^L x 64).
//   * Sample extent: a sample at ts covers [ts, ts + sampleIntervalTicks), clipped to the next sample of the
//     same thread, to the trace range, and (for the flame chart) to nothing else. Zero-length extents (equal
//     timestamps) are ignored. A sample belongs to a bucket in proportion to its overlap, so every statistic
//     below is time-weighted. WP-42 draws a sample over exactly this extent.
//   * Callstack prefix at depth d: the ancestor of a sample's leaf node at depth d from the root; the root
//     (nodes[0]) has depth 0 and is never drawn, the outermost frame has depth 1. A sample whose leaf is
//     shallower than d has no prefix at d. An invalid or out-of-range callstack id counts as the root.
//   * Pixel columns: column c of a query over [t0, t1) with P columns covers
//     [pixelEdge(c), pixelEdge(c+1)) where pixelEdge(c) = t0 + floor(c * (t1 - t0) / P). The same grid is
//     used by every query and by the LOD and direct code paths. (t1 - t0) must be <= 2^47 ticks.
//
// Storage (decision 1 of the WP-36 brief)
//   Levels are stored only from a base level upwards, as dense arrays per level (struct-of-arrays), with
//   `levelStep` (default 2) level numbers between stored levels, up to the level with a single bucket. The
//   base level is the finest L with <= maxBaseBuckets buckets per thread whose bucket is also at least
//   minSamplesPerBaseBucket sample intervals wide (bounds LOD bytes per sample). Per bucket and level:
//     band   4 B  running / ready / waiting fraction of the bucket in 1/255 (unknown = the remainder) and the
//                 dominant wait category of the Waiting ticks (kNoWaitCategory when none);
//     strip  6 B  most frequent leaf callstack (by overlapped ticks, ties to the smaller id), its share of
//                 the sampled ticks (1/255) and the sampled fraction of the bucket (1/255);
//     flame  6 B  deepest common prefix node (LCA of all leaves of the bucket) and the deepest leaf depth.
//   i.e. 16 B per bucket, about 21 B per base bucket with levelStep 2. Dominant stacks are exact as long as
//   a bucket has <= maxStacksPerBucket distinct leaves (otherwise the lightest are dropped while merging
//   levels), LCA and depth are always exact.
//
// Level selection: the query level is the coarsest stored level whose bucket width is <= the tick span of
// one pixel ((t1 - t0) / P). When even the base level is wider than a pixel the answer is computed directly
// from statesByThread / the sample arrays (binary search + one sweep over the window), which is exact and
// costs O(samples in window) <= O(P * samples per base bucket). Direct and LOD results are consistent: at a
// zoom where a pixel column coincides with one bucket they are equal (fractions up to the 1/255
// quantisation); otherwise the LOD result assigns each bucket to the column containing its start (state
// band, strip: fractions are the tick-weighted average of those buckets, the dominant stack is the heaviest
// of the bucket dominants) or to every column it overlaps (flame chart, conservative), assuming a uniform
// bucket.
//
// Flame chart regimes. (1) A pixel at least one sample interval wide: per depth and pixel column the cell is
// Empty / Uniform / Mixed (see TimelineSpan) and spans lie on pixel edges; answered from the levels (LOD) or
// directly. (2) A pixel narrower than one sample interval: samples are wider than a pixel, so spans follow
// the samples exactly (runs of consecutive contiguous samples with the same prefix); runs shorter than a
// pixel are merged with neighbouring short runs; always computed directly, which is cheap because the window
// then holds few samples. Both regimes give spans that are sorted and never overlap within a depth.
//
// Incremental build: prepare() is cheap (geometry and the node table), build() fills the per-thread data on
// worker threads, setting a per-thread ready flag (release) as each thread completes. Queries are
// thread-safe, may run concurrently with build(), and for a thread that is not ready are answered
// directly when the zoom is below the base level (always correct, cheap) and otherwise return
// `pending = true` with default (unknown / invalid / no span) columns.
#pragma once

#include "analysis/wait_reasons.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace tachyon::analysis {

// Optional cancellation flag polled between work items; when it becomes true the call returns Cancelled.
using LodCancel = const std::atomic<bool>*;  // distinct from analysis::Cancel (query.h)

inline constexpr uint8_t kNoWaitCategory = 0xFF;  // BandColumn::waitCategory when nothing waited
inline constexpr uint8_t kDirectLevel = 0xFF;     // result `level` when the answer was computed directly
inline constexpr uint32_t kMaxPixelWidth = 1u << 16;
inline constexpr uint64_t kMaxQuerySpanTicks = 1ull << 47;

struct LodOptions {
  // Build workers; 0 means std::thread::hardware_concurrency(). The result does not depend on it.
  unsigned threads = 0;
  // Upper bound on base-level buckets per thread (memory/transient bound).
  uint32_t maxBaseBuckets = 1u << 20;
  // Lower bound on the base bucket width in sampleIntervalTicks (bounds LOD bytes per sample).
  uint32_t minSamplesPerBaseBucket = 4;
  // Test hook: if >= 0 the base level L is exactly this (bucket width 64 << L) instead of the rule above.
  int forceBaseLevel = -1;
  // Stored levels are base, base + step, base + 2*step, ... (1..4).
  uint32_t levelStep = 2;
  // Distinct leaf stacks kept per bucket while merging levels (exact below this).
  uint32_t maxStacksPerBucket = 32;
};

enum class QueryMode : uint8_t {
  Auto,    // LOD when the zoom allows it, else direct
  Direct,  // always compute from statesByThread / samples (reference path, slow for wide windows)
};

// One stored level.
struct LodLevelInfo {
  uint32_t level = 0;        // L: bucketWidth == 64 << L
  uint64_t bucketWidth = 0;  // ticks
  uint32_t bucketCount = 0;  // per thread
};

// Contents of one bucket, for tests and tooling.
struct LodBucket {
  uint8_t running = 0, ready = 0, waiting = 0;  // 1/255 of the effective bucket width
  uint8_t waitCategory = kNoWaitCategory;
  CallstackIdx dominant;      // invalid when the bucket holds no sample
  uint8_t share = 0;          // dominant ticks / sampled ticks, 1/255
  uint8_t coverage = 0;       // sampled ticks / effective width, 1/255
  CallstackIdx commonPrefix;  // LCA of all leaves (invalid when no sample)
  uint16_t maxDepth = 0;      // deepest leaf depth (saturated at 65535)
};

// ---- state bands ----
struct BandColumn {
  float running = 0, ready = 0, waiting = 0,
        unknown = 1;                       // sum is 1 (unknown includes time outside the trace)
  uint8_t waitCategory = kNoWaitCategory;  // WaitCategory with the most waiting ticks
};
struct StateBandQuery {
  ThreadIdx thread;
  uint64_t t0 = 0, t1 = 0;
  uint32_t pixelWidth = 0;
  QueryMode mode = QueryMode::Auto;
};
struct StateBandResult {
  bool pending = false;
  uint8_t level = kDirectLevel;     // L used, or kDirectLevel
  std::vector<BandColumn> columns;  // pixelWidth entries
};

// ---- sample strip ----
struct StripColumn {
  CallstackIdx stack;  // dominant leaf callstack, invalid when no sample touches the column
  float share = 0;     // dominant ticks / sampled ticks in the column
  float coverage = 0;  // sampled ticks / column ticks
};
struct SampleStripQuery {
  ThreadIdx thread;
  uint64_t t0 = 0, t1 = 0;
  uint32_t pixelWidth = 0;
  QueryMode mode = QueryMode::Auto;
};
struct SampleStripResult {
  bool pending = false;
  uint8_t level = kDirectLevel;
  std::vector<StripColumn> columns;
};

// ---- flame chart ----
// Regime 1 (pixel >= one sample interval): per depth d the columns are classified: Empty (no sample with a
// prefix at d touches the column), Uniform (every sample touching it has the same prefix node at d and none
// is shallower) or Mixed. Adjacent Uniform columns with the same node form one span, adjacent Mixed columns
// form one merged span; Empty columns separate spans. Hence spans are at least one pixel wide and their
// edges lie on pixel edges.
// Regime 2 (pixel < one sample interval): a regular span is a run of consecutive contiguous samples with the
// same prefix node at d, at its exact tick edges (clipped to the window); runs shorter than one pixel
// (ticks * P < t1 - t0) that are less than one pixel apart are merged into one merged span.
// In both regimes spans of one depth are sorted and never overlap.
struct TimelineSpan {
  uint32_t depth = 0;       // 1 = outermost frame
  uint64_t t0 = 0, t1 = 0;  // [t0, t1) in ticks, within the query window
  CallstackIdx node;        // prefix node at `depth`; kInvalidIdx for a merged span
  // 1 for a regular span. For a merged span: regime 1 the number of samples (of any depth) starting in
  // [t0, t1); regime 2 the number of samples (with a prefix at this depth) of the merged runs.
  uint32_t mergedCount = 1;
};
struct TimelineQuery {
  ThreadIdx thread;
  uint32_t depthBegin = 1, depthEnd = 1;  // depths [depthBegin, depthEnd); depth 0 (root) yields nothing
  uint64_t t0 = 0, t1 = 0;
  uint32_t pixelWidth = 0;
  QueryMode mode = QueryMode::Auto;
};
struct TimelineResult {
  bool pending = false;
  uint8_t level = kDirectLevel;
  std::vector<TimelineSpan> spans;  // sorted by (depth, t0)
  // Internal reuse buffers (capacity is kept between queries); contents are unspecified.
  std::vector<TimelineSpan> scratch;
  std::vector<uint64_t> edges;
  std::vector<uint32_t> startCounts;
  std::vector<std::vector<TimelineSpan>> bins;
};

class TimelineLod {
 public:
  TimelineLod();
  ~TimelineLod();
  TimelineLod(TimelineLod&&) noexcept;
  TimelineLod& operator=(TimelineLod&&) noexcept;
  TimelineLod(const TimelineLod&) = delete;
  TimelineLod& operator=(const TimelineLod&) = delete;

  // Thread-safe (no shared state). Computes the level geometry and the callstack node table (O(nodes)); no
  // thread is ready afterwards. `model` must outlive the result and its samples/states must not change.
  static Result<TimelineLod> prepare(const model::TraceModel& model, const LodOptions& options = {});

  // Not thread-safe against another build() of the same object; concurrent with queries and the accessors.
  // Builds all threads that are not ready on options.threads workers (the caller is one of them and the only
  // one calling `progress`, with a monotonic value in [0,1], 1.0 on success). On cancellation returns
  // Cancelled; threads finished before stay ready.
  Result<void> build(LodCancel cancel = nullptr, const std::function<void(float)>& progress = {});

  // Thread-safe.
  size_t threadCount() const;
  bool threadReady(ThreadIdx thread) const;
  size_t readyThreadCount() const;
  // Fraction of the build work finished, weighted by sample count; 1.0 when all threads are ready.
  float progress() const;

  // Thread-safe. Stored levels, finest first (empty before a successful prepare()).
  const std::vector<LodLevelInfo>& levels() const;
  // Thread-safe. Bucket of a ready thread; false if the thread is not ready or the indices are out of range.
  bool bucket(ThreadIdx thread, size_t levelIndex, uint32_t bucketIndex, LodBucket& out) const;
  // Thread-safe. Bytes held by the per-thread level arrays of ready threads.
  size_t memoryBytes() const;
  // Thread-safe. Hash of all level arrays of ready threads (determinism checks).
  uint64_t contentHash() const;

  // Queries: thread-safe for concurrent calls (also during build()). `out` is overwritten (its capacity is
  // reused). Errors: InvalidArgument for a bad thread, pixelWidth (1..kMaxPixelWidth), t1 <= t0 or a span
  // above kMaxQuerySpanTicks.
  Result<void> stateBands(const StateBandQuery& query, StateBandResult& out) const;
  Result<void> sampleStrip(const SampleStripQuery& query, SampleStripResult& out) const;
  Result<void> timeline(const TimelineQuery& query, TimelineResult& out) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Thread-safe: pure function. First tick of pixel column `column` (0..pixelWidth) of a query over [t0, t1).
uint64_t pixelEdge(uint64_t t0, uint64_t t1, uint32_t pixelWidth, uint32_t column);

// Thread-safe for distinct models: prepare() + build() on the calling thread plus options.threads - 1
// workers. Convenience for batch use; the viewer calls prepare() and build() on a worker instead.
Result<TimelineLod> buildLod(const model::TraceModel& model, const LodOptions& options = {},
                             LodCancel cancel = nullptr, const std::function<void(float)>& progress = {});

}  // namespace tachyon::analysis
