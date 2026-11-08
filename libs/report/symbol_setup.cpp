#include "report/symbol_setup.h"

#include "symbols/dwarf/dwarf_provider.h"
#if defined(_WIN32) || (defined(TACHYON_HAVE_DIA) && TACHYON_HAVE_DIA)
#include "symbols/dia/dia_provider.h"
#define TACHYON_SETUP_DIA 1
#endif

#include <algorithm>
#include <system_error>

namespace tachyon::report {

namespace {

namespace fs = std::filesystem;

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::vector<SymbolSetupProblem> setupSymbolService(symbols::SymbolService& service,
                                                   const SymbolSetupOptions& options) {
  std::vector<SymbolSetupProblem> problems;
  symbols::SymbolLocatorConfig config;
  config.servers = options.servers;
  config.cacheDir = options.cacheDir;

#if defined(TACHYON_SETUP_DIA)
  symbols::registerDiaProvider(service);
#endif
  symbols::registerDwarfProvider(service);  // no-op without libdw; .symbols.json overrides keep working

  auto addOverride = [&](const fs::path& file) {
    const Result<void> r = service.addSymbolFileOverride(file);
    if (!r.ok()) problems.push_back({SymbolSetupProblem::Kind::BadOverride, file, r.error().toString()});
  };
  std::error_code ec;
  for (const fs::path& path : options.symbolPaths) {
    if (fs::is_directory(path, ec)) {
      config.searchPaths.push_back(path);
      if (!options.directorySidecars) continue;
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
      if (path.extension() == ".json")
        addOverride(path);
      else
        config.searchPaths.push_back(path);
    } else {
      problems.push_back({SymbolSetupProblem::Kind::MissingPath, path, {}});
    }
  }
  if (!options.sidecarFile.empty() && fs::is_regular_file(options.sidecarFile, ec))
    addOverride(options.sidecarFile);
  if (!config.searchPaths.empty() || !config.servers.empty() || !config.cacheDir.empty())
    service.configureLocator(std::move(config));
  return problems;
}

}  // namespace tachyon::report
