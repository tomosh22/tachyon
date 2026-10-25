#include "symbols/dia/dia_symbol_table.h"

#include <algorithm>
#include <utility>

namespace tachyon::symbols {

void DiaSymbolTable::build(std::vector<DiaTableEntry> privates, std::vector<DiaTableEntry> publics,
                           std::vector<DiaCodeRange> code) {
  privates.erase(
      std::remove_if(privates.begin(), privates.end(), [](const DiaTableEntry& e) { return e.size == 0; }),
      privates.end());
  const auto byRva = [](const DiaTableEntry& a, const DiaTableEntry& b) { return a.rva < b.rva; };
  std::stable_sort(privates.begin(), privates.end(), byRva);
  std::stable_sort(publics.begin(), publics.end(), byRva);
  std::sort(code.begin(), code.end(),
            [](const DiaCodeRange& a, const DiaCodeRange& b) { return a.rva < b.rva; });

  // One public per rva (aliases: keep the first).
  publics.erase(std::unique(publics.begin(), publics.end(),
                            [](const DiaTableEntry& a, const DiaTableEntry& b) { return a.rva == b.rva; }),
                publics.end());

  // Every code symbol start, ascending, for "next code symbol" bounds.
  std::vector<uint32_t> starts;
  starts.reserve(privates.size() + publics.size());
  for (const DiaTableEntry& e : privates) starts.push_back(e.rva);
  for (const DiaTableEntry& e : publics) starts.push_back(e.rva);
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

  for (DiaTableEntry& pub : publics) {
    uint64_t end = 0;  // 0 = no bound
    auto next = std::upper_bound(starts.begin(), starts.end(), pub.rva);
    if (next != starts.end()) end = *next;
    // Clip to the end of the code contribution containing the symbol.
    auto c = std::upper_bound(code.begin(), code.end(), pub.rva,
                              [](uint32_t v, const DiaCodeRange& r) { return v < r.rva; });
    if (c != code.begin()) {
      --c;
      const uint64_t cEnd = static_cast<uint64_t>(c->rva) + c->size;
      if (pub.rva < cEnd) end = end == 0 ? cEnd : std::min(end, cEnd);
    }
    pub.size = end > pub.rva ? static_cast<uint32_t>(end - pub.rva) : 0;
  }

  m_maxEnd.clear();
  m_maxEnd.reserve(privates.size());
  uint64_t running = 0;
  for (const DiaTableEntry& e : privates) {
    running = std::max<uint64_t>(running, static_cast<uint64_t>(e.rva) + e.size);
    m_maxEnd.push_back(running);
  }
  m_privates = std::move(privates);
  m_publics = std::move(publics);
}

std::optional<DiaRawFunction> DiaSymbolTable::find(uint64_t rva) const {
  const auto toRaw = [](const DiaTableEntry& e, bool isPublic) {
    DiaRawFunction out;
    out.startRva = e.rva;
    out.size = e.size;
    out.name = e.name;
    out.isPublicSymbol = isPublic;
    return out;
  };

  // Privates: scan back from the last entry starting at or before rva while some earlier entry may still
  // reach rva; the first entry that contains it has the greatest start (the innermost of nested symbols).
  auto it = std::upper_bound(m_privates.begin(), m_privates.end(), rva,
                             [](uint64_t v, const DiaTableEntry& e) { return v < e.rva; });
  for (size_t i = static_cast<size_t>(it - m_privates.begin()); i > 0; --i) {
    const size_t j = i - 1;
    if (m_maxEnd[j] <= rva) break;
    const DiaTableEntry& e = m_privates[j];
    if (rva - e.rva < e.size) return toRaw(e, false);
  }

  auto p = std::upper_bound(m_publics.begin(), m_publics.end(), rva,
                            [](uint64_t v, const DiaTableEntry& e) { return v < e.rva; });
  if (p == m_publics.begin()) return std::nullopt;
  --p;
  if (p->size != 0 && rva - p->rva >= p->size) return std::nullopt;
  return toRaw(*p, true);
}

}  // namespace tachyon::symbols
