// Baseline comparison (WP-62): compares a Report with the Report of a previous run and lists regressions and
// improvements against configurable thresholds. Functions and scopes are matched by display name and module,
// frame series by label ("name@thread"), because FunctionKeys and indices are not stable across builds.
#pragma once

#include "core/result.h"
#include "report/report.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::report {

// Defaults are documented in apps/report/README.md. A thresholds file is a JSON object with any of these
// snake_case keys (see thresholdsFromJson); omitted keys keep the defaults below.
struct Thresholds {
  // Frame p95 / p99 regress when they grow by more than this many percent AND by more than frameAbsFloorMs.
  double frameP95RelPct = 10.0;
  double frameP99RelPct = 15.0;
  double frameAbsFloorMs = 0.5;
  // A function regresses when its share of the total exclusive time grows by more than this many percentage
  // points (improves when it shrinks by more).
  double functionExclusivePp = 2.0;
  // A function listed exactly in both reports also regresses when its absolute exclusive time grows by more
  // than this many percent AND by more than the floor (improves likewise). Catches growth in a flat profile
  // that shifts shares by less than functionExclusivePp.
  double functionExclusiveRelPct = 25.0;
  double functionExclusiveAbsFloorMs = 5.0;
  // Total CPU (running time of all threads) regresses when it grows by more than this many percent AND by
  // more than the floor. Skipped when either report has no total (baselines written before WP-69).
  double totalCpuRelPct = 10.0;
  double totalCpuAbsFloorMs = 10.0;
  // A scope's median regresses when it grows by more than this many percent AND by more than the floor.
  double scopeMedianRelPct = 25.0;
  double scopeAbsFloorMs = 0.05;
  // The number of additional lost events tolerated (lost events regress when current > baseline + this).
  uint64_t lostEventsAllowed = 0;
};

// Thread-safe: pure functions. Strict parse: unknown keys, non-numbers and negative values are rejected
// (InvalidArgument).
Result<Thresholds> thresholdsFromJson(const nlohmann::json& j);
nlohmann::ordered_json thresholdsToJson(const Thresholds& t);

enum class FindingKind : uint8_t { Regression, Improvement };

struct Finding {
  FindingKind kind = FindingKind::Regression;
  // frame_p95, frame_p99, function_exclusive (share, pp), function_exclusive_ms (absolute), total_cpu,
  // scope_median, lost_events
  std::string category;
  std::string subject;  // series label, "module!function" or "scope:name"; "" for lost_events
  double baseline = 0, current = 0;
  std::string valueUnit;         // "ms", "% of total", "events"
  std::optional<double> change;  // relative change in percent, percentage points, or events
  std::string changeUnit;        // "%", "pp", "events"
  // function_exclusive only: the baseline (or current) value is an upper bound because the function was not
  // in that report's function set (baseline.functions.significant, or only the top N for old reports).
  bool baselineIsBound = false, currentIsBound = false;
};

struct FrameBaseline {
  std::string label;
  double p95Ms = 0, p99Ms = 0;
};

struct Diff {
  Thresholds thresholds;
  std::vector<Finding> regressions, improvements;  // sorted: category order, then |change| desc, then subject
  std::vector<FrameBaseline> frames;               // baseline values of the matched frame series
  std::vector<std::string> notes;                  // things that could not be compared

  // Thread-safe (value type).
  bool regressed() const { return !regressions.empty(); }
};

// Thread-safe: pure function.
Diff compareReports(const Report& baseline, const Report& current, const Thresholds& thresholds);

}  // namespace tachyon::report
