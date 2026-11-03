#include "report/analyze.h"

#include "analysis/thread_states.h"
#include "model/model.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "symbols/symbol_service.h"
#if defined(TACHYON_HAVE_DIA)
#include "symbols/dia/dia_provider.h"
#endif

#include <system_error>

namespace tachyon::report {

Result<Report> analyzeTrace(const std::filesystem::path& tracePath, const AnalyzeOptions& options) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(tracePath, ec))
    return makeError(ErrorCode::NotFound, "trace file not found: " + tracePath.generic_string());

  symbols::SymbolLocatorConfig locator;
  locator.servers = options.symbolServers;
  locator.cacheDir = options.symbolCacheDir;
  std::vector<std::filesystem::path> overrides;
  for (const std::filesystem::path& p : options.symbolPaths) {
    if (!std::filesystem::exists(p, ec))
      return makeError(ErrorCode::InvalidArgument, "symbol path does not exist: " + p.generic_string());
    if (std::filesystem::is_regular_file(p, ec) && p.extension() == ".json")
      overrides.push_back(p);
    else
      locator.searchPaths.push_back(p);
  }
  if (options.useSidecarSymbols) {
    std::filesystem::path sidecar = tracePath;
    sidecar.replace_extension(".symbols.json");
    if (std::filesystem::is_regular_file(sidecar, ec)) overrides.push_back(std::move(sidecar));
  }

  auto loaded = model::TraceModel::load(tracePath);
  if (!loaded.ok()) return loaded.error();
  model::TraceModel model = std::move(*loaded);
  if (Result<void> s = analysis::buildThreadStates(model); !s.ok()) return s.error();

  symbols::SymbolService service(options.symbolServers, options.symbolCacheDir);
  service.configureLocator(std::move(locator));
#if defined(TACHYON_HAVE_DIA)
  symbols::registerDiaProvider(service);
#endif
  symbols::registerDwarfProvider(service);  // no-op without libdw; .symbols.json overrides keep working
  for (const std::filesystem::path& p : overrides) {
    if (Result<void> s = service.addSymbolFileOverride(p); !s.ok())
      return makeError(ErrorCode::InvalidArgument,
                       "symbol file " + p.filename().generic_string() + ": " + s.error().toString());
  }
  service.resolveAllAsync(model);
  service.waitAll();
  return buildReport(model, service, options.report);
}

}  // namespace tachyon::report
