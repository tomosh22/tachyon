#include "symbols/dwarf/elf_symbol_table.h"

#include <algorithm>
#include <utility>

namespace tachyon::symbols {

namespace {

int bindingRank(uint8_t binding) {
  switch (binding) {
    case 1: return 0;  // global
    case 2: return 1;  // weak
    default: return 2;
  }
}

struct Pending {
  uint64_t rva = 0;
  uint64_t size = 0;
  uint64_t sectionEndRva = 0;
  int rank = 0;
  size_t order = 0;
  std::string name;
};

}  // namespace

void ElfSymbolTable::build(std::vector<ElfSymbol> symbols, uint64_t loadBias) {
  std::vector<Pending> pending;
  pending.reserve(symbols.size());
  for (size_t i = 0; i < symbols.size(); ++i) {
    ElfSymbol& s = symbols[i];
    if (s.value < loadBias) continue;
    Pending p;
    p.rva = s.value - loadBias;
    p.size = s.size;
    p.sectionEndRva = s.sectionEnd > loadBias ? s.sectionEnd - loadBias : 0;
    p.rank = bindingRank(s.binding);
    p.order = i;
    p.name = std::move(s.name);
    pending.push_back(std::move(p));
  }
  std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
    if (a.rva != b.rva) return a.rva < b.rva;
    if (a.rank != b.rank) return a.rank < b.rank;
    return a.order < b.order;
  });
  pending.erase(std::unique(pending.begin(), pending.end(),
                            [](const Pending& a, const Pending& b) { return a.rva == b.rva; }),
                pending.end());

  m_entries.clear();
  m_entries.reserve(pending.size());
  std::vector<IndexedRange> ranges;
  ranges.reserve(pending.size());
  for (size_t i = 0; i < pending.size(); ++i) {
    Pending& p = pending[i];
    uint64_t size = p.size;
    if (size == 0) {
      uint64_t end = p.sectionEndRva;  // 0 = unknown
      if (i + 1 < pending.size()) end = end == 0 ? pending[i + 1].rva : std::min(end, pending[i + 1].rva);
      size = end > p.rva ? end - p.rva : 0;
    }
    if (size == 0 || p.rva + size < p.rva) continue;  // empty or wrapping
    ranges.push_back(IndexedRange{p.rva, p.rva + size, static_cast<uint32_t>(m_entries.size())});
    m_entries.push_back(ElfTableEntry{p.rva, size, std::move(p.name)});
  }
  m_index.build(std::move(ranges));
}

const ElfTableEntry* ElfSymbolTable::find(uint64_t rva) const {
  const IndexedRange* r = m_index.find(rva);
  return r == nullptr ? nullptr : &m_entries[r->payload];
}

}  // namespace tachyon::symbols
