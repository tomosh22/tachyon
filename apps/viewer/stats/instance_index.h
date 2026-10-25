// Per-thread index over an instance list and the overlay geometry of the timeline's instance highlight
// (WP-47). Pure, no ImGui.
//
// The bus hands the timeline one list of instances sorted by begin across all threads. A timeline row needs
// the instances of its thread that overlap the visible window, so the index splits the list per thread once
// (O(n), when the highlight changes) and keeps, per thread, the running maximum of the end ticks: instances
// of one thread may nest (scopes) or overlap (recursion), and the maximum end makes the first candidate of a
// window a binary search.
//
// Overlay rects. One rect per visible instance (x extent in pixels, at least kMinRectWidth wide). Instances
// narrower than a pixel are dense: after drawing one, the next candidate is the first instance beginning in
// a later pixel (binary search), so the work per row is bounded by the plot width in pixels, not by the
// number of instances, and sub-pixel instances in neighbouring pixels merge into one run rect (count > 1). An
// instance lying entirely inside the previous rect (nested scope) adds no rect.
#pragma once

#include "analysis/instances.h"
#include "viewer/timeline/timeline_math.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tachyon::viewer::stats {

inline constexpr float kMinRectWidth = 2.0f;

struct OverlayRect {
  uint32_t instance = 0;  // index into the instance list (the first one of a merged run)
  float x0 = 0, x1 = 0;
  uint32_t count = 1;  // instances this rect stands for: more than one for a run of sub-pixel instances
};

class InstanceIndex {
 public:
  InstanceIndex() = default;
  // Thread-safe (constructor). O(instances + threads). Instances of a thread >= threadCount are ignored.
  InstanceIndex(std::shared_ptr<const std::vector<analysis::Instance>> instances, size_t threadCount);

  // Thread-safe (const).
  const std::vector<analysis::Instance>& instances() const;
  size_t threadCount() const { return m_rows.size(); }
  // Number of instances of `thread`.
  size_t countOn(ThreadIdx thread) const;

  // Thread-safe (const). Appends the overlay rects of `thread`'s instances that overlap the absolute window
  // [t0, t1] and the plot area (x0, width) under `view`, in begin order, at most `maxRects` of them.
  // Returns false when the cap cut the list short.
  bool rectsFor(ThreadIdx thread, const timeline::TimeView& view, const timeline::TimeLimits& limits,
                float x0, float width, size_t maxRects, std::vector<OverlayRect>& out) const;

  // Thread-safe (const). Brute-force count of the instances of `thread` overlapping [t0, t1] (tests).
  size_t countOverlapping(ThreadIdx thread, uint64_t t0, uint64_t t1) const;

 private:
  // Struct of arrays in begin order: the searches touch only contiguous 8-byte columns, not the (large,
  // shared) instance list.
  struct Row {
    std::vector<uint32_t> index;   // position in the instance list
    std::vector<uint64_t> begin;   // begin ticks
    std::vector<uint64_t> end;     // end ticks
    std::vector<uint64_t> maxEnd;  // running maximum of `end` over [0..k]
  };

  std::shared_ptr<const std::vector<analysis::Instance>> m_instances;
  std::vector<Row> m_rows;
};

}  // namespace tachyon::viewer::stats
