#include "cli/export_command.h"

#include "analysis/query.h"
#include "analysis/thread_states.h"
#include "cli/cli.h"
#include "export/export.h"
#include "model/model.h"
#include "symbols/dia/dia_provider.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "symbols/symbol_service.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <set>

namespace tachyon::cli {

namespace {

namespace fs = std::filesystem;

enum class Format { Speedscope, Chrome };

struct ExportArgs {
  std::string tracePath, outPath;
  Format format = Format::Speedscope;
  bool haveFormat = false;
  std::vector<uint32_t> tids;
  bool haveFrom = false, haveTo = false;
  double fromMs = 0.0, toMs = 0.0;
  std::vector<std::string> symbolPaths, symbolServers;
};

// Same shape as the other offline commands: a line on stderr and, with --json, one JSON error object.
void fail(bool json, std::ostream& out, std::ostream& err, const char* code, const std::string& message) {
  err << "tachyon-cli: " << message << "\n";
  if (json) {
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = {{"code", code}, {"message", message}};
    out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
  }
}

bool parseMs(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end != s.c_str() + s.size() || !std::isfinite(v) || v < 0.0) return false;
  out = v;
  return true;
}

// Returns an error message (usage error) or an empty string.
std::string parseExportArgs(const std::vector<std::string>& args, ExportArgs& a) {
  std::vector<std::string> positional;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= args.size()) return false;
      out = args[++i];
      return true;
    };
    std::string v;
    if (arg == "--json") {
      // global flag, handled by the caller
    } else if (arg == "--format") {
      if (!value(v)) return "missing value for --format";
      if (v == "speedscope") {
        a.format = Format::Speedscope;
      } else if (v == "chrome") {
        a.format = Format::Chrome;
      } else {
        return "--format must be speedscope or chrome: " + v;
      }
      a.haveFormat = true;
    } else if (arg == "--out") {
      if (!value(a.outPath)) return "missing value for --out";
    } else if (arg == "--threads") {
      if (!value(v)) return "missing value for --threads";
      size_t pos = 0;
      while (pos <= v.size()) {
        size_t comma = v.find(',', pos);
        if (comma == std::string::npos) comma = v.size();
        const std::string item = v.substr(pos, comma - pos);
        uint32_t tid = 0;
        const auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), tid);
        if (item.empty() || ec != std::errc() || ptr != item.data() + item.size())
          return "--threads expects a comma-separated list of thread ids: " + v;
        a.tids.push_back(tid);
        pos = comma + 1;
      }
    } else if (arg == "--from-ms") {
      if (!value(v)) return "missing value for --from-ms";
      if (!parseMs(v, a.fromMs)) return "--from-ms expects a non-negative number of milliseconds: " + v;
      a.haveFrom = true;
    } else if (arg == "--to-ms") {
      if (!value(v)) return "missing value for --to-ms";
      if (!parseMs(v, a.toMs)) return "--to-ms expects a non-negative number of milliseconds: " + v;
      a.haveTo = true;
    } else if (arg == "--symbols") {
      if (!value(v)) return "missing value for --symbols";
      a.symbolPaths.push_back(v);
    } else if (arg == "--symbol-server") {
      if (!value(v)) return "missing value for --symbol-server";
      a.symbolServers.push_back(v);
    } else if (arg.rfind("--", 0) == 0) {
      return "unknown argument: " + arg;
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.size() != 1) return "export expects exactly one TRACE.tachyon";
  a.tracePath = positional[0];
  if (!a.haveFormat) return "export requires --format speedscope|chrome";
  if (a.outPath.empty()) return "export requires --out PATH";
  if (a.haveFrom && a.haveTo && a.toMs <= a.fromMs) return "--to-ms must be greater than --from-ms";
  return {};
}

