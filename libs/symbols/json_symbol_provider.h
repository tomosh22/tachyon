// Provider for tracegen's `.symbols.json` (schema tachyon-symbols-1, see tools/tracegen/README.md).
#pragma once

#include "symbols/range_table.h"
#include "symbols/symbol_provider.h"
#include "symbols/symbol_strings.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::symbols {

// Identity of one module entry in a symbols.json file.
struct JsonModuleIdentity {
  std::string name;
  std::string path;
  ModuleId id;
  uint32_t moduleIdx = kInvalidIdx;
};

// Thread-safe: pure function. True if `entry` describes `module`. When both sides carry an id, the ids (kind,
// bytes, and age for PDB ids) must be equal; otherwise the path (or its filename, case-insensitive) must
// match.
bool moduleMatchesIdentity(const Module& module, const JsonModuleIdentity& entry);

class JsonSymbolProvider final : public ISymbolProvider {
 public:
  // Thread-safe (constructor). `strings` must outlive the provider; all StringIdx values it returns index it.
  explicit JsonSymbolProvider(SymbolStringTable& strings);

  // Thread-safe: reads a file. Lists the module entries of a symbols.json without loading their functions.
  static Result<std::vector<JsonModuleIdentity>> readIdentities(const std::filesystem::path& symbolsJson);

  // Not thread-safe; see ISymbolProvider. Picks the module entry matching `module` (moduleMatchesIdentity).
  Result<void> load(const Module& module, const std::filesystem::path& symbolFile) override;
  // Thread-safe after load().
  ResolvedAddress resolve(uint64_t rva) override;
  // Thread-safe after load().
  Result<std::vector<LineRange>> linesForFunction(uint64_t functionStartRva) override;
  // Thread-safe after load().
  Result<FunctionBounds> functionBounds(uint64_t rva) override;

 private:
  struct InlineRange {
    uint64_t rva = 0;
    uint64_t size = 0;
    std::vector<InlineFrame> chain;  // innermost first
  };
  struct FunctionData {
    StringIdx file;
    uint32_t line = 0;
    std::vector<LineRange> lines;      // sorted by rva
    std::vector<InlineRange> inlines;  // sorted by rva
  };

  SymbolStringTable& m_strings;
  FunctionRangeTable m_table;
  std::vector<FunctionData> m_data;  // parallel to m_table.all()
};

}  // namespace tachyon::symbols
