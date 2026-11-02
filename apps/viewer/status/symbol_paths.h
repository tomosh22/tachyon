// Explicit symbol search paths (WP-48): how Settings::symbolSearchPaths reach a SymbolService.
//   * a directory becomes a SymbolLocatorConfig::searchPaths entry (the locator probes it for the symbol file
//     name, the symstore layout and `.build-id/xx/rest.debug`) and is scanned (non-recursively) for
//     `*.symbols.json`, each of which is registered with SymbolService::addSymbolFileOverride;
//   * a `.json` file is registered with addSymbolFileOverride;
//   * any other file becomes a searchPaths entry (the locator uses it when its name matches).
#pragma once

#include "core/result.h"
#include "symbols/symbol_service.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct SymbolPathReport {
  size_t overrides = 0;  // symbol file overrides registered
  size_t searchPaths = 0;
  std::vector<std::string> problems;  // paths that were skipped, with the reason
};

// Main-thread only (configures `service`; call before resolveAllAsync). `servers` and `cacheDir` are the
// settings the service was constructed with: configureLocator() replaces its default locator, so they are
// repeated here. With no search path entry the locator is left alone.
SymbolPathReport applySymbolSearchPaths(symbols::SymbolService& service,
                                        const std::vector<std::string>& paths,
                                        const std::vector<std::string>& servers,
                                        const std::filesystem::path& cacheDir);

// Thread-safe (reads the filesystem). Whether `path` can be used as a symbol search path: it must exist, and
// a `.json` file must be a readable tracegen symbols file.
Result<void> validateSymbolPath(const std::filesystem::path& path);

}  // namespace tachyon::viewer
