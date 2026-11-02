// Address lookup over ELF .symtab / .dynsym code symbols (WP-72): the fallback for code without DWARF.
//
// Rules (mirroring DiaSymbolTable's public-symbol policy, but never unbounded):
//  * RVA = st_value - loadBias (the table is built in RVA space).
//  * A sized symbol covers [rva, rva + st_size).
//  * A symbol with st_size 0 is bounded by the next code symbol and by the end of its section.
//  * Aliases at one address: the strongest binding wins (global, then weak, then local), then the first in
//    table order.
//  * Nested / overlapping symbols resolve to the one with the greatest start that contains the address.
#pragma once

#include "symbols/dwarf/elf_reader.h"
#include "symbols/dwarf/range_index.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tachyon::symbols {

struct ElfTableEntry {
  uint64_t rva = 0;
  uint64_t size = 0;  // always > 0
  std::string name;   // as stored in the ELF (possibly mangled)
};

class ElfSymbolTable {
 public:
  // Not thread-safe (build once). Symbols below `loadBias` are dropped.
  void build(std::vector<ElfSymbol> symbols, uint64_t loadBias);

  // Thread-safe after build (const). The symbol containing rva, or nullptr for gaps.
  const ElfTableEntry* find(uint64_t rva) const;

  size_t size() const { return m_entries.size(); }
  const std::vector<ElfTableEntry>& entries() const { return m_entries; }

 private:
  std::vector<ElfTableEntry> m_entries;  // sorted by rva, one per rva
  RangeIndex m_index;
};

}  // namespace tachyon::symbols
