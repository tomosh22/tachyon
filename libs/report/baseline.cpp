#include "report/baseline.h"

#include "report/report_builder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace tachyon::report {

namespace {

struct ThresholdKey {
  const char* name;
  bool integer;
};

constexpr ThresholdKey kThresholdKeys[] = {
    {"frame_p95_rel_pct", false},
    {"frame_p99_rel_pct", false},
    {"frame_abs_floor_ms", false},
    {"function_exclusive_pp", false},
    {"scope_median_rel_pct", false},
    {"scope_abs_floor_ms", false},
    {"lost_events_allowed", true},
    {"function_exclusive_rel_pct", false},
    {"function_exclusive_abs_floor_ms", false},
    {"total_cpu_rel_pct", false},
    {"total_cpu_abs_floor_ms", false},
};

int categoryOrder(const std::string& c) {
  if (c == "frame_p95") return 0;
  if (c == "frame_p99") return 1;
  if (c == "function_exclusive") return 2;
  if (c == "function_exclusive_ms") return 3;
  if (c == "total_cpu") return 4;
  if (c == "scope_median") return 5;
  return 6;
}

double magnitude(const Finding& f) {
  return f.change ? std::fabs(*f.change) : std::numeric_limits<double>::infinity();
}

std::string functionSubject(const FunctionReportRow& r) {
  if (r.isScope) return "scope:" + r.name;
  return r.module.empty() ? r.name : r.module + "!" + r.name;
}

// Adds a regression / improvement when `current` differs from `baseline` by more than both the relative and
// the absolute threshold.
void compareRelative(Diff& d, const char* category, const std::string& subject, double baseline,
                     double current, double relPct, double floorMs) {
  const double delta = current - baseline;
  if (std::fabs(delta) <= floorMs) return;
  std::optional<double> rel;
  if (baseline > 0) rel = roundPct(delta / baseline * 100.0);
  const bool beyondRel = rel ? std::fabs(delta / baseline * 100.0) > relPct : true;
  if (!beyondRel) return;
  Finding f;
  f.kind = delta > 0 ? FindingKind::Regression : FindingKind::Improvement;
  f.category = category;
  f.subject = subject;
  f.baseline = baseline;
  f.current = current;
  f.valueUnit = "ms";
  f.change = rel;
  f.changeUnit = "%";
  (f.kind == FindingKind::Regression ? d.regressions : d.improvements).push_back(std::move(f));
}

struct ShareTable {
  std::map<std::string, double> share;  // subject -> percent of total (duplicates summed)
  std::map<std::string, double> ms;     // subject -> exclusive ms (duplicates summed)
  double bound = 0;                     // upper bound for an unlisted function
};

ShareTable shareTable(const Report& r) {
  ShareTable t;
  auto add = [&t](const FunctionReportRow& f) {
    const std::string s = functionSubject(f);
    t.share[s] += f.exclusivePct;
    t.ms[s] += f.exclusiveMs;
  };
  if (r.significantFloorPct >= 0) {
    // The baseline function set lists every function at or above the floor; the top N adds the smaller ones
    // it reaches. An unlisted function is below the floor.
    for (const FunctionReportRow& f : r.significantFunctions) add(f);
    for (const FunctionReportRow& f : r.functions)
      if (f.exclusivePct < r.significantFloorPct) add(f);
    t.bound = r.significantFloorPct;
    return t;
  }
  // Old report: only the top N is known.
  double minShare = std::numeric_limits<double>::infinity();
  for (const FunctionReportRow& f : r.functions) {
    add(f);
    minShare = std::min(minShare, f.exclusivePct);
  }
  const bool complete = r.functions.size() < r.settings.topN;
  t.bound = complete || r.functions.empty() ? 0.0 : minShare;
  return t;
}

void compareFrames(Diff& d, const Report& base, const Report& cur) {
  std::map<std::string, const FrameSeriesRow*> byLabel;
  for (const FrameSeriesRow& s : base.frames) byLabel.emplace(s.label, &s);
  for (const FrameSeriesRow& c : cur.frames) {
    const auto it = byLabel.find(c.label);
    if (it == byLabel.end()) {
      d.notes.push_back("frame series '" + c.label + "' is not in the baseline");
      continue;
    }
    const FrameSeriesRow& b = *it->second;
    d.frames.push_back({c.label, b.p95Ms, b.p99Ms});
    if (b.frameCount == 0 || c.frameCount == 0) continue;
    compareRelative(d, "frame_p95", c.label, b.p95Ms, c.p95Ms, d.thresholds.frameP95RelPct,
                    d.thresholds.frameAbsFloorMs);
    compareRelative(d, "frame_p99", c.label, b.p99Ms, c.p99Ms, d.thresholds.frameP99RelPct,
                    d.thresholds.frameAbsFloorMs);
  }
  // Series of the baseline that the current report does not list (only certain when its list is complete).
  if (cur.frameSeriesTotal <= cur.frames.size()) {
    std::set<std::string> present;
    for (const FrameSeriesRow& c : cur.frames) present.insert(c.label);
    for (const FrameSeriesRow& b : base.frames)
      if (present.count(b.label) == 0)
        d.notes.push_back("frame series '" + b.label + "' of the baseline is not in the current trace");
  }
}

void compareFunctions(Diff& d, const Report& base, const Report& cur) {
  const ShareTable bt = shareTable(base), ct = shareTable(cur);
  std::map<std::string, std::pair<bool, bool>> keys;  // subject -> (in baseline, in current)
  for (const auto& [k, v] : bt.share) keys[k].first = true;
  for (const auto& [k, v] : ct.share) keys[k].second = true;
  const double pp = d.thresholds.functionExclusivePp;
  std::set<std::string> flagged;
  for (const auto& [subject, where] : keys) {
    // A function missing from a report's top N has a share in [0, bound]; one that is listed has it exactly.
    const double bLo = where.first ? bt.share.at(subject) : 0.0;
    const double bHi = where.first ? bt.share.at(subject) : bt.bound;
    const double cLo = where.second ? ct.share.at(subject) : 0.0;
    const double cHi = where.second ? ct.share.at(subject) : ct.bound;
    Finding f;
    f.category = "function_exclusive";
    f.subject = subject;
    f.valueUnit = "% of total";
    f.changeUnit = "pp";
    if (cLo - bHi > pp) {  // grew for sure
      f.kind = FindingKind::Regression;
      f.baseline = bHi;
      f.current = cLo;
      f.change = roundPct(cLo - bHi);
    } else if (bLo - cHi > pp) {  // shrank for sure
      f.kind = FindingKind::Improvement;
      f.baseline = bLo;
      f.current = cHi;
      f.change = roundPct(cHi - bLo);
    } else {
      continue;
    }
    f.baselineIsBound = !where.first;
    f.currentIsBound = !where.second;
    (f.kind == FindingKind::Regression ? d.regressions : d.improvements).push_back(std::move(f));
    flagged.insert(subject);
  }

  // Absolute exclusive time, for functions known exactly in both reports and not already flagged by share.
  for (const auto& [subject, where] : keys) {
    if (!where.first || !where.second || flagged.count(subject) != 0) continue;
    compareRelative(d, "function_exclusive_ms", subject, bt.ms.at(subject), ct.ms.at(subject),
                    d.thresholds.functionExclusiveRelPct, d.thresholds.functionExclusiveAbsFloorMs);
  }
}

void compareScopes(Diff& d, const Report& base, const Report& cur) {
  std::map<std::string, double> medians;
  std::set<std::string> present;
  for (const ScopeReportRow& s : base.scopes) medians.emplace(s.name, s.medianMs);
  // A scope missing from a list that holds every scope of its trace is new / removed; with a capped list it
  // may only have fallen out of the top N.
  const bool baseComplete = base.scopeCount <= base.scopes.size();
  const bool curComplete = cur.scopeCount <= cur.scopes.size();
  for (const ScopeReportRow& c : cur.scopes) {
    present.insert(c.name);
    const auto it = medians.find(c.name);
    if (it == medians.end()) {
      if (baseComplete) d.notes.push_back("scope '" + c.name + "' is new (not in the baseline)");
      continue;
    }
    compareRelative(d, "scope_median", "scope:" + c.name, it->second, c.medianMs,
                    d.thresholds.scopeMedianRelPct, d.thresholds.scopeAbsFloorMs);
  }
  if (curComplete)
    for (const ScopeReportRow& s : base.scopes)
      if (present.count(s.name) == 0)
        d.notes.push_back("scope '" + s.name + "' was removed (not in the current trace)");
}

void compareTotalCpu(Diff& d, const Report& base, const Report& cur) {
  if (!(base.trace.totalCpuMs > 0) || !(cur.trace.totalCpuMs > 0)) return;
  compareRelative(d, "total_cpu", "total CPU", base.trace.totalCpuMs, cur.trace.totalCpuMs,
                  d.thresholds.totalCpuRelPct, d.thresholds.totalCpuAbsFloorMs);
}

void compareLost(Diff& d, const Report& base, const Report& cur) {
  const uint64_t b = base.trace.eventsLost, c = cur.trace.eventsLost;
  if (c > b && c - b > d.thresholds.lostEventsAllowed) {
    Finding f;
    f.kind = FindingKind::Regression;
    f.category = "lost_events";
    f.baseline = static_cast<double>(b);
    f.current = static_cast<double>(c);
    f.valueUnit = "events";
    f.change = static_cast<double>(c - b);
    f.changeUnit = "events";
    d.regressions.push_back(std::move(f));
  } else if (c < b) {
    Finding f;
    f.kind = FindingKind::Improvement;
    f.category = "lost_events";
    f.baseline = static_cast<double>(b);
    f.current = static_cast<double>(c);
    f.valueUnit = "events";
    f.change = -static_cast<double>(b - c);
    f.changeUnit = "events";
    d.improvements.push_back(std::move(f));
  }
}

void sortFindings(std::vector<Finding>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Finding& a, const Finding& b) {
    const int ca = categoryOrder(a.category), cb = categoryOrder(b.category);
    if (ca != cb) return ca < cb;
    const double ma = magnitude(a), mb = magnitude(b);
    if (ma != mb) return ma > mb;
    return a.subject < b.subject;
  });
}

}  // namespace

