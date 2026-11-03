#include "report/report_cli.h"

#include "report/analyze.h"
#include "report/baseline.h"
#include "report/markdown.h"
#include "report/report_json.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>

namespace tachyon::report {

namespace {

constexpr const char* kUsage =
    "usage: tachyon-report <trace.tachyon> [options]\n"
    "  --baseline FILE        compare with a previous report.json; exit 1 on a regression\n"
    "  --out FILE             write the report; format by extension (.json, .md); repeatable\n"
    "  --format json|md       stdout format when no --out is given (default json)\n"
    "  --top N                functions, scopes and locks listed (default 20)\n"
    "  --frame-series NAME    report only the frame series with this name\n"
    "  --frame-budget-ms MS   frame budget for the over-budget count (default 16.67)\n"
    "  --thresholds FILE      baseline thresholds (JSON object, see apps/report/README.md)\n"
    "  --symbols PATH...      symbol files (.json) or symbol search directories\n"
    "  --symbol-server URL... symbol servers (HTTP, debuginfod or local stores)\n"
    "  --symbol-cache DIR     local symbol cache\n"
    "  --no-blend             do not blend instrumentation scopes into the function table\n"
    "  --fail-on-lost         exit 4 when the trace is partial or lost events\n"
    "exit codes: 0 ok, 1 regression, 2 usage, 3 trace/IO error, 4 partial or lost events\n";

enum class Format { Json, Markdown };

struct Parsed {
  bool help = false;
  std::filesystem::path trace;
  std::optional<std::filesystem::path> baseline;
  std::vector<std::filesystem::path> outs;
  std::optional<Format> format;
  uint32_t top = 20;
  std::optional<std::string> frameSeries;
  double budgetMs = 16.67;
  std::optional<std::filesystem::path> thresholds;
  std::vector<std::filesystem::path> symbols;
  std::vector<std::string> servers;
  std::filesystem::path cache;
  bool noBlend = false;
  bool failOnLost = false;
};

bool parseUint(const std::string& s, uint32_t& out) {
  uint32_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}

bool parseDouble(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end != s.c_str() + s.size() || !std::isfinite(v)) return false;
  out = v;
  return true;
}

std::optional<Format> formatOfName(const std::string& ext) {
  if (ext == "json") return Format::Json;
  if (ext == "md" || ext == "markdown") return Format::Markdown;
  return std::nullopt;
}

std::optional<Format> formatOfPath(const std::filesystem::path& p) {
  std::string ext = p.extension().generic_string();
  if (!ext.empty()) ext.erase(0, 1);
  return formatOfName(ext);
}

// Returns an error message, empty on success.
std::string parseArgs(const std::vector<std::string>& args, Parsed& p) {
  bool haveTrace = false;
  for (size_t i = 0; i < args.size(); ++i) {
    std::string arg = args[i];
    std::optional<std::string> inlineValue;
    if (arg.rfind("--", 0) == 0) {
      if (const size_t eq = arg.find('='); eq != std::string::npos) {
        inlineValue = arg.substr(eq + 1);
        arg.erase(eq);
      }
    }
    // Next value of an option: the inline one or the following argument.
    auto value = [&](std::string& v) -> bool {
      if (inlineValue) {
        v = *inlineValue;
        return true;
      }
      if (i + 1 >= args.size()) return false;
      v = args[++i];
      return true;
    };
    std::string v;
    if (arg == "--help" || arg == "-h") {
      p.help = true;
    } else if (arg == "--no-blend") {
      p.noBlend = true;
    } else if (arg == "--fail-on-lost") {
      p.failOnLost = true;
    } else if (arg == "--baseline") {
      if (!value(v)) return "--baseline needs a file";
      p.baseline = v;
    } else if (arg == "--out") {
      if (!value(v)) return "--out needs a file";
      if (!formatOfPath(v)) return "--out: cannot infer the format of '" + v + "' (use .json or .md)";
      p.outs.emplace_back(v);
    } else if (arg == "--format") {
      if (!value(v)) return "--format needs json or md";
      p.format = formatOfName(v);
      if (!p.format) return "--format: expected json or md, got '" + v + "'";
    } else if (arg == "--top") {
      uint32_t n = 0;
      if (!value(v) || !parseUint(v, n) || n == 0 || n > 10000) return "--top needs an integer in 1..10000";
      p.top = n;
    } else if (arg == "--frame-series") {
      if (!value(v) || v.empty()) return "--frame-series needs a name";
      p.frameSeries = v;
    } else if (arg == "--frame-budget-ms") {
      double d = 0;
      if (!value(v) || !parseDouble(v, d) || !(d > 0)) return "--frame-budget-ms needs a positive number";
      p.budgetMs = d;
    } else if (arg == "--thresholds") {
      if (!value(v)) return "--thresholds needs a file";
      p.thresholds = v;
    } else if (arg == "--symbol-cache") {
      if (!value(v)) return "--symbol-cache needs a directory";
      p.cache = v;
    } else if (arg == "--symbols" || arg == "--symbol-server") {
      // One or more values: every following argument up to the next option. Before the trace path is known
      // only one value is taken, so that `--symbols a.json trace.tachyon` stays unambiguous.
      if (!value(v)) return arg + " needs a value";
      auto add = [&](const std::string& s) {
        if (arg == "--symbols")
          p.symbols.emplace_back(s);
        else
          p.servers.push_back(s);
      };
      add(v);
      if (!inlineValue && haveTrace) {
        while (i + 1 < args.size() && args[i + 1].rfind("-", 0) != 0) add(args[++i]);
      }
    } else if (arg.rfind("-", 0) == 0 && arg.size() > 1) {
      return "unknown option '" + arg + "'";
    } else {
      if (haveTrace) return "unexpected argument '" + arg + "' (one trace file is expected)";
      p.trace = arg;
      haveTrace = true;
    }
  }
  if (p.help) return {};
  if (!haveTrace) return "no trace file given";
  return {};
}

Result<std::string> readFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return makeError(ErrorCode::NotFound, "cannot open " + path.generic_string());
  std::ostringstream ss;
  ss << f.rdbuf();
  if (f.bad()) return makeError(ErrorCode::IoError, "cannot read " + path.generic_string());
  return ss.str();
}

