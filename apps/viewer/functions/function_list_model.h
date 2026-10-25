// Pure logic of the Function List panel (WP-44): the display table built from bottomUpFunctions rows, the
// sort / text-filter view, module grouping with per-module sums, and the mapping of the bus filter to the
// query filter. Nothing here touches ImGui or the executor, so it is unit-testable and benchmarkable alone.
//
// Table. A Table is immutable once built (it is built on the query's worker thread and handed to the panel as
// shared_ptr<const Table>). Names are computed once per row at build time: the display name, the module and a
// lower-case haystack for the text filter. Sorting by name or module does not compare strings per frame:
// build time also assigns each row a dense rank of its lower-case function and module name, so a sort is an
// integer sort (100 k rows sort in a few ms).
//
// Percentages. Inclusive % and exclusive % are relative to the total weight of the selected samples, which is
// the sum of all exclusive ticks (every sample has exactly one leaf function, so exclusive ticks partition
// the total; samples of an empty stack are not in any function row and therefore not in the total either).
//
// Module grouping. A group holds the rows of one module (by module name) that pass the text filter. Its
// exclusive sum is exact (exclusive time is attributed to exactly one function). Its inclusive figure is the
// SUM OF THE FUNCTION INCLUSIVES, which double counts a sample whose stack passes through several functions
// of the module (and through a function and its callers in the same module). An exact per-module inclusive
// would need a per-sample scan per module, which the cached bottom-up result does not carry; the header
// tooltip of the panel says so. Instances are summed the same way.
#pragma once

#include "analysis/query.h"
#include "symbols/function_key.h"
#include "viewer/common/function_names.h"
#include "viewer/selection_bus.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tachyon::viewer::functions {

// ---- query ------------------------------------------------------------------------------------------------

// The effective query of a view; the panel re-queries when it differs from the last one requested.
struct QuerySpec {
  analysis::Filter filter;
  bool expandInline = false;
  analysis::KernelFrames kernelFrames = analysis::KernelFrames::Show;
  uint64_t documentGeneration = 0;
  uint64_t symbolGeneration = 0;
  bool operator==(const QuerySpec&) const = default;
};

// Thread-safe: pure function. Bus filter -> analysis::Filter. With `restrictToSubtree` the bus filter's
// subtreeRoot (set by the Call Graph) is kept, otherwise it is cleared. Time range and threads always follow
// the bus. ViewFilter::function is not mapped (see common::toQueryFilter).
analysis::Filter makeFilter(const std::optional<ViewFilter>& busFilter, bool restrictToSubtree);

// Thread-safe: pure function. The aggregation options a query for `selected` needs: a key of an inline frame
// only exists with inline expansion, and the merged kernel key only with kernel grouping or hiding, so those
// options are forced on when the selection requires them (otherwise the toggles are used as they are).
analysis::AggregationOptions effectiveOptions(bool expandInline, analysis::KernelFrames kernelFrames,
                                              const std::optional<symbols::FunctionKey>& selected);

// ---- table ------------------------------------------------------------------------------------------------

struct Entry {
  symbols::FunctionKey key;
  uint64_t inclusiveTicks = 0, exclusiveTicks = 0;
  uint32_t instances = 0;
  std::string function;  // name without the module ("Engine::update", "game.exe!0x1a40" for unresolved)
  std::string module;    // "game.exe"; empty for <root>, <kernel>, scopes and unmapped frames
  std::string full;      // module!function as copied
  uint32_t haystackOffset = 0, haystackLength = 0;  // lower-case `full` inside Table::haystacks
  uint32_t functionRank = 0, moduleRank = 0;        // dense ranks of the lower-case function / module name
  uint32_t keyRank = 0;  // position in the order of (key, row index): the unique deterministic tie-break
  bool inlined = false;
};

struct Table {
  std::vector<Entry> entries;  // in the order of the query rows
  std::string haystacks;       // the lower-case full names back to back (contiguous: the filter scans it)
  uint64_t totalTicks = 0;     // sum of exclusive ticks, the base of the percentages
  double clockHz = 1e9;
};

using Namer = std::function<common::NameParts(const symbols::FunctionKey&)>;

// Thread-safe (pure; `namer` must be). O(n log n) for the ranks.
std::shared_ptr<const Table> buildTable(std::span<const analysis::FunctionRow> rows, const Namer& namer,
                                        double clockHz);

// Thread-safe: pure function. Percentage (0..100) of `ticks` of `total`; 0 when the total is 0.
double percentOfTotal(uint64_t ticks, uint64_t total);

// Thread-safe (reads the table). The lower-case "module!function" text of a row.
std::string_view haystackOf(const Table& table, const Entry& entry);

// ---- view: filter and sort --------------------------------------------------------------------------------

enum class SortColumn : uint8_t { Function, Module, Inclusive, Exclusive, Instances };

struct SortSpec {
  SortColumn column = SortColumn::Exclusive;
  bool ascending = false;
  bool operator==(const SortSpec&) const = default;
};

// Thread-safe: pure function. The natural first direction of a column when the user picks it: descending for
// the numeric columns, ascending for the names.
bool defaultAscending(SortColumn column);

// Thread-safe: pure function. Lower-cases ASCII letters and trims surrounding blanks (the text filter is a
// case-insensitive substring match; non-ASCII bytes are compared as they are).
std::string normalizeFilterText(std::string_view text);

// Thread-safe (reads the table). Indices into Table::entries of the rows whose lower-case "module!function"
// contains `filterText` (already normalized; empty = all), sorted by `sort`. Ties are broken by the function
// key ascending (independent of the direction), then by row index, so the order is total and deterministic.
// O(n) filter, O(m log m) sort of the m survivors.
std::vector<uint32_t> buildView(const Table& table, std::string_view filterText, SortSpec sort);

// ---- module grouping --------------------------------------------------------------------------------------

struct Group {
  std::string module;         // "(no module)" for rows without one
  uint64_t inclusiveSum = 0;  // sum of the function inclusives (may double count, see the file comment)
  uint64_t exclusiveSum = 0;  // exact
  uint64_t instancesSum = 0;
  std::vector<uint32_t> members;  // entry indices in view order
};

// Thread-safe (reads the table). Groups the rows of `view` by module name. The groups are sorted by the same
// column as the rows, using the group sums (Function and Module sort by module name); ties by module name.
std::vector<Group> groupByModule(const Table& table, std::span<const uint32_t> view, SortSpec sort);

// ---- display rows -----------------------------------------------------------------------------------------

// One line of the table: a module header or a function row.
struct Item {
  bool header = false;
  uint32_t group = 0;  // index into the groups (headers and members of a group)
  uint32_t entry = 0;  // Table::entries index (function rows)
};

// Thread-safe (pure). Flat mode: one function item per view row.
std::vector<Item> flatItems(std::span<const uint32_t> view);
// Thread-safe (pure). Grouped mode: a header per group followed by its members unless the module is in
// `collapsed`.
std::vector<Item> groupedItems(std::span<const Group> groups,
                               const std::unordered_set<std::string>& collapsed);
// Thread-safe (pure). Index of the function item of `entry`, or SIZE_MAX.
size_t itemOfEntry(std::span<const Item> items, uint32_t entry);
// Thread-safe (reads the table). Index of the row with `key`, or SIZE_MAX. O(n).
size_t entryOfKey(const Table& table, const symbols::FunctionKey& key);

}  // namespace tachyon::viewer::functions