Result<Thresholds> thresholdsFromJson(const nlohmann::json& j) {
  if (!j.is_object()) return makeError(ErrorCode::InvalidArgument, "thresholds: expected a JSON object");
  Thresholds t;
  for (const auto& [key, value] : j.items()) {
    const ThresholdKey* known = nullptr;
    for (const ThresholdKey& k : kThresholdKeys)
      if (key == k.name) known = &k;
    if (known == nullptr)
      return makeError(ErrorCode::InvalidArgument, "thresholds: unknown key '" + key + "'");
    if (!value.is_number() || !(value.get<double>() >= 0) || !std::isfinite(value.get<double>()))
      return makeError(ErrorCode::InvalidArgument, "thresholds: '" + key + "' must be a non-negative number");
    const double v = value.get<double>();
    if (key == "frame_p95_rel_pct")
      t.frameP95RelPct = v;
    else if (key == "frame_p99_rel_pct")
      t.frameP99RelPct = v;
    else if (key == "frame_abs_floor_ms")
      t.frameAbsFloorMs = v;
    else if (key == "function_exclusive_pp")
      t.functionExclusivePp = v;
    else if (key == "scope_median_rel_pct")
      t.scopeMedianRelPct = v;
    else if (key == "scope_abs_floor_ms")
      t.scopeAbsFloorMs = v;
    else if (key == "function_exclusive_rel_pct")
      t.functionExclusiveRelPct = v;
    else if (key == "function_exclusive_abs_floor_ms")
      t.functionExclusiveAbsFloorMs = v;
    else if (key == "total_cpu_rel_pct")
      t.totalCpuRelPct = v;
    else if (key == "total_cpu_abs_floor_ms")
      t.totalCpuAbsFloorMs = v;
    else if (key == "lost_events_allowed") {
      if (v != std::floor(v))
        return makeError(ErrorCode::InvalidArgument, "thresholds: 'lost_events_allowed' must be an integer");
      t.lostEventsAllowed = static_cast<uint64_t>(v);
    }
  }
  return t;
}

