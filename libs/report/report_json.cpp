#include "report/report_json.h"

#include <cmath>
#include <limits>
#include <type_traits>

namespace tachyon::report {

namespace {

using nlohmann::json;
using nlohmann::ordered_json;

// ---- writing
// ------------------------------------------------------------------------------------------------

ordered_json traceToJson(const TraceSummary& t) {
  ordered_json j;
  j["duration_ms"] = t.durationMs;
  j["platform"] = t.platform;
  j["clock_frequency_hz"] = t.clockFrequencyHz;
  j["sampling_hz"] = t.samplingHz;
  j["events_lost"] = t.eventsLost;
  j["partial"] = t.partial;
  j["throttle_events"] = t.throttleEvents;
  j["thread_count"] = t.threadCount;
  j["sample_count"] = t.sampleCount;
  j["has_instrumentation"] = t.hasInstrumentation;
  j["warnings"] = t.warnings;
  j["total_cpu_ms"] = t.totalCpuMs;
  return j;
}

ordered_json settingsToJson(const ReportSettings& s) {
  ordered_json j;
  j["top_n"] = s.topN;
  j["frame_budget_ms"] = s.frameBudgetMs;
  j["blend_scopes"] = s.blendScopes;
  j["frame_series"] = s.frameSeriesFilter ? ordered_json(*s.frameSeriesFilter) : ordered_json(nullptr);
  return j;
}

ordered_json frameSeriesToJson(const FrameSeriesRow& f) {
  ordered_json j;
  j["label"] = f.label;
  j["name"] = f.name;
  if (f.threadTid) {
    ordered_json t;
    t["tid"] = *f.threadTid;
    t["name"] = f.threadName;
    j["thread"] = std::move(t);
  } else {
    j["thread"] = nullptr;
  }
  j["default"] = f.isDefault;
  j["mark_count"] = f.markCount;
  j["frame_count"] = f.frameCount;
  j["p50_ms"] = f.p50Ms;
  j["p95_ms"] = f.p95Ms;
  j["p99_ms"] = f.p99Ms;
  j["max_ms"] = f.maxMs;
  j["mean_ms"] = f.meanMs;
  j["over_budget_frames"] = f.overBudget;
  j["slowest_frame_number"] = f.slowestFrameNumber;
  return j;
}

ordered_json threadToJson(const ThreadRow& t) {
  ordered_json j;
  j["tid"] = t.tid;
  j["name"] = t.name;
  j["running_ms"] = t.runningMs;
  j["ready_ms"] = t.readyMs;
  j["waiting_ms"] = t.waitingMs;
  j["unknown_ms"] = t.unknownMs;
  j["sample_count"] = t.sampleCount;
  return j;
}

ordered_json functionToJson(const FunctionReportRow& f) {
  ordered_json j;
  j["name"] = f.name;
  j["module"] = f.module;
  j["kind"] = f.isScope ? "scope" : "function";
  j["exclusive_ms"] = f.exclusiveMs;
  j["inclusive_ms"] = f.inclusiveMs;
  j["exclusive_pct"] = f.exclusivePct;
  j["samples"] = f.samples;
  return j;
}

ordered_json scopeToJson(const ScopeReportRow& s) {
  ordered_json j;
  j["name"] = s.name;
  j["count"] = s.count;
  j["min_ms"] = s.minMs;
  j["median_ms"] = s.medianMs;
  j["p95_ms"] = s.p95Ms;
  j["max_ms"] = s.maxMs;
  j["total_ms"] = s.totalMs;
  j["mean_ms"] = s.meanMs;
  j["unterminated"] = s.unterminated;
  j["orphan_ends"] = s.orphanEnds;
  return j;
}

ordered_json lockToJson(const LockReportRow& l) {
  ordered_json j;
  j["name"] = l.name;
  j["address"] = l.address;
  j["contentions"] = l.contentions;
  j["total_wait_ms"] = l.totalWaitMs;
  j["max_wait_ms"] = l.maxWaitMs;
  j["unterminated"] = l.unterminated;
  ordered_json threads = ordered_json::array();
  for (const LockWaiterRow& w : l.topThreads) {
    ordered_json t;
    t["tid"] = w.tid;
    t["name"] = w.name;
    t["waits"] = w.waits;
    t["wait_ms"] = w.waitMs;
    threads.push_back(std::move(t));
  }
  j["top_threads"] = std::move(threads);
  return j;
}

ordered_json findingToJson(const Finding& f) {
  ordered_json j;
  j["category"] = f.category;
  j["subject"] = f.subject;
  j["baseline"] = f.baseline;
  j["current"] = f.current;
  j["value_unit"] = f.valueUnit;
  j["change"] = f.change ? ordered_json(*f.change) : ordered_json(nullptr);
  j["change_unit"] = f.changeUnit;
  if (f.baselineIsBound) j["baseline_is_upper_bound"] = true;
  if (f.currentIsBound) j["current_is_upper_bound"] = true;
  return j;
}

// ---- reading
// ------------------------------------------------------------------------------------------------

struct Reader {
  std::string error;

