// Instrumentation scope lanes of the threads timeline (WP-54): under a thread row, one line per scope depth,
// each scope a coloured rectangle labelled "id [data]". With "Group by fiber" the lanes are per fiber (drawn
// under the thread each piece of the fiber ran on). Lock waits get a hatch, unterminated scopes an open end.
//
// Data. The scope pairing (analysis/scope_pairing.h, or fibers.h when grouped) runs on the QueryExecutor
// through Document::analysis() (cached in the QueryContext's QueryCache, so scopeStats and lockSummary reuse
// it) and is turned into ScopeLaneSet on the same worker: per lane and depth one TRACK of disjoint intervals
// sorted by begin, so everything the UI thread does is a binary search. A row whose set is not ready shows
// the pending look.
//
// Bounded work. A track holds disjoint intervals (stack discipline: scopes of one depth never overlap), so
// begin and end are both sorted. layoutTrack() finds the first visible interval by binary search and, for
// intervals narrower than a pixel, jumps to the first interval that begins in a later pixel (binary search),
// merging the run of such pixels into one item with a count. The items of a row are therefore bounded by the
// plot width in pixels (times the lines), never by the number of scopes.
#pragma once

#include "analysis/fibers.h"
#include "analysis/query.h"
#include "analysis/scope_pairing.h"
#include "model/model.h"
#include "viewer/common/latest_query.h"
#include "viewer/panel.h"
#include "viewer/timeline/timeline_colors.h"
#include "viewer/timeline/timeline_math.h"
#include "viewer/timeline/timeline_rows.h"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tachyon::viewer::timeline {

// Deeper scopes are not drawn (counted in ScopeLaneSet::droppedDeep); lines per lane and per row are capped
// so a pathological nesting cannot make a row taller than a screen.
inline constexpr uint32_t kMaxScopeDepth = 64;
inline constexpr uint32_t kMaxLinesPerLane = 12;
inline constexpr uint32_t kMaxLinesPerRow = 24;

// The disjoint intervals of one depth of one lane, sorted by begin (so by end too). A fiber lane holds the
// PIECES of its scopes (the stretches during which the fiber ran), the plain pairing the whole scopes.
inline constexpr uint8_t kTrackLock = 1;  // a lock wait
inline constexpr uint8_t kTrackOpen = 2;  // unterminated: no End event

struct ScopeTrack {
  std::vector<uint64_t> begin, end;
  std::vector<const analysis::ScopeInstance*> instance;  // owned by ScopeLaneSet::plain / fibers
  // Drawing data kept next to the extents, so that drawing a row never follows the instance pointers (random
  // memory): the fill colour (Color32) and kTrack* flags. Empty in tracks built by hand; fillOf() / flagsOf()
  // then read the instance.
  std::vector<uint32_t> fill;
  std::vector<uint8_t> flags;
  // OR of `flags` over blocks of kFlagBlock intervals, so that the flags of a merged run are found without
  // visiting every interval (WP-64). Empty in tracks built by hand.
  std::vector<uint8_t> flagBlocks;
  static constexpr size_t kFlagBlock = 64;

  // Thread-safe (const access).
  size_t size() const { return begin.size(); }
  uint32_t fillOf(size_t i) const;
  uint8_t flagsOf(size_t i) const;
  // Thread-safe (const access). OR of flagsOf() over [first, first + count): what a merged item of `count`
  // sub-pixel intervals stands for (a lock wait or unterminated scope anywhere in the run marks it).
  uint8_t flagsOver(size_t first, size_t count) const;
};

struct ScopeLane {
  uint64_t fiberId = 0;  // 0: the thread's own scopes (no fiber ran, or the trace has no fibers)
  ThreadIdx thread;      // the row the lane is drawn under
  std::vector<ScopeTrack> depths;
  uint64_t intervalCount = 0;
};

// One drawn line of a row: depth `depth` of lane `lane`.
struct LaneLine {
  uint32_t lane = 0;
  uint32_t depth = 0;
};

struct ScopeLaneSet {
  // Keep the instances the tracks point at alive.
  std::shared_ptr<const analysis::PairedScopes> plain;
  std::shared_ptr<const analysis::PairedFibers> fibers;
  bool grouped = false;
  std::vector<ScopeLane> lanes;                      // sorted by (thread, fiberId)
  std::vector<std::vector<uint32_t>> lanesOfThread;  // indexed by ThreadIdx
  std::shared_ptr<const analysis::LockSummary> locks;
  std::unordered_map<uint64_t, uint32_t> lockByAddress;  // address -> index into locks->locks
  uint64_t droppedDeep = 0;                              // intervals deeper than kMaxScopeDepth
  uint64_t intervalCount = 0;

