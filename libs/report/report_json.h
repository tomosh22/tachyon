// JSON encoding of a Report (schema version 1, apps/report/report.schema.json).
#pragma once

#include "core/result.h"
#include "report/baseline.h"
#include "report/report.h"

#include <nlohmann/json.hpp>

#include <string>

namespace tachyon::report {

// Thread-safe: pure function. The report document; `diff` (optional) is added as "baseline_diff".
nlohmann::ordered_json reportToJson(const Report& report, const Diff* diff = nullptr);
nlohmann::ordered_json diffToJson(const Diff& diff);

// Thread-safe: pure function. Strict reader of a document written by reportToJson (any other
// schema_version, a missing section or a field of the wrong type is CorruptData). Unknown keys, including
// "baseline_diff", are ignored.
Result<Report> reportFromJson(const nlohmann::json& doc);

// Thread-safe: pure function. Two-space indented text with a trailing newline; invalid UTF-8 in strings is
// replaced, so this never throws.
std::string dumpJson(const nlohmann::ordered_json& doc);
std::string dumpJson(const nlohmann::json& doc);

}  // namespace tachyon::report