  bool fail(const std::string& path, const char* what) {
    if (error.empty()) error = path + ": " + what;
    return false;
  }

  const json* member(const json& o, const std::string& path, const char* key) {
    if (!o.is_object()) {
      fail(path, "expected an object");
      return nullptr;
    }
    const auto it = o.find(key);
    if (it == o.end()) {
      fail(path + "." + key, "missing");
      return nullptr;
    }
    return &*it;
  }

  template <typename T>
  bool get(const json& o, const std::string& path, const char* key, T& out) {
    const json* v = member(o, path, key);
    if (v == nullptr) return false;
    const std::string where = path + "." + key;
    if constexpr (std::is_same_v<T, bool>) {
      if (!v->is_boolean()) return fail(where, "expected a boolean");
      out = v->get<bool>();
    } else if constexpr (std::is_same_v<T, std::string>) {
      if (!v->is_string()) return fail(where, "expected a string");
      out = v->get<std::string>();
    } else if constexpr (std::is_floating_point_v<T>) {
      if (!v->is_number()) return fail(where, "expected a number");
      out = v->get<double>();
      if (!std::isfinite(out)) return fail(where, "not finite");
    } else {
      static_assert(std::is_unsigned_v<T>);
      if (!v->is_number_integer() && !v->is_number_unsigned()) return fail(where, "expected an integer");
      if (v->is_number_integer() && !v->is_number_unsigned() && v->get<int64_t>() < 0)
        return fail(where, "expected a non-negative integer");
      const uint64_t u = v->get<uint64_t>();
      if (u > std::numeric_limits<T>::max()) return fail(where, "out of range");
      out = static_cast<T>(u);
    }
    return true;
  }

