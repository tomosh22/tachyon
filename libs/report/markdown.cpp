#include "report/markdown.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace tachyon::report {

namespace {

constexpr size_t kMaxFindingRows = 10;
constexpr size_t kMaxImprovementRows = 5;
constexpr size_t kMaxFunctionRows = 10;
constexpr size_t kMaxThreadRows = 8;
constexpr size_t kMaxScopeRows = 5;
constexpr size_t kMaxLockRows = 5;
constexpr size_t kMaxFrameRows = 8;

std::string fmt(const char* format, double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), format, v);
  return buf;
}

std::string ms(double v) {
  return fmt("%.3f", v);
}

std::string uintStr(uint64_t v) {
  return std::to_string(v);
}

// Inline code span that is safe inside a table cell.
std::string code(const std::string& s) {
  if (s.empty()) return "-";
  std::string out = "`";
  for (const char c : s) {
    if (c == '|')
      out += "\\|";
    else if (c == '`')
      out += '\'';
    else if (c == '\n' || c == '\r')
      out += ' ';
    else
      out += c;
  }
  out += '`';
  return out;
}

std::string text(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '|')
      out += "\\|";
    else if (c == '\n' || c == '\r')
      out += ' ';
    else
      out += c;
  }
  return out;
}

std::string threadLabel(uint32_t tid, const std::string& name) {
  return name.empty() ? "tid " + std::to_string(tid) : text(name) + " (" + std::to_string(tid) + ")";
}

std::string signedPct(double baseline, double current) {
  if (!(baseline > 0)) return "new";
  return fmt("%+.1f", (current - baseline) / baseline * 100.0) + "%";
}

std::string valueText(const Finding& f, double v, bool bound) {
  std::string s;
  if (f.valueUnit == "ms")
    s = ms(v) + " ms";
  else if (f.valueUnit == "events")
    s = uintStr(static_cast<uint64_t>(v));
  else
    s = fmt("%.2f", v) + "%";
  return bound ? "<= " + s : s;
}

std::string changeText(const Finding& f) {
  if (!f.change) return "new";
  if (f.changeUnit == "%") return fmt("%+.1f", *f.change) + "%";
  if (f.changeUnit == "pp") return fmt("%+.2f", *f.change) + " pp";
  return fmt("%+.0f", *f.change);
}

const char* categoryText(const std::string& c) {
  if (c == "frame_p95") return "Frame p95";
  if (c == "frame_p99") return "Frame p99";
  if (c == "function_exclusive") return "Function exclusive";
  if (c == "function_exclusive_ms") return "Function exclusive time";
  if (c == "total_cpu") return "Total CPU";
  if (c == "scope_median") return "Scope median";
  if (c == "lost_events") return "Lost events";
  return "Other";
}

void findingsTable(std::string& out, const char* heading, const std::vector<Finding>& v, size_t cap) {
  out += std::string("### ") + heading + "\n\n";
  out += "| Metric | Subject | Baseline | Current | Change |\n|---|---|---:|---:|---:|\n";
  for (size_t i = 0; i < v.size() && i < cap; ++i) {
    const Finding& f = v[i];
    out += std::string("| ") + categoryText(f.category) + " | " + code(f.subject) + " | " +
           valueText(f, f.baseline, f.baselineIsBound) + " | " + valueText(f, f.current, f.currentIsBound) +
           " | " + changeText(f) + " |\n";
  }
  if (v.size() > cap) out += "\n_... and " + std::to_string(v.size() - cap) + " more in the JSON report._\n";
  out += "\n";
}

}  // namespace