fs::path defaultSymbolCacheDir() {
  auto env = [](const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
  };
#ifdef _WIN32
  const std::string local = env("LOCALAPPDATA");
  if (!local.empty()) return fs::path(local) / "Tachyon" / "symbols";
#else
  const std::string xdg = env("XDG_CACHE_HOME");
  if (!xdg.empty()) return fs::path(xdg) / "tachyon" / "symbols";
  const std::string home = env("HOME");
  if (!home.empty()) return fs::path(home) / ".cache" / "tachyon" / "symbols";
#endif
  return {};
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Directories become search paths (and contribute their *.symbols.json files as overrides), *.json files are
// overrides, any other file is a search path. Problems are returned as warnings.
std::vector<std::string> applySymbolPaths(symbols::SymbolService& service, const ExportArgs& a) {
  std::vector<std::string> problems;
  symbols::SymbolLocatorConfig config;
  config.servers = a.symbolServers;
  config.cacheDir = defaultSymbolCacheDir();
  auto addOverride = [&](const fs::path& file) {
    const Result<void> r = service.addSymbolFileOverride(file);
    if (!r.ok()) problems.push_back(file.string() + ": " + r.error().toString());
  };
  for (const std::string& p : a.symbolPaths) {
    const fs::path path(p);
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
      config.searchPaths.push_back(path);
      std::vector<fs::path> sidecars;
      for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && endsWith(it->path().filename().string(), ".symbols.json"))
          sidecars.push_back(it->path());
      }
      std::sort(sidecars.begin(), sidecars.end());
      for (const fs::path& s : sidecars) addOverride(s);
    } else if (fs::is_regular_file(path, ec)) {
      if (path.extension() == ".json") {
        addOverride(path);
      } else {
        config.searchPaths.push_back(path);
      }
    } else {
      problems.push_back(p + ": does not exist");
    }
  }
  if (!config.searchPaths.empty() || !config.servers.empty()) service.configureLocator(std::move(config));
  return problems;
}

}  // namespace