  const json* array(const json& o, const std::string& path, const char* key) {
    const json* v = member(o, path, key);
    if (v == nullptr) return nullptr;
    if (!v->is_array()) {
      fail(path + "." + key, "expected an array");
      return nullptr;
    }
    return v;
  }
};

}  // namespace

ordered_json reportToJson(const Report& r, const Diff* diff) {
  ordered_json j;
  j["schema_version"] = kSchemaVersion;
  j["tool"] = "tachyon-report";
  j["trace"] = traceToJson(r.trace);
  j["settings"] = settingsToJson(r.settings);
  {
    ordered_json frames;
    frames["series_total"] = r.frameSeriesTotal;
    ordered_json list = ordered_json::array();
    for (const FrameSeriesRow& f : r.frames) list.push_back(frameSeriesToJson(f));
    frames["series"] = std::move(list);
    j["frames"] = std::move(frames);
  }
  {
    ordered_json list = ordered_json::array();
    for (const ThreadRow& t : r.threads) list.push_back(threadToJson(t));
    j["threads"] = std::move(list);
  }
  {
    ordered_json fns;
    fns["total_exclusive_ms"] = r.totalExclusiveMs;
    fns["function_count"] = r.functionCount;
    ordered_json list = ordered_json::array();
    for (const FunctionReportRow& f : r.functions) list.push_back(functionToJson(f));
    fns["top"] = std::move(list);
    if (r.significantFloorPct >= 0) {
      fns["significant_floor_pct"] = r.significantFloorPct;
      ordered_json sig = ordered_json::array();
      for (const FunctionReportRow& f : r.significantFunctions) sig.push_back(functionToJson(f));
      fns["significant"] = std::move(sig);
    }
    j["functions"] = std::move(fns);
  }
  {
    ordered_json sc;
    sc["scope_count"] = r.scopeCount;
    ordered_json list = ordered_json::array();
    for (const ScopeReportRow& s : r.scopes) list.push_back(scopeToJson(s));
    sc["top"] = std::move(list);
    j["scopes"] = std::move(sc);
  }
  {
    ordered_json lk;
    lk["lock_count"] = r.lockCount;
    lk["unparsed_waits"] = r.unparsedLockWaits;
    ordered_json list = ordered_json::array();
    for (const LockReportRow& l : r.locks) list.push_back(lockToJson(l));
    lk["top"] = std::move(list);
    j["locks"] = std::move(lk);
  }
  if (diff != nullptr) j["baseline_diff"] = diffToJson(*diff);
  return j;
}

ordered_json diffToJson(const Diff& d) {
  ordered_json j;
  j["verdict"] = d.regressed() ? "regression" : "pass";
  j["regression_count"] = d.regressions.size();
  j["improvement_count"] = d.improvements.size();
  j["thresholds"] = thresholdsToJson(d.thresholds);
  ordered_json reg = ordered_json::array(), imp = ordered_json::array(), notes = ordered_json::array();
  for (const Finding& f : d.regressions) reg.push_back(findingToJson(f));
  for (const Finding& f : d.improvements) imp.push_back(findingToJson(f));
  for (const std::string& n : d.notes) notes.push_back(n);
  j["regressions"] = std::move(reg);
  j["improvements"] = std::move(imp);
  j["notes"] = std::move(notes);
  return j;
}

Result<Report> reportFromJson(const json& doc) {
  Reader rd;
  Report r;
  auto bad = [&]() -> Result<Report> { return makeError(ErrorCode::CorruptData, "report: " + rd.error); };
  if (!doc.is_object()) {
    rd.fail("$", "expected an object");
    return bad();
  }
  uint32_t version = 0;
  if (!rd.get(doc, "$", "schema_version", version)) return bad();
  if (version != static_cast<uint32_t>(kSchemaVersion))
    return makeError(ErrorCode::CorruptData, "report: unsupported schema_version " + std::to_string(version) +
                                                 " (expected " + std::to_string(kSchemaVersion) + ")");

  const json* t = rd.member(doc, "$", "trace");
  if (t == nullptr) return bad();
  TraceSummary& ts = r.trace;
  if (!(rd.get(*t, "$.trace", "duration_ms", ts.durationMs) &&
        rd.get(*t, "$.trace", "platform", ts.platform) &&
        rd.get(*t, "$.trace", "clock_frequency_hz", ts.clockFrequencyHz) &&
        rd.get(*t, "$.trace", "sampling_hz", ts.samplingHz) &&
        rd.get(*t, "$.trace", "events_lost", ts.eventsLost) && rd.get(*t, "$.trace", "partial", ts.partial) &&
        rd.get(*t, "$.trace", "throttle_events", ts.throttleEvents) &&
        rd.get(*t, "$.trace", "thread_count", ts.threadCount) &&
        rd.get(*t, "$.trace", "sample_count", ts.sampleCount) &&
        rd.get(*t, "$.trace", "has_instrumentation", ts.hasInstrumentation)))
    return bad();
  if (const json* w = rd.array(*t, "$.trace", "warnings"); w != nullptr) {
    for (const json& e : *w) {
      if (!e.is_string()) {
        rd.fail("$.trace.warnings", "expected strings");
        return bad();
      }
      ts.warnings.push_back(e.get<std::string>());
    }
  } else {
    return bad();
  }

  if (const auto it = t->find("total_cpu_ms"); it != t->end()) {  // optional: absent before WP-69
    if (!rd.get(*t, "$.trace", "total_cpu_ms", ts.totalCpuMs)) return bad();
  }

  const json* s = rd.member(doc, "$", "settings");
  if (s == nullptr) return bad();
  if (!(rd.get(*s, "$.settings", "top_n", r.settings.topN) &&
        rd.get(*s, "$.settings", "frame_budget_ms", r.settings.frameBudgetMs) &&
        rd.get(*s, "$.settings", "blend_scopes", r.settings.blendScopes)))
    return bad();
  const json* fsf = rd.member(*s, "$.settings", "frame_series");
  if (fsf == nullptr) return bad();
  if (fsf->is_string())
    r.settings.frameSeriesFilter = fsf->get<std::string>();
  else if (!fsf->is_null()) {
    rd.fail("$.settings.frame_series", "expected a string or null");
    return bad();
  }

  const json* fr = rd.member(doc, "$", "frames");
  if (fr == nullptr || !rd.get(*fr, "$.frames", "series_total", r.frameSeriesTotal)) return bad();
  const json* series = rd.array(*fr, "$.frames", "series");
  if (series == nullptr) return bad();
  for (const json& e : *series) {
    FrameSeriesRow f;
    if (!(rd.get(e, "$.frames.series[]", "label", f.label) &&
          rd.get(e, "$.frames.series[]", "name", f.name) &&
          rd.get(e, "$.frames.series[]", "default", f.isDefault) &&
          rd.get(e, "$.frames.series[]", "mark_count", f.markCount) &&
          rd.get(e, "$.frames.series[]", "frame_count", f.frameCount) &&
          rd.get(e, "$.frames.series[]", "p50_ms", f.p50Ms) &&
          rd.get(e, "$.frames.series[]", "p95_ms", f.p95Ms) &&
          rd.get(e, "$.frames.series[]", "p99_ms", f.p99Ms) &&
          rd.get(e, "$.frames.series[]", "max_ms", f.maxMs) &&
          rd.get(e, "$.frames.series[]", "mean_ms", f.meanMs) &&
          rd.get(e, "$.frames.series[]", "over_budget_frames", f.overBudget) &&
          rd.get(e, "$.frames.series[]", "slowest_frame_number", f.slowestFrameNumber)))
      return bad();
    const json* th = rd.member(e, "$.frames.series[]", "thread");
    if (th == nullptr) return bad();
    if (th->is_object()) {
      uint32_t tid = 0;
      if (!(rd.get(*th, "$.frames.series[].thread", "tid", tid) &&
            rd.get(*th, "$.frames.series[].thread", "name", f.threadName)))
        return bad();
      f.threadTid = tid;
    } else if (!th->is_null()) {
      rd.fail("$.frames.series[].thread", "expected an object or null");
      return bad();
    }
    r.frames.push_back(std::move(f));
  }

  const json* threads = rd.array(doc, "$", "threads");
  if (threads == nullptr) return bad();
  for (const json& e : *threads) {
    ThreadRow t2;
    if (!(rd.get(e, "$.threads[]", "tid", t2.tid) && rd.get(e, "$.threads[]", "name", t2.name) &&
          rd.get(e, "$.threads[]", "running_ms", t2.runningMs) &&
          rd.get(e, "$.threads[]", "ready_ms", t2.readyMs) &&
          rd.get(e, "$.threads[]", "waiting_ms", t2.waitingMs) &&
          rd.get(e, "$.threads[]", "unknown_ms", t2.unknownMs) &&
          rd.get(e, "$.threads[]", "sample_count", t2.sampleCount)))
      return bad();
    r.threads.push_back(std::move(t2));
  }

  const json* fn = rd.member(doc, "$", "functions");
  if (fn == nullptr || !rd.get(*fn, "$.functions", "total_exclusive_ms", r.totalExclusiveMs) ||
      !rd.get(*fn, "$.functions", "function_count", r.functionCount))
    return bad();
  const json* top = rd.array(*fn, "$.functions", "top");
  if (top == nullptr) return bad();
  auto readFunctions = [&](const json& list, const char* path, std::vector<FunctionReportRow>& dst) {
    const std::string p = std::string(path) + "[]";
    for (const json& e : list) {
      FunctionReportRow f;
      std::string kind;
      if (!(rd.get(e, p, "name", f.name) && rd.get(e, p, "module", f.module) && rd.get(e, p, "kind", kind) &&
            rd.get(e, p, "exclusive_ms", f.exclusiveMs) && rd.get(e, p, "inclusive_ms", f.inclusiveMs) &&
            rd.get(e, p, "exclusive_pct", f.exclusivePct) && rd.get(e, p, "samples", f.samples)))
        return false;
      f.isScope = kind == "scope";
      dst.push_back(std::move(f));
    }
    return true;
  };
  if (!readFunctions(*top, "$.functions.top", r.functions)) return bad();
  if (const auto it = fn->find("significant"); it != fn->end()) {  // optional: absent before WP-69
    if (!it->is_array()) {
      rd.fail("$.functions.significant", "expected an array");
      return bad();
    }
    if (!rd.get(*fn, "$.functions", "significant_floor_pct", r.significantFloorPct) ||
        !readFunctions(*it, "$.functions.significant", r.significantFunctions))
      return bad();
  }

  const json* sc = rd.member(doc, "$", "scopes");
  if (sc == nullptr || !rd.get(*sc, "$.scopes", "scope_count", r.scopeCount)) return bad();
  const json* stop = rd.array(*sc, "$.scopes", "top");
  if (stop == nullptr) return bad();
  for (const json& e : *stop) {
    ScopeReportRow x;
    if (!(rd.get(e, "$.scopes.top[]", "name", x.name) && rd.get(e, "$.scopes.top[]", "count", x.count) &&
          rd.get(e, "$.scopes.top[]", "min_ms", x.minMs) &&
          rd.get(e, "$.scopes.top[]", "median_ms", x.medianMs) &&
          rd.get(e, "$.scopes.top[]", "p95_ms", x.p95Ms) && rd.get(e, "$.scopes.top[]", "max_ms", x.maxMs) &&
          rd.get(e, "$.scopes.top[]", "total_ms", x.totalMs) &&
          rd.get(e, "$.scopes.top[]", "mean_ms", x.meanMs) &&
          rd.get(e, "$.scopes.top[]", "unterminated", x.unterminated) &&
          rd.get(e, "$.scopes.top[]", "orphan_ends", x.orphanEnds)))
      return bad();
    r.scopes.push_back(std::move(x));
  }

  const json* lk = rd.member(doc, "$", "locks");
  if (lk == nullptr || !rd.get(*lk, "$.locks", "lock_count", r.lockCount) ||
      !rd.get(*lk, "$.locks", "unparsed_waits", r.unparsedLockWaits))
    return bad();
  const json* ltop = rd.array(*lk, "$.locks", "top");
  if (ltop == nullptr) return bad();
  for (const json& e : *ltop) {
    LockReportRow x;
    if (!(rd.get(e, "$.locks.top[]", "name", x.name) && rd.get(e, "$.locks.top[]", "address", x.address) &&
          rd.get(e, "$.locks.top[]", "contentions", x.contentions) &&
          rd.get(e, "$.locks.top[]", "total_wait_ms", x.totalWaitMs) &&
          rd.get(e, "$.locks.top[]", "max_wait_ms", x.maxWaitMs) &&
          rd.get(e, "$.locks.top[]", "unterminated", x.unterminated)))
      return bad();
    const json* tt = rd.array(e, "$.locks.top[]", "top_threads");
    if (tt == nullptr) return bad();
    for (const json& w : *tt) {
      LockWaiterRow wr;
      if (!(rd.get(w, "$.locks.top[].top_threads[]", "tid", wr.tid) &&
            rd.get(w, "$.locks.top[].top_threads[]", "name", wr.name) &&
            rd.get(w, "$.locks.top[].top_threads[]", "waits", wr.waits) &&
            rd.get(w, "$.locks.top[].top_threads[]", "wait_ms", wr.waitMs)))
        return bad();
      x.topThreads.push_back(std::move(wr));
    }
    r.locks.push_back(std::move(x));
  }
  return r;
}

std::string dumpJson(const ordered_json& doc) {
  std::string s = doc.dump(2, ' ', false, ordered_json::error_handler_t::replace);
  s.push_back('\n');
  return s;
}

std::string dumpJson(const json& doc) {
  std::string s = doc.dump(2, ' ', false, json::error_handler_t::replace);
  s.push_back('\n');
  return s;
}

}  // namespace tachyon::report
