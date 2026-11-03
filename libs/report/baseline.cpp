#include "report/baseline.h"

#include "report/report_builder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace tachyon::report {

namespace {

struct ThresholdKey {
  const char* name;
  bool integer;
};

constexpr ThresholdKey kThresholdKeys[] = {
    {"frame_p95_rel_pct", false},     {"frame_p99_rel_pct", false},    {"frame_abs_floor_ms", false},
    {"function_exclusive_pp", false}, {"scope_median_rel_pct", false}, {"scope_abs_floor_ms", false},
    {"lost_events_allowed", true},
};

int categoryOrder(const std::string& c) {
  if (c == "frame_p95") return 0;
  if (c == "frame_p99") return 1;
  if (c == "function_exclusive") return 2;
  if (c == "scope_median") return 3;
  return 4;
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
  double bound = 0;                     // upper bound for an unlisted function
};

ShareTable shareTable(const Report& r) {
  ShareTable t;
  double minShare = std::numeric_limits<double>::infinity();
  for (const FunctionReportRow& f : r.functions) {
    t.share[functionSubject(f)] += f.exclusivePct;
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
}

void compareFunctions(Diff& d, const Report& base, const Report& cur) {
  const ShareTable bt = shareTable(base), ct = shareTable(cur);
  std::map<std::string, std::pair<bool, bool>> keys;  // subject -> (in baseline, in current)
  for (const auto& [k, v] : bt.share) keys[k].first = true;
  for (const auto& [k, v] : ct.share) keys[k].second = true;
  const double pp = d.thresholds.functionExclusivePp;
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
  }
}

void compareScopes(Diff& d, const Report& base, const Report& cur) {
  std::map<std::string, double> medians;
  for (const ScopeReportRow& s : base.scopes) medians.emplace(s.name, s.medianMs);
  for (const ScopeReportRow& c : cur.scopes) {
    const auto it = medians.find(c.name);
    if (it == medians.end()) continue;
    compareRelative(d, "scope_median", "scope:" + c.name, it->second, c.medianMs,
                    d.thresholds.scopeMedianRelPct, d.thresholds.scopeAbsFloorMs);
  }
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
  j["lost_events_allowed"] = t.lostEventsAllowed;
  return j;
}

Diff compareReports(const Report& baseline, const Report& current, const Thresholds& thresholds) {
  Diff d;
  d.thresholds = thresholds;
  if (baseline.trace.platform != current.trace.platform)
    d.notes.push_back("baseline platform '" + baseline.trace.platform + "' differs from '" +
                      current.trace.platform + "'");
  compareFrames(d, baseline, current);
  compareFunctions(d, baseline, current);
  compareScopes(d, baseline, current);
  compareLost(d, baseline, current);
  sortFindings(d.regressions);
  sortFindings(d.improvements);
  return d;
}

}  // namespace tachyon::report
