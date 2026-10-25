// Address lookup tables of the DIA session (WP-22): private function symbols and code public symbols, with
// correct interval semantics. Platform-neutral so it is unit-tested on Linux; the COM session fills it.
//
// Rules:
//  * Private function symbols (real extents) always win over public symbols.
//  * Private lookup handles nested and overlapping extents (running-max-end array): the function with the
//    greatest start that contains the address is returned.
//  * A public symbol has no length in the PDB. Its extent is bounded by the next code symbol (private or
//    public) and clipped to the end of the code section contribution holding it. When neither bound exists
//    the extent is UNBOUNDED: the address resolves (size 0 reported) for every rva at or after the symbol.
//  * LIMITATION: separated code (hot/cold splitting that places a function's blocks at unrelated addresses)
//    is mapped to the parent function only where the parent's own length covers it; DIA's SymTagSeparatedCode
//    ranges are not enumerated (that would cost a query per function).
#pragma once

#include "symbols/dia/dia_session.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::symbols {

struct DiaTableEntry {
  uint32_t rva = 0;
  uint32_t size = 0;  // private functions: > 0
  std::string name;
};

struct DiaCodeRange {
  uint32_t rva = 0;
  uint32_t size = 0;
};

class DiaSymbolTable {
 public:
  // Not thread-safe (build once). `privates`: function symbols (entries with size 0 are dropped). `publics`:
  // public symbols that are code. `code`: code section contributions, any order, may be empty.
  void build(std::vector<DiaTableEntry> privates, std::vector<DiaTableEntry> publics,
             std::vector<DiaCodeRange> code);

  // Thread-safe after build (const). The function containing rva: private first, public fallback; nullopt for
  // gaps. Public results have isPublicSymbol = true and size 0 when unbounded.
  std::optional<DiaRawFunction> find(uint64_t rva) const;

  size_t privateCount() const { return m_privates.size(); }
  size_t publicCount() const { return m_publics.size(); }

 private:
  std::vector<DiaTableEntry> m_privates;  // sorted by rva (stable)
  std::vector<uint64_t> m_maxEnd;         // m_maxEnd[i] = max end of m_privates[0..i]
  std::vector<DiaTableEntry> m_publics;  // sorted by rva, one per rva; size = bounded extent or 0 (unbounded)
};

}  // namespace tachyon::symbols
