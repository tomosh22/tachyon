#include "viewer/functions/function_list_model.h"

#include "viewer/common/query_filter.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <unordered_map>

namespace tachyon::viewer::functions {

using analysis::FunctionRow;
using symbols::FunctionKey;

namespace {

char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = lowerAscii(c);
  return out;
}

struct SortItem {
  uint64_t primary;
  uint32_t tie;  // Entry::keyRank, unique
  uint32_t index;
};

bool sortItemLess(const SortItem& a, const SortItem& b) {
  return a.primary != b.primary ? a.primary < b.primary : a.tie < b.tie;
}

uint64_t primaryOf(const Entry& e, SortColumn column, bool ascending) {
  uint64_t v = 0;
  switch (column) {
    case SortColumn::Function: v = e.functionRank; break;
    case SortColumn::Module: v = e.moduleRank; break;
    case SortColumn::Inclusive: v = e.inclusiveTicks; break;
    case SortColumn::Exclusive: v = e.exclusiveTicks; break;
    case SortColumn::Instances: v = e.instances; break;
  }
  return ascending ? v : ~v;
}

uint64_t primaryOfGroup(const Group& g, uint32_t moduleRank, SortColumn column, bool ascending) {
  uint64_t v = 0;
  switch (column) {
    case SortColumn::Function:
    case SortColumn::Module: v = moduleRank; break;
    case SortColumn::Inclusive: v = g.inclusiveSum; break;
    case SortColumn::Exclusive: v = g.exclusiveSum; break;
    case SortColumn::Instances: v = g.instancesSum; break;
  }
  return ascending ? v : ~v;
}

}  // namespace

analysis::Filter makeFilter(const std::optional<ViewFilter>& busFilter, bool restrictToSubtree) {
  analysis::Filter f = common::toQueryFilter(busFilter);
  if (!restrictToSubtree) f.subtreeRoot.reset();
  return f;
}

analysis::AggregationOptions effectiveOptions(bool expandInline, analysis::KernelFrames kernelFrames,
                                              const std::optional<FunctionKey>& selected) {
  analysis::AggregationOptions o;
  o.expandInline = expandInline;
  o.kernelFrames = kernelFrames;
  if (selected) {
    if (selected->isInline()) o.expandInline = true;
    if (*selected == analysis::kKernelKey && o.kernelFrames == analysis::KernelFrames::Show)
      o.kernelFrames = analysis::KernelFrames::Group;
  }
  return o;
}

std::shared_ptr<const Table> buildTable(std::span<const FunctionRow> rows, const Namer& namer,
                                        double clockHz) {
  auto table = std::make_shared<Table>();
  table->clockHz = clockHz;
  const size_t n = rows.size();
  table->entries.resize(n);
  std::vector<std::string> lowerFunction(n), lowerModule(n);
  size_t blobBytes = 0;
  for (size_t i = 0; i < n; ++i) {
    Entry& e = table->entries[i];
    const FunctionRow& r = rows[i];
    e.key = r.key;
    e.inclusiveTicks = r.inclusiveTicks;
    e.exclusiveTicks = r.exclusiveTicks;
    e.instances = r.instances;
    table->totalTicks += r.exclusiveTicks;
    common::NameParts parts = namer(r.key);
    e.function = std::move(parts.function);
    e.module = std::move(parts.module);
    e.full = std::move(parts.full);
    e.inlined = parts.inlined;
    lowerFunction[i] = lowered(e.function);
    lowerModule[i] = lowered(e.module);
    blobBytes += e.full.size();
  }
  table->haystacks.reserve(blobBytes);
  for (Entry& e : table->entries) {
    e.haystackOffset = static_cast<uint32_t>(table->haystacks.size());
    e.haystackLength = static_cast<uint32_t>(e.full.size());
    for (const char c : e.full) table->haystacks.push_back(lowerAscii(c));
  }

  std::vector<uint32_t> order(n);
  const auto denseRanks = [&](const std::vector<std::string>& names, auto assign) {
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return names[a] < names[b]; });
    uint32_t rank = 0;
    for (size_t i = 0; i < n; ++i) {
      if (i > 0 && names[order[i]] != names[order[i - 1]]) ++rank;
      assign(table->entries[order[i]], rank);
    }
  };
  denseRanks(lowerFunction, [](Entry& e, uint32_t r) { e.functionRank = r; });
  denseRanks(lowerModule, [](Entry& e, uint32_t r) { e.moduleRank = r; });
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const FunctionKey &ka = table->entries[a].key, &kb = table->entries[b].key;
    return ka != kb ? ka < kb : a < b;
  });
  for (size_t i = 0; i < n; ++i) table->entries[order[i]].keyRank = static_cast<uint32_t>(i);
  return table;
}