std::string renderMarkdown(const Report& r, const Diff* diff) {
  std::string out;
  out += "## Tachyon performance report\n\n";
  if (diff == nullptr) {
    out += "**No baseline** - informational report.\n\n";
  } else if (diff->regressed()) {
    out += "**FAIL** - " + std::to_string(diff->regressions.size()) + " regression" +
           (diff->regressions.size() == 1 ? "" : "s") + " against the baseline (" +
           std::to_string(diff->improvements.size()) + " improvement" +
           (diff->improvements.size() == 1 ? "" : "s") + ").\n\n";
  } else {
    out += "**PASS** - no regressions against the baseline (" + std::to_string(diff->improvements.size()) +
           " improvement" + (diff->improvements.size() == 1 ? "" : "s") + ").\n\n";
  }

  const TraceSummary& t = r.trace;
  out += "Trace: " + fmt("%.2f", t.durationMs / 1000.0) + " s on " + t.platform;
  if (t.samplingHz != 0) out += ", " + uintStr(t.samplingHz) + " Hz sampling";
  out += ", " + uintStr(t.threadCount) + " threads, " + uintStr(t.sampleCount) + " samples, " +
         uintStr(t.eventsLost) + " events lost.\n\n";
  if (t.partial) out += "> Warning: the trace is partial (the capture did not finish cleanly).\n\n";
  if (t.eventsLost > 0)
    out += "> Warning: " + uintStr(t.eventsLost) + " events were lost during the capture.\n\n";
  if (t.throttleEvents > 0)
    out += "> Warning: the kernel throttled sampling " + uintStr(t.throttleEvents) + " time(s).\n\n";
  for (const std::string& w : t.warnings) out += "> Warning: " + text(w) + "\n\n";

  if (diff != nullptr) {
    if (!diff->regressions.empty()) findingsTable(out, "Regressions", diff->regressions, kMaxFindingRows);
    if (!diff->improvements.empty())
      findingsTable(out, "Improvements", diff->improvements, kMaxImprovementRows);
    for (const std::string& n : diff->notes) out += "> Note: " + text(n) + "\n\n";
  }

  out += "### Frame times\n\n";
  if (r.frames.empty()) {
    out += "No frame marks in the trace.\n\n";
  } else {
    const bool withBase = diff != nullptr && !diff->frames.empty();
    std::map<std::string, const FrameBaseline*> base;
    if (diff != nullptr)
      for (const FrameBaseline& b : diff->frames) base.emplace(b.label, &b);
    out += "| Series | Frames | p50 ms | p95 ms | p99 ms | max ms | Over " +
           fmt("%.2f", r.settings.frameBudgetMs) + " ms |";
    out += withBase ? " p95 vs baseline | p99 vs baseline |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|\n"
                    : "\n|---|---:|---:|---:|---:|---:|---:|\n";
    for (size_t i = 0; i < r.frames.size() && i < kMaxFrameRows; ++i) {
      const FrameSeriesRow& f = r.frames[i];
      out += "| " + code(f.label) + " | " + uintStr(f.frameCount) + " | " + ms(f.p50Ms) + " | " +
             ms(f.p95Ms) + " | " + ms(f.p99Ms) + " | " + ms(f.maxMs) + " | " + uintStr(f.overBudget) + " |";
      if (withBase) {
        const auto it = base.find(f.label);
        if (it == base.end())
          out += " - | - |";
        else
          out += " " + signedPct(it->second->p95Ms, f.p95Ms) + " | " + signedPct(it->second->p99Ms, f.p99Ms) +
                 " |";
      }
      out += "\n";
    }
    if (r.frames.size() > kMaxFrameRows)
      out += "\n_... and " + std::to_string(r.frames.size() - kMaxFrameRows) +
             " more series in the JSON report._\n";
    out += "\n";
  }

  out += "### Top functions by exclusive time\n\n";
  if (r.functions.empty()) {
    out += "No samples attributed to functions.\n\n";
  } else {
    out += "| Function | Module | Exclusive ms | Exclusive % | Inclusive ms |\n|---|---|---:|---:|---:|\n";
    for (size_t i = 0; i < r.functions.size() && i < kMaxFunctionRows; ++i) {
      const FunctionReportRow& f = r.functions[i];
      out += "| " + code(f.isScope ? "scope:" + f.name : f.name) + " | " + code(f.module) + " | " +
             ms(f.exclusiveMs) + " | " + fmt("%.2f", f.exclusivePct) + " | " + ms(f.inclusiveMs) + " |\n";
    }
    out += "\n";
  }

  if (!r.threads.empty()) {
    out +=
        "### Threads by running time\n\n| Thread | Running ms | Ready ms | Waiting ms "
        "|\n|---|---:|---:|---:|\n";
    for (size_t i = 0; i < r.threads.size() && i < kMaxThreadRows; ++i) {
      const ThreadRow& th = r.threads[i];
      out += "| " + threadLabel(th.tid, th.name) + " | " + ms(th.runningMs) + " | " + ms(th.readyMs) + " | " +
             ms(th.waitingMs) + " |\n";
    }
    out += "\n";
  }

  if (!r.scopes.empty()) {
    out +=
        "### Instrumentation scopes\n\n| Scope | Count | Median ms | p95 ms | Max ms | Total ms |\n"
        "|---|---:|---:|---:|---:|---:|\n";
    for (size_t i = 0; i < r.scopes.size() && i < kMaxScopeRows; ++i) {
      const ScopeReportRow& s = r.scopes[i];
      out += "| " + code(s.name) + " | " + uintStr(s.count) + " | " + ms(s.medianMs) + " | " + ms(s.p95Ms) +
             " | " + ms(s.maxMs) + " | " + ms(s.totalMs) + " |\n";
    }
    out += "\n";
  }

  if (!r.locks.empty()) {
    out +=
        "### Lock contention\n\n| Lock | Contentions | Total wait ms | Max wait ms |\n|---|---:|---:|---:|\n";
    for (size_t i = 0; i < r.locks.size() && i < kMaxLockRows; ++i) {
      const LockReportRow& l = r.locks[i];
      out += "| " + (l.name.empty() ? std::string("(unnamed)") : code(l.name)) + " | " +
             uintStr(l.contentions) + " | " + ms(l.totalWaitMs) + " | " + ms(l.maxWaitMs) + " |\n";
    }
    out += "\n";
  }

  out += "_tachyon-report, schema " + std::to_string(kSchemaVersion) + "_\n";
  return out;
}

}  // namespace tachyon::report
