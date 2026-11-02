// Row model of the threads timeline (WP-42): sorting, pin/hide/expand flags, vertical layout, hit-testing of
// a point inside a row, and lookup of the flame span under a time. Pure logic, no ImGui.
#pragma once

#include "analysis/lod.h"
#include "core/strong_id.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace tachyon::viewer::timeline {

enum class SortKey : uint8_t { RunningTime, SampleCount, Name, ThreadId, Natural };

inline constexpr const char* sortKeyName(SortKey key) {
  switch (key) {
    case SortKey::RunningTime: return "Running time";
    case SortKey::SampleCount: return "Sample count";
    case SortKey::Name: return "Name";
    case SortKey::ThreadId: return "Thread id";
    case SortKey::Natural: return "Trace order";
  }
  return "";
}

// Per-thread numbers a row is sorted by.
struct ThreadSortInfo {
  ThreadIdx thread;
  uint64_t runningTicks = 0;
  uint64_t sampleCount = 0;
  std::string_view name;
  uint32_t tid = 0;
};

// What a row shows below its state band (WP-54): the sampled strip / flame chart, the instrumentation scope
// lanes, or both (scope lanes under the sampled part).
enum class RowMode : uint8_t { Sampled, Scopes, Both };

inline constexpr const char* rowModeName(RowMode mode) {
  switch (mode) {
    case RowMode::Sampled: return "Sampled";
    case RowMode::Scopes: return "Scopes";
    case RowMode::Both: return "Both";
  }
  return "";
}
inline constexpr bool modeHasSampled(RowMode m) {
  return m != RowMode::Scopes;
}
inline constexpr bool modeHasScopes(RowMode m) {
  return m != RowMode::Sampled;
}

// Per-thread user flags, indexed by ThreadIdx.
struct RowFlags {
  bool pinned = false;
  bool hidden = false;
  bool expanded = false;
  // WP-54: this row's mode; nullopt follows the global mode of the timeline.
  std::optional<RowMode> mode;
};

// All functions in this header: thread-safe, pure.

// Visible rows in display order: pinned rows first, then the rest, each group sorted by `key` in
// the natural direction of the key (`reverse` flips it), with ties broken by thread index. Hidden threads are
// omitted. `flags` may be shorter than `infos` (missing entries are default flags).
std::vector<ThreadIdx> orderRows(const std::vector<ThreadSortInfo>& infos, const std::vector<RowFlags>& flags,
                                 SortKey key, bool reverse);

struct RowGeometry {
  float bandHeight = 12.0f;
  float stripHeight = 10.0f;
  float rowPadding = 2.0f;  // above and below the content
  float flameDepthHeight = 14.0f;
  float flameGap = 2.0f;          // between the strip and the flame chart
  float rowSpacing = 2.0f;        // between rows
  float scopeLineHeight = 14.0f;  // WP-54: one scope depth line
  float scopeGap = 2.0f;          // between the sampled part (or the band) and the scope lanes
};

struct RowLayout {
  ThreadIdx thread;
  float y0 = 0;      // top in content space (relative to the first row)
  float height = 0;  // including padding, excluding spacing
  bool expanded = false;
  uint32_t flameDepths = 0;         // depths shown when expanded
  RowMode mode = RowMode::Sampled;  // WP-54: what the row shows
  uint32_t scopeLines = 0;          // WP-54: scope lane lines shown (0: none, the lane block is absent)
};

// WP-54: per-thread extras of the layout, indexed by ThreadIdx; missing entries mean Sampled / no scope
// lines.
struct RowExtra {
  RowMode mode = RowMode::Sampled;
  uint32_t scopeLines = 0;
};

// Content-space layout. `flameDepths[i]` is the number of depths to show for order[i] when expanded.
std::vector<RowLayout> layoutRows(const std::vector<ThreadIdx>& order, const std::vector<RowFlags>& flags,
                                  const std::vector<uint32_t>& flameDepthsByThread, const RowGeometry& geom,
                                  const std::vector<RowExtra>& extras = {});
float contentHeight(const std::vector<RowLayout>& layout, const RowGeometry& geom);

struct RowRange {
  size_t first = 0, last = 0;  // [first, last)
};
// Rows intersecting [scrollY, scrollY + viewHeight) in content space.
RowRange visibleRows(const std::vector<RowLayout>& layout, float scrollY, float viewHeight);
// Index of the row containing content-space y (spacing counts as the row above), or SIZE_MAX.
size_t rowAt(const std::vector<RowLayout>& layout, const RowGeometry& geom, float contentY);

enum class RowSection : uint8_t { None, Band, Strip, Flame, Scope };
struct RowHit {
  RowSection section = RowSection::None;
  uint32_t depth = 0;  // 1-based flame depth for Flame; 0-based scope line for Scope
};
// Which part of a row the offset `yInRow` (from the row's y0) falls in.
RowHit hitRowSection(const RowLayout& row, const RowGeometry& geom, float yInRow);
// Y of the band / strip / flame depth `depth` (1-based) relative to the row top.
float bandTop(const RowGeometry& geom);
float stripTop(const RowGeometry& geom);
float flameTop(const RowGeometry& geom);
float flameDepthTop(const RowGeometry& geom, uint32_t depth);
// WP-54: Y of the first scope lane line relative to the row top (below the band, or below the sampled part).
float scopeTop(const RowLayout& row, const RowGeometry& geom);
float scopeLineTop(const RowLayout& row, const RowGeometry& geom, uint32_t line);

// The span at `depth` covering tick `ts` in a TimelineResult::spans vector (sorted by (depth, t0)), or
// nullptr.
const analysis::TimelineSpan* findSpanAt(const std::vector<analysis::TimelineSpan>& spans, uint32_t depth,
                                         uint64_t ts);

// Index of the column of a result with `columns` entries over [t0, t1) containing `ts` (clamped).
uint32_t columnForTick(uint64_t t0, uint64_t t1, uint32_t columns, uint64_t ts);

}  // namespace tachyon::viewer::timeline