  // Thread-safe (const access). The lock row of an address, or nullptr.
  const analysis::LockRow* lockRow(uint64_t address) const;
  // Thread-safe (const access). Lanes of `thread` (empty for unknown threads).
  const std::vector<uint32_t>& lanesFor(ThreadIdx thread) const;
  // Thread-safe (const access). The lines drawn for `thread`: per lane (thread lane first, then fibers by id)
  // all depths when `expanded`, else depth 0 only; at most kMaxLinesPerLane per lane and kMaxLinesPerRow per
  // row. Empty when the thread has no scopes.
  std::vector<LaneLine> lines(ThreadIdx thread, bool expanded) const;
  // Thread-safe (const access). lines(thread, expanded).size() without building the list.
  uint32_t lineCount(ThreadIdx thread, bool expanded) const;
};

// Thread-safe (pure). Lanes of the plain pairing: one lane per thread with scopes.
std::shared_ptr<ScopeLaneSet> buildScopeLanes(std::shared_ptr<const analysis::PairedScopes> paired,
                                              size_t threadCount);
// Thread-safe (pure). Lanes of the fiber-aware pairing: one lane per (fiber, thread it ran on) and per
// thread-context lane. Without fiber events (`fiberAware` false) this equals the plain lanes.
std::shared_ptr<ScopeLaneSet> buildScopeLanes(std::shared_ptr<const analysis::PairedFibers> paired,
                                              size_t threadCount);

// ---- item layout
// --------------------------------------------------------------------------------------------

// A rectangle of a track on screen. `count` > 1 (or a width at the minimum) stands for sub-pixel intervals.
struct LaneItem {
  float x0 = 0, x1 = 0;
  uint32_t first = 0;  // position in the track (the first interval of the item)
  uint32_t count = 1;  // intervals the item stands for
};
inline constexpr float kMinItemWidth = 1.0f;

// Thread-safe (pure). Appends the items of `track` visible in `area`, in order, at most `maxItems`. Returns
// false when the cap cut the list short. An interval at least kMinItemWidth wide is its own item (x clipped
// to the plot area); narrower ones merge per pixel and neighbouring pixels join into one run. Every interval
// that overlaps the view is counted in exactly one item.
bool layoutTrack(const ScopeTrack& track, const PlotArea& area, size_t maxItems, std::vector<LaneItem>& out);

// Result of hitting a track.
struct TrackHit {
  uint32_t first = 0;  // position of the interval (single) or of the first one of the group
  uint32_t count = 1;  // > 1: several sub-pixel intervals under the cursor
};
// Thread-safe (pure). The interval of `track` under screen x (contained in time, else any within
// `tolerancePx`), or the group of intervals within the tolerance.
std::optional<TrackHit> hitTrack(const ScopeTrack& track, const PlotArea& area, float x, float tolerancePx);

// ---- colours, labels, tooltips
// ------------------------------------------------------------------------------

inline constexpr Color32 kLockScopeColor = makeColor(200, 70, 60);
// Thread-safe (pure). The fill of a scope: its colour (SDK TACHYON_MAKE_COLOR layout 0xRRGGBBAA); the default
// colour (0xFFFFFFFF) is hashed from the id; lock waits (0x4C4F43FF) are kLockScopeColor.
Color32 scopeColor(uint32_t rgba, StringIdx id);
// Thread-safe (pure). "id [data]" or just "id" without data.
std::string scopeLabel(std::string_view id, std::string_view data);
// Thread-safe (pure). The longest prefix of `text` (cut at a UTF-8 boundary) that fits `widthPx` when one
// character is `charPx` wide, shortened by two characters and ended with ".." when it had to be cut; empty
// when fewer than 3 characters fit.
std::string clipLabel(std::string_view text, float widthPx, float charPx);

struct ScopeTooltipInput {
  const model::TraceModel* model = nullptr;
  const ScopeLaneSet* set = nullptr;
  const ScopeLane* lane = nullptr;
  const ScopeTrack* track = nullptr;
  TrackHit hit;
  double clockHz = 1e9;
};
// Thread-safe (pure). Tooltip lines of a scope: id, data, duration, depth, thread / fiber, and for lock waits
// the lock address with its contention summary; a merged hit gives the number of scopes and the time they
// cover instead.
std::vector<std::string> scopeTooltipLines(const ScopeTooltipInput& input);

