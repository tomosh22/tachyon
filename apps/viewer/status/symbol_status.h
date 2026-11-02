// Symbol resolution status (WP-48): the numbers behind the status bar's "Symbols: resolved/total (N failed)"
// and the module tables of the status bar tooltip and the Symbols panel. Pure functions on the model and the
// SymbolService (whose moduleState/moduleError/progress are lock-free or thread-safe), so they run on the
// main thread while the workers resolve.
#pragma once

#include "model/model.h"
#include "symbols/symbol_service.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct SymbolSummary {
  uint32_t total = 0;     // modules referenced by at least one frame (SymbolService::progress().total)
  uint32_t done = 0;      // modules that reached a final state
  uint32_t resolved = 0;  // ... of which Resolved
  uint32_t failed = 0;    // ... of which NotFound or Failed
  uint32_t loading = 0;   // currently Loading
  bool running() const { return done < total; }
};

struct ModuleSymbolRow {
  ModuleIdx module;
  std::string name;  // file name part of the module path
  std::string path;  // module path as recorded in the trace
  symbols::SymbolState state = symbols::SymbolState::Pending;
  std::string detail;  // reason for NotFound / Failed (SymbolService::moduleError); empty otherwise
};

// Thread-safe: pure function. One flag per module of `model`: referenced by at least one frame. O(frames);
// the viewer computes it once per document.
std::vector<bool> referencedModules(const model::TraceModel& model);

// Thread-safe: reads the service lock-free. `referenced` as returned by referencedModules().
SymbolSummary summarizeSymbols(const symbols::SymbolService& service, const std::vector<bool>& referenced);

// Thread-safe. The referenced modules in module order, with their current state. With `onlyUnresolved`, the
// ones whose state is not Resolved.
std::vector<ModuleSymbolRow> moduleSymbolRows(const model::TraceModel& model,
                                              const symbols::SymbolService& service,
                                              const std::vector<bool>& referenced, bool onlyUnresolved);

// Thread-safe: pure functions.
const char* symbolStateName(symbols::SymbolState state);
std::string moduleFileName(const std::string& path);
// "name, path, build id / PDB GUID+age" for the clipboard.
std::string moduleInfoText(const model::Module& module, const ModuleSymbolRow& row);

}  // namespace tachyon::viewer