Result<void> writeFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return makeError(ErrorCode::IoError, "cannot create " + path.generic_string());
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  f.close();
  if (!f) return makeError(ErrorCode::IoError, "cannot write " + path.generic_string());
  return {};
}

int errorExit(const Error& e) {
  return e.code == ErrorCode::InvalidArgument ? kExitUsage : kExitIoError;
}

}  // namespace

int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Parsed p;
  if (const std::string msg = parseArgs(args, p); !msg.empty()) {
    err << "tachyon-report: " << msg << "\n" << kUsage;
    return kExitUsage;
  }
  if (p.help) {
    out << kUsage;
    return kExitOk;
  }

  Thresholds thresholds;
  if (p.thresholds) {
    auto text = readFile(*p.thresholds);
    if (!text.ok()) {
      err << "tachyon-report: thresholds: " << text.error().message << "\n";
      return kExitIoError;
    }
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
    if (doc.is_discarded()) {
      err << "tachyon-report: thresholds: not valid JSON\n";
      return kExitUsage;
    }
    auto t = thresholdsFromJson(doc);
    if (!t.ok()) {
      err << "tachyon-report: " << t.error().message << "\n";
      return kExitUsage;
    }
    thresholds = *t;
  }

  std::optional<Report> baseline;
  if (p.baseline) {
    auto text = readFile(*p.baseline);
    if (!text.ok()) {
      err << "tachyon-report: baseline: " << text.error().message << "\n";
      return kExitIoError;
    }
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
    if (doc.is_discarded()) {
      err << "tachyon-report: baseline: not valid JSON\n";
      return kExitIoError;
    }
    auto b = reportFromJson(doc);
    if (!b.ok()) {
      err << "tachyon-report: baseline: " << b.error().message << "\n";
      return kExitIoError;
    }
    baseline = std::move(*b);
  }

  AnalyzeOptions ao;
  ao.report.topN = p.top;
  ao.report.frameBudgetMs = p.budgetMs;
  ao.report.frameSeries = p.frameSeries;
  ao.report.blendScopes = !p.noBlend;
  ao.symbolPaths = p.symbols;
  ao.symbolServers = p.servers;
  ao.symbolCacheDir = p.cache;
  auto report = analyzeTrace(p.trace, ao);
  if (!report.ok()) {
    err << "tachyon-report: " << report.error().toString() << "\n";
    return errorExit(report.error());
  }

  std::optional<Diff> diff;
  if (baseline) diff = compareReports(*baseline, *report, thresholds);
  const Diff* diffPtr = diff ? &*diff : nullptr;

  auto render = [&](Format f) {
    return f == Format::Json ? dumpJson(reportToJson(*report, diffPtr)) : renderMarkdown(*report, diffPtr);
  };
  if (p.outs.empty()) {
    out << render(p.format.value_or(Format::Json));
  } else {
    for (const std::filesystem::path& path : p.outs) {
      if (Result<void> w = writeFile(path, render(*formatOfPath(path))); !w.ok()) {
        err << "tachyon-report: " << w.error().message << "\n";
        return kExitIoError;
      }
    }
    out << "tachyon-report: "
        << (diff
                ? (diff->regressed() ? "FAIL (" + std::to_string(diff->regressions.size()) + " regression(s))"
                                     : std::string("PASS"))
                : std::string("report written (no baseline)"))
        << "\n";
  }

  if (report->trace.partial) err << "tachyon-report: warning: the trace is partial\n";
  if (report->trace.eventsLost > 0)
    err << "tachyon-report: warning: " << report->trace.eventsLost << " events were lost\n";
  if (diff && diff->regressed()) return kExitRegression;
  if (p.failOnLost && (report->trace.partial || report->trace.eventsLost > 0)) return kExitLostEvents;
  return kExitOk;
}

}  // namespace tachyon::report