// ---- the view
// -----------------------------------------------------------------------------------------------

// A scope item drawn in the last frame (tests).
struct DrawnScopeItem {
  ThreadIdx thread;
  uint32_t lane = 0;
  uint32_t line = 0;  // line within the row
  uint32_t depth = 0;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t first = 0;
  uint32_t count = 1;
  bool lock = false;
  bool unterminated = false;
  bool labelled = false;
  StringIdx id;
};

struct ScopeFrameStats {
  uint32_t rows = 0, lines = 0, items = 0, merged = 0, labels = 0, lockHatches = 0, openEnds = 0;
};

// What a mouse position over a scope line refers to.
struct ScopeHit {
  uint32_t laneIndex = 0;
  uint32_t depth = 0;
  TrackHit track;
  const ScopeLane* lane = nullptr;
  const ScopeTrack* trackPtr = nullptr;
};

class ScopeLanesView {
 public:
  ScopeLanesView() = default;
  ScopeLanesView(const ScopeLanesView&) = delete;
  ScopeLanesView& operator=(const ScopeLanesView&) = delete;

  // Main-thread only. Drops the set and cancels the query in flight.
  void reset();
  // Main-thread only. Starts the pairing query when the (document, grouped) pair changed, delivers a finished
  // one. Needs the document's analysis; without the query executor nothing happens (rows stay pending).
  void update(const PanelContext& context, bool grouped);
  // Main-thread only. Test hook: installs a ready set as if the query had delivered it.
  void inject(std::shared_ptr<const ScopeLaneSet> set, bool grouped);

  // Main-thread only.
  bool ready() const { return m_set != nullptr; }
  bool pending() const { return m_set == nullptr && m_query.pending(); }
  bool failed() const { return !m_error.empty(); }
  const std::string& error() const { return m_error; }
  bool grouped() const { return m_shownGrouped; }
  const std::shared_ptr<const ScopeLaneSet>& set() const { return m_set; }
  uint64_t issuedQueries() const { return m_query.issued(); }
  // Lines a row of `thread` needs: 0 when the thread has no scopes (or the set is ready and empty for it),
  // 1 while the set is pending (the pending look).
  uint32_t linesFor(ThreadIdx thread, bool expanded) const;
  // True when the (ready) set has scopes for `thread`; while the set is not ready, when the trace has scope or
  // fiber events on `thread` (WP-64: rows of threads without any do not reserve a line for the pending look).
  bool mayHaveScopes(ThreadIdx thread) const;

  // Main-thread only. Draws the lines of one row. `rowTop` is the screen y of the row top; the lane block
  // starts at scopeTop(row, geom). The caller has the row clip rect set.
  void drawRow(ImDrawList* dl, const model::TraceModel& model, const RowLayout& row, const RowGeometry& geom,
               float rowTop, const PlotArea& area, float gutterX0);
  // Main-thread only. The scope under (x, line) of a row, if any.
  std::optional<ScopeHit> hit(const RowLayout& row, uint32_t line, const PlotArea& area, float x) const;
  // Main-thread only. Selected-function outline: scopes of this id get a white frame.
  void setHighlightedId(std::optional<StringIdx> id) { m_highlightId = id; }

  // Main-thread only. Frame counters and the items drawn in the last frame (capped at kMaxDrawnItems).
  static constexpr size_t kMaxDrawnItems = 20000;
  // Test hook: whether drawRow() records DrawnScopeItem (off by default: the app never reads them).
  void setRecordItems(bool record) { m_record = record; }
  const ScopeFrameStats& frameStats() const { return m_stats; }
  const std::vector<DrawnScopeItem>& drawnItems() const { return m_items; }
  void beginFrame();

 private:
  common::LatestQuery<ScopeLaneSet> m_query;
  std::shared_ptr<const ScopeLaneSet> m_set;
  bool m_shownGrouped = false;
  bool m_haveRequested = false;
  bool m_requestedGrouped = false;
  uint64_t m_requestedDocument = 0;
  std::string m_error;
  std::optional<StringIdx> m_highlightId;
  ScopeFrameStats m_stats;
  std::vector<DrawnScopeItem> m_items;
  std::vector<LaneItem> m_scratch;
  uint32_t m_labelBudget = 0;
  bool m_record = false;
  std::vector<LaneItem> m_decorated;
  // Threads with scope or fiber events, computed once per document (one pass over the events).
  std::vector<uint8_t> m_candidates;
  uint64_t m_candidatesDocument = ~0ull;
};

}  // namespace tachyon::viewer::timeline
