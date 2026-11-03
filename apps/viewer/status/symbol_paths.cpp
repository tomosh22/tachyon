#include "viewer/status/symbol_paths.h"

#include "symbols/json_symbol_provider.h"
#include "viewer/paths.h"

#include <algorithm>
#include <cctype>

namespace tachyon::viewer {

namespace fs = std::filesystem;

namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isJsonFile(const fs::path& p) {
  std::string ext = pathToUtf8(p.extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == ".json";
}

}  // namespace

SymbolPathReport applySymbolSearchPaths(symbols::SymbolService& service,
                                        const std::vector<std::string>& paths,
                                        const std::vector<std::string>& servers, const fs::path& cacheDir,
                                        const SymbolPathOptions& options) {
  SymbolPathReport report;
  size_t droppedProblems = 0;
  auto problem = [&](std::string text) {
    if (report.problems.size() < kMaxSymbolPathProblems) {
      report.problems.push_back(std::move(text));
    } else {
      ++droppedProblems;
    }
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(std::memory_order_relaxed); };
  symbols::SymbolLocatorConfig config;
  config.servers = servers;
  config.cacheDir = cacheDir;
  auto addOverride = [&](const fs::path& file) {
    const Result<void> r = service.addSymbolFileOverride(file);
    if (r.ok()) {
      ++report.overrides;
    } else {
      problem(pathToUtf8(file) + ": " + r.error().toString());
    }
  };
  for (const std::string& utf8 : paths) {
    if (cancelled()) {
      report.cancelled = true;
      break;
    }
    const fs::path path = pathFromUtf8(utf8);
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
      config.searchPaths.push_back(path);
      std::vector<fs::path> sidecars;
      size_t entries = 0;
      bool stopped = false;
      std::error_code listEc;
      fs::directory_iterator it(path, listEc), end;
      if (listEc) {
        problem(utf8 + ": cannot read the directory (" + listEc.message() + ")");
        continue;
      }
      for (; it != end; it.increment(listEc)) {
        if (listEc) break;
        if (entries >= options.maxDirectoryEntries) {
          stopped = true;
          break;
        }
        ++entries;
        if ((entries & 0xFF) == 0 && cancelled()) break;
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && endsWith(pathToUtf8(it->path().filename()), ".symbols.json"))
          sidecars.push_back(it->path());
      }
      if (listEc)
        problem(utf8 + ": listing stopped after " + std::to_string(entries) + " files (" + listEc.message() +
                ")");
      if (stopped) {
        ++report.scanStops;
        problem(utf8 + ": scan stopped at " + std::to_string(entries) + " files (" +
                std::to_string(sidecars.size()) +
                " *.symbols.json found; the rest of the directory was not scanned)");
      }
      std::sort(sidecars.begin(), sidecars.end());  // directory order is unspecified; keep the result stable
      for (const fs::path& s : sidecars) {
        if (cancelled()) break;
        addOverride(s);
      }
    } else if (fs::is_regular_file(path, ec)) {
      if (isJsonFile(path)) {
        addOverride(path);
      } else {
        config.searchPaths.push_back(path);
      }
    } else {
      problem(utf8 + ": does not exist");
    }
  }
  if (cancelled()) report.cancelled = true;
  if (droppedProblems > 0)
    report.problems.push_back("... and " + std::to_string(droppedProblems) + " more problems");
  report.searchPaths = config.searchPaths.size();
  if (!config.searchPaths.empty()) service.configureLocator(std::move(config));
  return report;
}

Result<void> validateSymbolPath(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return makeError(ErrorCode::NotFound, pathToUtf8(path) + " does not exist");
  if (fs::is_directory(path, ec)) {
    std::error_code listEc;
    fs::directory_iterator it(path, listEc);
    if (listEc)
      return makeError(ErrorCode::IoError,
                       pathToUtf8(path) + ": cannot read the directory (" + listEc.message() + ")");
    return ok();
  }
  if (!fs::is_regular_file(path, ec))
    return makeError(ErrorCode::InvalidArgument, pathToUtf8(path) + " is not a file or directory");
  if (isJsonFile(path)) {
    const auto identities = symbols::JsonSymbolProvider::readIdentities(path);
    if (!identities.ok())
      return makeError(identities.error().code, pathToUtf8(path) + ": " + identities.error().toString());
  }
  return ok();
}

}  // namespace tachyon::viewer
