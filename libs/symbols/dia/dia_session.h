// Backend seam of the DIA symbol provider (WP-22). DiaSymbolProvider (dia_provider.h) holds all
// platform-neutral logic (string interning, function-range cache, locking, error mapping); everything that
// touches COM sits behind these two interfaces, so the provider is unit-tested on Linux with fakes and the
// real msdia140 implementation (dia_com_session.cpp) is Windows-only.
#pragma once

#include "core/result.h"
#include "model/model.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::symbols {

// A function (or public symbol) as the backend reports it. Names are as stored in the PDB: DIA's name for a
// function is already undecorated; a public symbol may still be a decorated "?..." name.
struct DiaRawFunction {
  uint64_t startRva = 0;
  uint64_t size = 0;  // 0 for public symbols without a length
  std::string name;
  bool isPublicSymbol = false;  // found through the public symbol fallback (no lines, no inline info)
};

// One source line covering [rva, rva + size).
struct DiaRawLine {
  uint64_t rva = 0;
  uint32_t size = 0;
  std::string file;
  uint32_t line = 0;
};

// One level of an inline chain, innermost first.
struct DiaRawInline {
  std::string name;
  std::string file;
  uint32_t line = 0;
};

// One open PDB. NOT thread-safe: DiaSymbolProvider serializes every call with its mutex (the documented
// threading policy: one session per provider, guarded by a mutex; parallelism comes from one provider per
// module).
class IDiaSymbolSession {
 public:
  virtual ~IDiaSymbolSession() = default;
  // The function containing rva (private function symbol first, public symbol as fallback).
  virtual std::optional<DiaRawFunction> findFunction(uint64_t rva) = 0;
  // The line-table entry covering rva.
  virtual std::optional<DiaRawLine> findLine(uint64_t rva) = 0;
  // Inline frames active at rva inside `function`, innermost first; empty if none.
  virtual std::vector<DiaRawInline> findInlines(const DiaRawFunction& function, uint64_t rva) = 0;
  // Every line-table entry intersecting [rva, rva + size), in the order the PDB stores them.
  virtual Result<std::vector<DiaRawLine>> findLines(uint64_t rva, uint64_t size) = 0;
};

// Opens PDBs. Thread-safe: open() may be called concurrently (SymbolService loads modules on a worker pool).
class IDiaSessionOpener {
 public:
  virtual ~IDiaSessionOpener() = default;
  // Opens `pdbFile` and validates it against module.id (GUID + age) when the module carries a PDB id. Errors
  // use the mapping of dia_errors.h (NotFound for a missing or mismatching PDB, CorruptData for a damaged
  // one).
  virtual Result<std::unique_ptr<IDiaSymbolSession>> open(const model::Module& module,
                                                          const std::filesystem::path& pdbFile) = 0;
};

// Thread-safe. The msdia140-backed opener, or nullptr when DIA is not compiled in (non-Windows builds,
// TACHYON_ENABLE_DIA=OFF) or msdia140.dll cannot be loaded (the reason goes to *error when given).
std::shared_ptr<IDiaSessionOpener> makeComDiaOpener(std::string* error = nullptr);

}  // namespace tachyon::symbols
