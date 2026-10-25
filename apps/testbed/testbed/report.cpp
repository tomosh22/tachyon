#include "testbed/report.h"

#include <cmath>

namespace tachyon::testbed {

using nlohmann::json;

std::vector<MeasuredShare> makeMeasured(const std::vector<std::pair<std::string, uint64_t>>& ticks) {
  uint64_t total = 0;
  for (const auto& entry : ticks) total += entry.second;
  std::vector<MeasuredShare> out;
  out.reserve(ticks.size());
  for (const auto& entry : ticks) {
    const double share = total == 0 ? 0.0 : static_cast<double>(entry.second) / static_cast<double>(total);
    out.push_back(MeasuredShare{entry.first, share, entry.second});
  }
  return out;
}

std::vector<ExpectedShare> makeExpected(const std::vector<std::pair<std::string, double>>& shares) {
  std::vector<ExpectedShare> out;
  out.reserve(shares.size());
  for (const auto& entry : shares) out.push_back(ExpectedShare{entry.first, entry.second});
  return out;
}

static json threadToJson(const ThreadReport& thread) {
  json expected = json::array();
  for (const auto& e : thread.expected) expected.push_back({{"function", e.function}, {"share", e.share}});
  json measured = json::array();
  for (const auto& m : thread.measured) {
    measured.push_back({{"function", m.function}, {"share", m.share}, {"tsc_ticks", m.tscTicks}});
  }
  return {{"name", thread.name},
          {"tid", thread.tid},
          {"enforced", thread.enforced},
          {"tolerance", thread.tolerance},
          {"expected", std::move(expected)},
          {"measured", std::move(measured)}};
}

json toJson(const RunReport& report) {
  json scenarios = json::array();
  for (const auto& scenario : report.scenarios) {
    json threads = json::array();
    for (const auto& thread : scenario.threads) threads.push_back(threadToJson(thread));
    scenarios.push_back({{"name", scenario.name},
                         {"description", scenario.description},
                         {"planned_duration_s", scenario.plannedDurationS},
                         {"actual_duration_s", scenario.actualDurationS},
                         {"threads", std::move(threads)},
                         {"details", scenario.details}});
  }
  return {{"schema", kReportSchema},
          {"tool", "tachyon-testbed"},
          {"platform", report.platform},
          {"pid", report.pid},
          {"mode", report.mode},
          {"requested_duration_s", report.requestedDurationS},
          {"clock_frequency_hz", report.clockFrequencyHz},
          {"tsc_frequency_hz", report.tscFrequencyHz},
          {"kernel_iters_per_second", report.kernelItersPerSecond},
          {"scenarios", std::move(scenarios)}};
}

namespace {

Error invalid(const std::string& message) {
  return makeError(ErrorCode::CorruptData, message);
}

// Returns an error message, or empty when `list` is a valid share list ("expected"/"measured") summing to 1.
std::string checkShareList(const json& list, const char* what) {
  if (!list.is_array() || list.empty()) return std::string(what) + " must be a non-empty array";
  double sum = 0.0;
  for (const auto& entry : list) {
    if (!entry.is_object() || !entry.contains("function") || !entry["function"].is_string() ||
        !entry.contains("share") || !entry["share"].is_number()) {
      return std::string(what) + " entries need string `function` and numeric `share`";
    }
    const double share = entry["share"].get<double>();
    if (!(share >= 0.0 && share <= 1.0)) return std::string(what) + " share out of [0,1]";
    sum += share;
  }
  if (std::fabs(sum - 1.0) > 1e-6)
    return std::string(what) + " shares sum to " + std::to_string(sum) + ", not 1";
  return {};
}

}  // namespace

Result<void> validateReportJson(const json& j) {
  if (!j.is_object()) return invalid("report is not an object");
  if (j.value("schema", std::string()) != kReportSchema) return invalid("missing or unknown `schema`");
  const char* strings[] = {"tool", "platform", "mode"};
  for (const char* key : strings) {
    if (!j.contains(key) || !j[key].is_string()) return invalid(std::string("missing string `") + key + "`");
  }
  const char* numbers[] = {"pid", "requested_duration_s", "clock_frequency_hz", "tsc_frequency_hz",
                           "kernel_iters_per_second"};
  for (const char* key : numbers) {
    if (!j.contains(key) || !j[key].is_number()) return invalid(std::string("missing number `") + key + "`");
  }
  if (!j.contains("scenarios") || !j["scenarios"].is_array() || j["scenarios"].empty()) {
    return invalid("`scenarios` must be a non-empty array");
  }
  for (const auto& scenario : j["scenarios"]) {
    if (!scenario.is_object() || !scenario.contains("name") || !scenario["name"].is_string() ||
        !scenario.contains("description") || !scenario["description"].is_string() ||
        !scenario.contains("planned_duration_s") || !scenario["planned_duration_s"].is_number() ||
        !scenario.contains("actual_duration_s") || !scenario["actual_duration_s"].is_number() ||
        !scenario.contains("details") || !scenario["details"].is_object()) {
      return invalid("scenario is missing name/description/durations/details");
    }
    const std::string name = scenario["name"].get<std::string>();
    if (!scenario.contains("threads") || !scenario["threads"].is_array() || scenario["threads"].empty()) {
      return invalid("scenario `" + name + "` needs a non-empty `threads` array");
    }
    for (const auto& thread : scenario["threads"]) {
      if (!thread.is_object() || !thread.contains("name") || !thread["name"].is_string() ||
          thread["name"].get<std::string>().empty() || !thread.contains("tid") ||
          !thread["tid"].is_number_unsigned() || thread["tid"].get<uint64_t>() == 0 ||
          !thread.contains("enforced") || !thread["enforced"].is_boolean() || !thread.contains("tolerance") ||
          !thread["tolerance"].is_number()) {
        return invalid("scenario `" + name + "` has a thread missing name/tid/enforced/tolerance");
      }
      for (const char* key : {"expected", "measured"}) {
        if (!thread.contains(key)) return invalid("scenario `" + name + "` thread is missing `" + key + "`");
        const std::string error = checkShareList(thread[key], key);
        if (!error.empty())
          return invalid("scenario `" + name + "` thread `" + thread["name"].get<std::string>() +
                         "`: " + error);
      }
      for (const auto& m : thread["measured"]) {
        if (!m.contains("tsc_ticks") || !m["tsc_ticks"].is_number_unsigned()) {
          return invalid("scenario `" + name + "` measured entry needs unsigned `tsc_ticks`");
        }
      }
    }
  }
  return ok();
}

}  // namespace tachyon::testbed
