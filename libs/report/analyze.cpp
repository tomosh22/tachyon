#include "report/analyze.h"

#include "analysis/thread_states.h"
#include "model/model.h"
#include "report/symbol_setup.h"
#include "symbols/symbol_service.h"

#include <system_error>

namespace tachyon::report {

Result<Report> analyzeTrace(const std::filesystem::path& tracePath, const AnalyzeOptions& options) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(tracePath, ec))
    return makeError(ErrorCode::NotFound, "trace file not found: " + tracePath.generic_string());

  SymbolSetupOptions setup;
  setup.servers = options.symbolServers;
  setup.cacheDir = options.symbolCacheDir;
  for (const std::filesystem::path& p : options.symbolPaths) {
    if (!std::filesystem::exists(p, ec))
      return makeError(ErrorCode::InvalidArgument, "symbol path does not exist: " + p.generic_string());
    setup.symbolPaths.push_back(p);
  }
  if (options.useSidecarSymbols) {
    setup.sidecarFile = tracePath;
    setup.sidecarFile.replace_extension(".symbols.json");
  }

  auto loaded = model::TraceModel::load(tracePath);
  if (!loaded.ok()) return loaded.error();
  model::TraceModel model = std::move(*loaded);
  if (Result<void> s = analysis::buildThreadStates(model); !s.ok()) return s.error();

  symbols::SymbolService service(options.symbolServers, options.symbolCacheDir);
  for (const SymbolSetupProblem& p : setupSymbolService(service, setup)) {
    if (p.kind == SymbolSetupProblem::Kind::BadOverride)
      return makeError(ErrorCode::InvalidArgument,
                       "symbol file " + p.path.filename().generic_string() + ": " + p.detail);
  }
  service.resolveAllAsync(model);
  service.waitAll();
  return buildReport(model, service, options.report);
}

}  // namespace tachyon::report
