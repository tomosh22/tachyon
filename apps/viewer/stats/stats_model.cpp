#include "viewer/stats/stats_model.h"

#include "trace/trace_format.h"
#include "viewer/common/query_filter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tachyon::viewer::stats {

using symbols::FunctionKey;

// ---------------------------------------------------------------------------------------------------------
// Search index

const SearchEntry* SearchIndex::find(const FunctionKey& key) const {
  const auto it = byKey.find(key);
  return it == byKey.end() ? nullptr : &entries[it->second];
}

std::vector<std::pair<StringIdx, uint32_t>> collectScopeIds(const model::TraceModel& model) {
  std::vector<std::pair<StringIdx, uint32_t>> out;
  std::unordered_map<uint32_t, size_t> at;
  for (const trace::ScopeEventRec& rec : model.scopeEvents) {
    trace::ScopeEventRec e;  // packed record: copy before use
    std::memcpy(&e, &rec, sizeof e);
    if (e.kind != trace::kScopeBegin || !e.id.valid()) continue;
    const auto [it, fresh] = at.try_emplace(e.id.get(), out.size());
    if (fresh) out.emplace_back(e.id, 0);
    ++out[it->second].second;
  }
  return out;
}

std::shared_ptr<const SearchIndex> buildSearchIndex(std::span<const analysis::FunctionRow> rows,
                                                    const Namer& namer,
                                                    std::span<const std::pair<StringIdx, uint32_t>> scopeIds,
                                                    const model::TraceModel& model) {
  auto index = std::make_shared<SearchIndex>();
  index->entries.reserve(rows.size() + scopeIds.size());
  for (const analysis::FunctionRow& row : rows) {
    SearchEntry e;
    e.kind = EntryKind::Function;
    e.key = row.key;
    const common::NameParts parts = namer(row.key);
    e.name = parts.full;
    e.module = parts.module;
    e.count = row.instances;
    e.countKnown = true;
    index->entries.push_back(std::move(e));
  }
  std::vector<SearchEntry> scopes;
  scopes.reserve(scopeIds.size());
  for (const auto& [id, count] : scopeIds) {
    SearchEntry e;
    e.kind = EntryKind::Scope;
    e.key = FunctionKey::scope(id);
    e.name = std::string(model.strings.get(id));
    e.count = count;
    e.countKnown = true;
    scopes.push_back(std::move(e));
  }
  std::stable_sort(scopes.begin(), scopes.end(),
                   [](const SearchEntry& a, const SearchEntry& b) { return a.name < b.name; });
  for (SearchEntry& e : scopes) index->entries.push_back(std::move(e));
  index->byKey.reserve(index->entries.size());
  for (uint32_t i = 0; i < index->entries.size(); ++i) {
    index->fuzzy.add(index->entries[i].name);
    index->byKey.try_emplace(index->entries[i].key, i);  // the first row of a key wins
  }
  return index;
}

std::vector<SearchHit> searchEntries(const SearchIndex& index, std::string_view query) {
  std::vector<SearchHit> out;
  for (const FuzzyResult& r : index.fuzzy.search(query, kMaxSearchResults))
    out.push_back({r.index, r.match.tier, r.match.score});
  return out;
}

int stepSelection(int current, int delta, int count) {
  if (count <= 0) return -1;
  if (current < 0) return delta >= 0 ? 0 : count - 1;
  return std::clamp(current + delta, 0, count - 1);
}

// ---------------------------------------------------------------------------------------------------------
// Formatting

std::string formatDuration(double ticks, double clockHz) {
  const double hz = clockHz > 0.0 ? clockHz : 1e9;
  const double ms = ticks * 1000.0 / hz;
  char buf[48];
  if (ms >= 1000.0) {
    std::snprintf(buf, sizeof buf, "%.3f s", ms / 1000.0);
  } else if (ms >= 1.0) {
    std::snprintf(buf, sizeof buf, "%.3f ms", ms);
  } else if (ms >= 0.001) {
    std::snprintf(buf, sizeof buf, "%.2f \xC2\xB5s", ms * 1000.0);
  } else {
    std::snprintf(buf, sizeof buf, "%.0f ns", ms * 1e6);
  }
  return buf;
}

