// Pure logic of the Function Statistics panel (WP-47): the search index over functions and scope ids, number
// formatting, the histogram and box plot geometry, and instance stepping / navigation windows. Nothing here
// touches ImGui or the executor, so it is unit-testable and benchmarkable alone.
//
// Search index. One entry per function row of a bottomUpFunctions result (kind Function: the display name is
// module!function, the count is the number of samples containing the function) and one per distinct scope id
// of the trace's Begin events (kind Scope: the display name is the id, the count is the number of Begin
// events). The index is immutable once built (it is built on a query worker and handed to the panel).
//
// Geometry. All coordinates are screen pixels, y grows downwards. The histogram has one bar per log2 bucket
// from the first non-empty bucket to the last (Stats::histogram); the box plot maps min..max linearly onto
// the available width.
#pragma once

#include "analysis/instances.h"
#include "analysis/query.h"
#include "symbols/function_key.h"
#include "viewer/common/function_names.h"
#include "viewer/stats/fuzzy_matcher.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace tachyon::viewer::stats {

// ---- search index -----------------------------------------------------------------------------------------

enum class EntryKind : uint8_t { Function, Scope };

struct SearchEntry {
  EntryKind kind = EntryKind::Function;
  symbols::FunctionKey key;
  std::string name;    // what is searched and shown: module!function (functions) or the scope id (scopes)
  std::string module;  // empty for scopes, <kernel> and unmapped frames
  uint32_t count = 0;  // functions: samples containing it; scopes: Begin events
  bool countKnown = false;
};

struct SearchIndex {
  std::vector<SearchEntry> entries;
  FuzzyIndex fuzzy;  // index i names entries[i]
  std::unordered_map<symbols::FunctionKey, uint32_t> byKey;

  // Thread-safe (const). Entry of `key` or nullptr.
  const SearchEntry* find(const symbols::FunctionKey& key) const;
};

using Namer = std::function<common::NameParts(const symbols::FunctionKey&)>;

// Thread-safe (pure; `namer` must be). Functions first (in the order of `rows`), then the scope ids of
// `scopeIds` ordered by name. `scopeIds` holds the distinct scope ids with their Begin counts.
std::shared_ptr<const SearchIndex> buildSearchIndex(std::span<const analysis::FunctionRow> rows,
                                                    const Namer& namer,
                                                    std::span<const std::pair<StringIdx, uint32_t>> scopeIds,
                                                    const model::TraceModel& model);

// Thread-safe (const access to the model). The distinct scope ids that have a Begin event, with their Begin
// counts, in first-seen order. O(scope events).
std::vector<std::pair<StringIdx, uint32_t>> collectScopeIds(const model::TraceModel& model);

// Maximum number of results shown for a query.
inline constexpr size_t kMaxSearchResults = 200;

struct SearchHit {
  uint32_t entry = 0;
  MatchTier tier = MatchTier::None;
  int32_t score = 0;
};

// Thread-safe (const). The best kMaxSearchResults matches of `query` (empty = the first entries in index
// order), best first.
std::vector<SearchHit> searchEntries(const SearchIndex& index, std::string_view query);

// Moves a result selection by `delta` rows with clamping; -1 for no selection yields 0 (down) or the last row
// (up). Thread-safe: pure function.
int stepSelection(int current, int delta, int count);

// ---- formatting -------------------------------------------------------------------------------------------

// Thread-safe: pure function. A duration in ticks as "1.234 s", "12.345 ms", "12.3 \xC2\xB5s" or "800 ns",
// unit chosen by magnitude (via ticksToMs). A zero clock frequency counts as 1 GHz.
std::string formatDuration(double ticks, double clockHz);

// ---- geometry ---------------------------------------------------------------------------------------------

struct PlotRect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float width() const { return x1 - x0; }
  float height() const { return y1 - y0; }
};

struct HistogramBar {
  uint32_t bucket = 0;
  uint32_t count = 0;
  float x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // y1 is the baseline (bottom of the plot)
};

struct HistogramLayout {
  std::vector<HistogramBar> bars;  // one per bucket in [firstBucket, lastBucket], empty buckets included
  uint32_t firstBucket = 0, lastBucket = 0;
  uint32_t maxCount = 0;
};

// Thread-safe: pure function. Bars of equal width (a 1 px gap between neighbours when they are wide enough)
// filling `area`; the tallest bar spans the whole height, other non-empty bars are proportional to their
// count and at least 1 px high, empty buckets have zero height. Empty layout for count == 0.
HistogramLayout layoutHistogram(const analysis::Stats& stats, const PlotRect& area);

// Thread-safe: pure function. Lower edge of a bucket in ticks (analysis::statsBucketLower).
double bucketLowerTicks(uint32_t bucket);

struct BoxPlotLayout {
  bool valid = false;
  float xMin = 0, xQ1 = 0, xMedian = 0, xQ3 = 0, xMax = 0, xMean = 0;
  float yTop = 0, yBottom = 0, yMid = 0;  // the box spans yTop..yBottom, the whiskers sit at yMid
};

// Thread-safe: pure function. Positions of min / p25 / median / p75 / max / mean on a linear scale from
// stats.min to stats.max across `area` (all at the centre when min == max); invalid for count == 0. The box
// takes the middle 60% of the area's height.
BoxPlotLayout layoutBoxPlot(const analysis::Stats& stats, const PlotRect& area);

// ---- instances --------------------------------------------------------------------------------------------

// Thread-safe: pure function. Next / previous instance index with wrap-around. With no current instance the
// first (forward) or last (backward) one is chosen; nullopt for an empty list.
std::optional<uint32_t> stepInstance(size_t count, std::optional<uint32_t> current, bool forward);

struct TickWindow {
  uint64_t t0 = 0, t1 = 0;
};

// Thread-safe: pure function. The window a NavigationRequest should show for `instance`: centred on it and
// three times its duration, but at least 16 sample intervals (and at least 1000 ticks) wide, clamped at
// tick 0. t1 > t0 always.
TickWindow navigationWindow(const analysis::Instance& instance, uint64_t sampleIntervalTicks);

}  // namespace tachyon::viewer::stats
