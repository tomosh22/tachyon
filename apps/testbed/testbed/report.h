// Result types for scenarios and their JSON form (schema documented in apps/testbed/README.md).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/result.h"

namespace tachyon::testbed {

inline constexpr const char* kReportSchema = "tachyon-testbed/1";

struct ExpectedShare {
  std::string function;
  double share = 0.0;
};

struct MeasuredShare {
  std::string function;
  double share = 0.0;
  uint64_t tscTicks = 0;
};

struct ThreadReport {
  std::string name;
  uint64_t tid = 0;
  bool enforced = true;  // true when the split is enforced by construction (calibrated loops / deadlines)
  double tolerance =
      0.05;  // advisory absolute per-function tolerance for consumers comparing against `expected`
  std::vector<ExpectedShare> expected;
  std::vector<MeasuredShare> measured;
};

struct ScenarioReport {
  std::string name;
  std::string description;
  double plannedDurationS = 0.0;
  double actualDurationS = 0.0;
  std::vector<ThreadReport> threads;
  nlohmann::json details = nlohmann::json::object();
};

struct RunReport {
  std::string platform;
  uint64_t pid = 0;
  std::string mode = "sequential";
  double requestedDurationS = 0.0;
  uint64_t clockFrequencyHz = 0;
  uint64_t tscFrequencyHz = 0;
  double kernelItersPerSecond = 0.0;
  std::vector<ScenarioReport> scenarios;
};

// Thread-safe: pure function. Turns per-function tick totals into shares that sum to 1 (all zero when the
// total is 0).
std::vector<MeasuredShare> makeMeasured(const std::vector<std::pair<std::string, uint64_t>>& ticks);

// Thread-safe: pure function. Builds the expected list from (function, share) pairs.
std::vector<ExpectedShare> makeExpected(const std::vector<std::pair<std::string, double>>& shares);

// Thread-safe: pure function.
nlohmann::json toJson(const RunReport& report);

// Thread-safe: pure function. Checks `json` against the documented schema: required keys and types, at least
// one scenario/thread, every share in [0,1], and expected and measured shares summing to 1 (within 1e-6) per
// thread.
Result<void> validateReportJson(const nlohmann::json& json);

}  // namespace tachyon::testbed
