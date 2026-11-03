// Explicit symbol search paths (WP-48): how Settings::symbolSearchPaths reach a SymbolService.
//   * a directory becomes a SymbolLocatorConfig::searchPaths entry (the locator probes it for the symbol file
//     name, the symstore layout and `.build-id/xx/rest.debug`) and is scanned (non-recursively) for
//     `*.symbols.json`, each of which is registered with SymbolService::addSymbolFileOverride;
//   * a `.json` file is registered with addSymbolFileOverride;
//   * any other file becomes a searchPaths entry (the locator uses it when its name matches).
#pragma once

#include "core/result.h"
#include "symbols/symbol_service.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::viewer {

// WP-67: a directory scan looks at no more than this many directory entries (files of any name).
inline constexpr size_t kMaxSymbolDirectoryEntries = 10000;
// At most this many problems are listed; a final "... and N more" line follows.
inline constexpr size_t kMaxSymbolPathProblems = 50;

struct SymbolPathReport {
  size_t overrides = 0;  // symbol file overrides registered
  size_t searchPaths = 0;
  size_t scanStops = 0;               // directories whose scan hit the entry cap (see `problems`)
  bool cancelled = false;             // WP-67: stopped early through the cancel flag
  std::vector<std::string> problems;  // paths that were skipped, with the reason
};

struct SymbolPathOptions {
  size_t maxDirectoryEntries = kMaxSymbolDirectoryEntries;
  // Polled between files; set from another thread to stop early (the report says `cancelled`).
  const std::atomic<bool>* cancel = nullptr;
};

// Configures `service`; call from one thread only and before resolveAllAsync (WP-67: the viewer calls it on
// a worker thread, because scanning a directory and reading its `*.symbols.json` files can take seconds; the
// service's const lookups may run concurrently, they do not read what this writes). `servers` and `cacheDir`
// are the settings the service was constructed with: configureLocator() replaces its default locator, so
// they are repeated here. With no search path entry the locator is left alone. A directory scan stops after
// options.maxDirectoryEntries entries ("scan stopped at N files" in the report); a directory that cannot be
// read is a problem, not silently accepted.
SymbolPathReport applySymbolSearchPaths(symbols::SymbolService& service,
                                        const std::vector<std::string>& paths,
                                        const std::vector<std::string>& servers,
                                        const std::filesystem::path& cacheDir,
                                        const SymbolPathOptions& options = {});

// Thread-safe (reads the filesystem). Whether `path` can be used as a symbol search path: it must exist, a
// directory must be listable (WP-67), and a `.json` file must be a readable tracegen symbols file.
Result<void> validateSymbolPath(const std::filesystem::path& path);

}  // namespace tachyon::viewer