// ---------------------------------------------------------------------------------------------------------
// Geometry

double bucketLowerTicks(uint32_t bucket) {
  return static_cast<double>(analysis::statsBucketLower(std::min(bucket, 64u)));
}

HistogramLayout layoutHistogram(const analysis::Stats& stats, const PlotRect& area) {
  HistogramLayout out;
  if (stats.count == 0 || stats.histogram.empty()) return out;
  const auto& h = stats.histogram;
  uint32_t first = 0;
  while (first < h.size() && h[first] == 0) ++first;
  if (first == h.size()) return out;
  const uint32_t last = static_cast<uint32_t>(h.size() - 1);
  out.firstBucket = first;
  out.lastBucket = last;
  for (uint32_t b = first; b <= last; ++b) out.maxCount = std::max(out.maxCount, h[b]);
  const uint32_t n = last - first + 1;
  const float slot = area.width() / static_cast<float>(n);
  const float gap = slot >= 4.0f ? 1.0f : 0.0f;
  for (uint32_t b = first; b <= last; ++b) {
    HistogramBar bar;
    bar.bucket = b;
    bar.count = h[b];
    bar.x0 = area.x0 + slot * static_cast<float>(b - first);
    bar.x1 = area.x0 + slot * static_cast<float>(b - first + 1) - gap;
    bar.y1 = area.y1;
    float height = 0.0f;
    if (bar.count > 0) {
      height = area.height() * static_cast<float>(bar.count) / static_cast<float>(out.maxCount);
      height = std::max(height, std::min(1.0f, area.height()));
    }
    bar.y0 = area.y1 - height;
    out.bars.push_back(bar);
  }
  return out;
}

BoxPlotLayout layoutBoxPlot(const analysis::Stats& stats, const PlotRect& area) {
  BoxPlotLayout out;
  if (stats.count == 0) return out;
  out.valid = true;
  const double lo = stats.min, hi = stats.max;
  const auto xOf = [&](double v) {
    if (hi <= lo) return area.x0 + area.width() * 0.5f;
    return area.x0 + static_cast<float>((v - lo) / (hi - lo)) * area.width();
  };
  out.xMin = xOf(stats.min);
  out.xQ1 = xOf(stats.p25);
  out.xMedian = xOf(stats.median);
  out.xQ3 = xOf(stats.p75);
  out.xMax = xOf(stats.max);
  out.xMean = xOf(std::clamp(stats.mean, lo, hi));
  out.yMid = (area.y0 + area.y1) * 0.5f;
  out.yTop = area.y0 + area.height() * 0.2f;
  out.yBottom = area.y1 - area.height() * 0.2f;
  return out;
}

// ---------------------------------------------------------------------------------------------------------
// Instances

std::optional<uint32_t> stepInstance(size_t count, std::optional<uint32_t> current, bool forward) {
  if (count == 0) return std::nullopt;
  if (!current || *current >= count) return forward ? 0u : static_cast<uint32_t>(count - 1);
  const uint32_t c = *current;
  if (forward) return c + 1 < count ? c + 1 : 0u;
  return c > 0 ? c - 1 : static_cast<uint32_t>(count - 1);
}

TickWindow navigationWindow(const analysis::Instance& instance, uint64_t sampleIntervalTicks) {
  const uint64_t duration = instance.end > instance.begin ? instance.end - instance.begin : 0;
  const uint64_t width = std::max<uint64_t>({duration * 3, sampleIntervalTicks * 16, 1000});
  const uint64_t centre = instance.begin + duration / 2;
  TickWindow w;
  w.t0 = centre > width / 2 ? centre - width / 2 : 0;
  w.t1 = w.t0 + width;
  return w;
}

}  // namespace tachyon::viewer::stats