double percentOfTotal(uint64_t ticks, uint64_t total) {
  if (total == 0) return 0.0;
  return 100.0 * static_cast<double>(ticks) / static_cast<double>(total);
}

std::string_view haystackOf(const Table& table, const Entry& entry) {
  return std::string_view(table.haystacks).substr(entry.haystackOffset, entry.haystackLength);
}

bool defaultAscending(SortColumn column) {
  return column == SortColumn::Function || column == SortColumn::Module;
}

std::string normalizeFilterText(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return lowered(text);
}

std::vector<uint32_t> buildView(const Table& table, std::string_view filterText, SortSpec sort) {
  std::vector<SortItem> items;
  items.reserve(table.entries.size());
  const std::string_view blob(table.haystacks);
  for (size_t i = 0; i < table.entries.size(); ++i) {
    const Entry& e = table.entries[i];
    if (!filterText.empty() &&
        blob.substr(e.haystackOffset, e.haystackLength).find(filterText) == std::string_view::npos)
      continue;
    items.push_back({primaryOf(e, sort.column, sort.ascending), e.keyRank, static_cast<uint32_t>(i)});
  }
  std::sort(items.begin(), items.end(), sortItemLess);
  std::vector<uint32_t> view(items.size());
  for (size_t i = 0; i < items.size(); ++i) view[i] = items[i].index;
  return view;
}

std::vector<Group> groupByModule(const Table& table, std::span<const uint32_t> view, SortSpec sort) {
  std::vector<Group> groups;
  std::vector<uint32_t> ranks;  // module rank per group
  std::unordered_map<uint32_t, uint32_t> byRank;
  for (const uint32_t idx : view) {
    const Entry& e = table.entries[idx];
    const auto [it, fresh] = byRank.try_emplace(e.moduleRank, static_cast<uint32_t>(groups.size()));
    if (fresh) {
      groups.emplace_back();
      groups.back().module = e.module.empty() ? "(no module)" : e.module;
      ranks.push_back(e.moduleRank);
    }
    Group& g = groups[it->second];
    g.inclusiveSum += e.inclusiveTicks;
    g.exclusiveSum += e.exclusiveTicks;
    g.instancesSum += e.instances;
    g.members.push_back(idx);
  }
  std::vector<uint32_t> order(groups.size());
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const uint64_t pa = primaryOfGroup(groups[a], ranks[a], sort.column, sort.ascending);
    const uint64_t pb = primaryOfGroup(groups[b], ranks[b], sort.column, sort.ascending);
    if (pa != pb) return pa < pb;
    return ranks[a] != ranks[b] ? ranks[a] < ranks[b] : a < b;
  });
  std::vector<Group> sorted;
  sorted.reserve(groups.size());
  for (const uint32_t g : order) sorted.push_back(std::move(groups[g]));
  return sorted;
}

std::vector<Item> flatItems(std::span<const uint32_t> view) {
  std::vector<Item> items(view.size());
  for (size_t i = 0; i < view.size(); ++i) items[i] = Item{false, 0, view[i]};
  return items;
}

std::vector<Item> groupedItems(std::span<const Group> groups,
                               const std::unordered_set<std::string>& collapsed) {
  std::vector<Item> items;
  for (size_t g = 0; g < groups.size(); ++g) {
    items.push_back(Item{true, static_cast<uint32_t>(g), 0});
    if (collapsed.count(groups[g].module)) continue;
    for (const uint32_t e : groups[g].members) items.push_back(Item{false, static_cast<uint32_t>(g), e});
  }
  return items;
}

size_t itemOfEntry(std::span<const Item> items, uint32_t entry) {
  for (size_t i = 0; i < items.size(); ++i)
    if (!items[i].header && items[i].entry == entry) return i;
  return SIZE_MAX;
}

size_t entryOfKey(const Table& table, const FunctionKey& key) {
  for (size_t i = 0; i < table.entries.size(); ++i)
    if (table.entries[i].key == key) return i;
  return SIZE_MAX;
}

}  // namespace tachyon::viewer::functions