nlohmann::ordered_json thresholdsToJson(const Thresholds& t) {
  nlohmann::ordered_json j;
  j["frame_p95_rel_pct"] = t.frameP95RelPct;
  j["frame_p99_rel_pct"] = t.frameP99RelPct;
  j["frame_abs_floor_ms"] = t.frameAbsFloorMs;
  j["function_exclusive_pp"] = t.functionExclusivePp;
  j["scope_median_rel_pct"] = t.scopeMedianRelPct;
  j["scope_abs_floor_ms"] = t.scopeAbsFloorMs;
  j["function_exclusive_rel_pct"] = t.functionExclusiveRelPct;
  j["function_exclusive_abs_floor_ms"] = t.functionExclusiveAbsFloorMs;
  j["total_cpu_rel_pct"] = t.totalCpuRelPct;
  j["total_cpu_abs_floor_ms"] = t.totalCpuAbsFloorMs;
  j["lost_events_allowed"] = t.lostEventsAllowed;
  return j;
}

Diff compareReports(const Report& baseline, const Report& current, const Thresholds& thresholds) {
  Diff d;
  d.thresholds = thresholds;
  if (baseline.trace.platform != current.trace.platform)
    d.notes.push_back("baseline platform '" + baseline.trace.platform + "' differs from '" +
                      current.trace.platform + "'");
  if (current.trace.partial)
    d.notes.push_back(
        "the current trace is partial (the capture did not finish cleanly); durations, totals and the "
        "comparison below may be incomplete");
  if (baseline.trace.partial)
    d.notes.push_back("the baseline trace was partial (the capture did not finish cleanly)");
  if (baseline.trace.durationMs > 0 && current.trace.durationMs > 0) {
    const double ratio = current.trace.durationMs / baseline.trace.durationMs;
    if (ratio > 1.25 || ratio < 0.8)
      d.notes.push_back("the traces differ in duration (" +
                        std::to_string(static_cast<int>(baseline.trace.durationMs)) + " ms vs " +
                        std::to_string(static_cast<int>(current.trace.durationMs)) +
                        " ms): absolute millisecond comparisons are not like for like");
  }
  compareFrames(d, baseline, current);
  compareFunctions(d, baseline, current);
  compareTotalCpu(d, baseline, current);
  compareScopes(d, baseline, current);
  compareLost(d, baseline, current);
  sortFindings(d.regressions);
  sortFindings(d.improvements);
  return d;
}

}  // namespace tachyon::report