int runExport(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err) {
  ExportArgs a;
  if (const std::string e = parseExportArgs(args, a); !e.empty()) {
    fail(json, out, err, "EUSAGE", e);
    err << "usage: tachyon-cli export TRACE.tachyon --format speedscope|chrome --out PATH [--threads "
           "TID,...]\n"
           "         [--from-ms X] [--to-ms Y] [--symbols PATH]... [--symbol-server URL]...\n";
    return kExitUsage;
  }
  std::error_code ec;
  if (!fs::is_regular_file(fs::path(a.tracePath), ec)) {
    fail(json, out, err, "ENOTFOUND", "trace not found: " + a.tracePath);
    return kExitProtocolError;
  }
  auto loaded = model::TraceModel::load(fs::path(a.tracePath));
  if (!loaded.ok()) {
    fail(json, out, err, "EIO", "cannot load " + a.tracePath + ": " + loaded.error().toString());
    return kExitProtocolError;
  }
  model::TraceModel& m = *loaded;
  if (auto st = analysis::buildThreadStates(m); !st.ok()) {
    fail(json, out, err, "EINTERNAL", "thread state analysis failed: " + st.error().toString());
    return kExitProtocolError;
  }

  analysis::Filter filter;
  const uint64_t hz = m.header.clock_frequency_hz;
  if (a.haveFrom || a.haveTo) {
    auto toTicks = [&](double ms) {
      const double ticks = ms * static_cast<double>(hz) / 1000.0;
      return ticks >= 1.8e19 ? UINT64_MAX : static_cast<uint64_t>(ticks + 0.5);
    };
    if (a.haveFrom) filter.t0 = m.header.start_ts + toTicks(a.fromMs);
    if (a.haveTo) filter.t1 = m.header.start_ts + toTicks(a.toMs);
  }
  if (!a.tids.empty()) {
    std::vector<ThreadIdx> threads;
    for (const uint32_t tid : a.tids) {
      const auto it = m.tidToThread.find(tid);
      if (it == m.tidToThread.end()) {
        fail(json, out, err, "EUSAGE", "the trace has no thread with id " + std::to_string(tid));
        return kExitUsage;
      }
      threads.push_back(it->second);
    }
    filter.threads = std::move(threads);
  }

  // Symbols matter for speedscope frame names only.
  symbols::SymbolService service(a.symbolServers, defaultSymbolCacheDir());
  if (a.format == Format::Speedscope) {
#ifdef _WIN32
    symbols::registerDiaProvider(service);
#else
    symbols::registerDwarfProvider(service);
#endif
    for (const std::string& p : applySymbolPaths(service, a)) err << "tachyon-cli: warning: " << p << "\n";
    service.resolveAllAsync(m);
    service.waitAll();
    size_t missing = 0;
    for (uint32_t i = 0; i < m.modules.size(); ++i) {
      const model::SymbolState s = service.moduleState(ModuleIdx(i));
      if (s == model::SymbolState::NotFound || s == model::SymbolState::Failed) ++missing;
    }
    if (missing != 0)
      err << "tachyon-cli: warning: no symbols for " << missing
          << " module(s); their frames are named module!0x<rva>\n";
  }

  const bool toStdout = a.outPath == "-";
  std::ofstream file;
  std::vector<char> fileBuffer;
  if (!toStdout) {
    fileBuffer.resize(size_t{1} << 20);
    file.rdbuf()->pubsetbuf(fileBuffer.data(), static_cast<std::streamsize>(fileBuffer.size()));
    file.open(fs::path(a.outPath), std::ios::binary | std::ios::trunc);
    if (!file) {
      fail(json, out, err, "EIO", "cannot open " + a.outPath + " for writing");
      return kExitProtocolError;
    }
  }
  std::ostream& sink = toStdout ? out : static_cast<std::ostream&>(file);

  Result<exporters::ExportStats> result = exporters::ExportStats{};
  if (a.format == Format::Speedscope) {
    analysis::QueryContext ctx(m, service);
    result = exporters::exportSpeedscope(ctx, filter, sink);
  } else {
    result = exporters::exportChromeTrace(m, service, filter, sink);
  }
  uint64_t bytes = 0;
  if (!toStdout) {
    file.flush();
    const bool good = static_cast<bool>(file);
    file.close();
    if (!good && result.ok())
      result = makeError(ErrorCode::IoError, "writing " + a.outPath + " failed (disk full?)");
    std::error_code sizeEc;
    const auto size = fs::file_size(fs::path(a.outPath), sizeEc);
    if (!sizeEc) bytes = size;
    if (!result.ok()) fs::remove(fs::path(a.outPath), sizeEc);
  }
  if (!result.ok()) {
    fail(json, out, err, result.error().code == ErrorCode::IoError ? "EIO" : "EINTERNAL",
         "export failed: " + result.error().toString());
    return kExitProtocolError;
  }

  const exporters::ExportStats& s = *result;
  const char* formatName = a.format == Format::Speedscope ? "speedscope" : "chrome";
  if (toStdout) return kExitOk;  // stdout carries the export itself
  if (json) {
    nlohmann::json j;
    j["ok"] = true;
    j["format"] = formatName;
    j["output"] = a.outPath;
    j["bytes"] = bytes;
    j["threads"] = s.threads;
    if (a.format == Format::Speedscope) {
      j["samples"] = s.samples;
      j["frames"] = s.frames;
    } else {
      j["events"] = s.events;
    }
    out << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
  } else if (a.format == Format::Speedscope) {
    out << "exported " << s.threads << " thread profile(s), " << s.samples << " samples, " << s.frames
        << " frames (speedscope) to " << a.outPath << "\n";
  } else {
    out << "exported " << s.events << " events of " << s.threads << " thread(s) (chrome trace) to "
        << a.outPath << "\n";
  }
  return kExitOk;
}

}  // namespace tachyon::cli
