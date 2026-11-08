// Shared symbol-service setup of the offline tools (WP-69): tachyon-report (analyze.cpp) and `tachyon-cli
// export` register the platform providers, split --symbols paths into locator search paths and symbol-file
// overrides, and configure the locator in exactly the same way.
#pragma once

#include "symbols/symbol_service.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::report {

struct SymbolSetupOptions {
  // `.json` files are tracegen/JSON symbol files registered as overrides; directories and other files are
  // symbol search paths (symbol store layout, `.build-id`, next to the module).
  std::vector<std::filesystem::path> symbolPaths;
  std::vector<std::string> servers;
  std::filesystem::path cacheDir;  // empty: the locator's default
  // A directory also contributes the `*.symbols.json` files directly inside it as overrides (sorted by name).
  bool directorySidecars = false;
  // Registered as an override when it is an existing regular file (e.g. "<trace stem>.symbols.json").
  std::filesystem::path sidecarFile;
};

struct SymbolSetupProblem {
  enum class Kind { MissingPath, BadOverride };
  Kind kind = Kind::MissingPath;
  std::filesystem::path path;  // the symbol path, or the override file
  std::string detail;          // BadOverride: the error text of addSymbolFileOverride
};

// Thread-safe for distinct services (not thread-safe on one service; call before resolveAllAsync). Registers
// the platform providers (DIA where compiled in, libdw where compiled in), configures the locator when there
// is anything to configure, and adds the overrides. Problems (missing path, unreadable override) are
// returned in order; the remaining paths are still applied, so a caller may treat them as warnings or fail on
// the first one.
std::vector<SymbolSetupProblem> setupSymbolService(symbols::SymbolService& service,
                                                   const SymbolSetupOptions& options);

}  // namespace tachyon::report
