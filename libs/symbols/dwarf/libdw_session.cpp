// libdw-backed IDwarfSession (WP-72).
//
// libdw is used for what it is good at (decoding DIEs, ranges, line programs, compressed sections) while the
// lookup structures are our own, built lazily per compilation unit the first time an address falls into it:
//   * a global index of CU address ranges (DW_AT_ranges / low_pc+high_pc of the CU DIE);
//   * per CU: every DW_TAG_subprogram with code and every DW_TAG_inlined_subroutine (with their ranges), as
//     interval indexes that handle nesting; and the flattened line table.
// This avoids dwarf_getscopes() / dwarf_addrdie() per address (each walks DIEs) and keeps resolve() at a few
// binary searches once a CU is indexed.
//
// Everything here is single-threaded: DwarfSymbolProvider holds a mutex around all calls.
#include "symbols/dwarf/dwarf_session.h"
#include "symbols/dwarf/range_index.h"

#include <dwarf.h>
#include <elfutils/libdw.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tachyon::symbols {

namespace {

constexpr int kMaxDieDepth = 200;
constexpr size_t kMaxUnits = 1u << 22;
constexpr size_t kMaxLineRows = 1u << 28;
constexpr size_t kMaxChain = 8;

struct FuncRec {
  uint32_t rangeBegin = 0;
  uint32_t rangeCount = 0;
  uint64_t entry = 0;
  uint64_t dieOffset = 0;
  bool plainTopLevel = false;  // directly under the CU DIE: no enclosing scopes to qualify the name with
};

struct InlRec {
  uint32_t func = 0;
  uint32_t depth = 0;       // 1 = directly inside the physical function
  uint64_t nameOffset = 0;  // DIE that names the inlined function (abstract origin, else the DIE itself)
  uint32_t callFile = 0;
  uint32_t callLine = 0;
};

struct LineRow {
  uint64_t addr = 0;
  uint32_t line = 0;
  uint32_t file = 0;
  bool end = false;
};

struct CuData {
  Dwarf_Die die{};
  bool indexed = false;
  bool filesLoaded = false;   // file table + comp dir (small, kept)
  bool linesFailed = false;   // the line program could not be decoded: do not retry
  bool rowsResident = false;  // `rows` is populated (and listed in the LRU)
  std::list<uint32_t>::iterator lruPos;
  std::vector<DwarfAddrRange> pool;  // ranges of functions and inlines (sorted by lo within one record)
  std::vector<FuncRec> funcs;
  std::vector<InlRec> inls;
  std::vector<std::pair<uint32_t, uint32_t>> inlRanges;  // per InlRec: (begin, count) into pool
  RangeIndex funcIdx;
  RangeIndex inlIdx;
  std::vector<LineRow> rows;
  Dwarf_Files* files = nullptr;
  size_t fileCount = 0;
  std::vector<std::string_view> fileNames;  // resolved lazily, empty = not yet
  std::string compDir;
};

bool attrString(Dwarf_Die* die, unsigned name, const char*& out) {
  Dwarf_Attribute a;
  if (dwarf_attr(die, name, &a) == nullptr) return false;
  out = dwarf_formstring(&a);
  return out != nullptr;
}

bool attrUnsigned(Dwarf_Die* die, unsigned name, uint64_t& out) {
  Dwarf_Attribute a;
  if (dwarf_attr(die, name, &a) == nullptr) return false;
  Dwarf_Word w = 0;
  if (dwarf_formudata(&a, &w) != 0) return false;
  out = w;
  return true;
}

// Follows DW_AT_abstract_origin / DW_AT_specification; false at the end of the chain.
bool followOrigin(Dwarf_Die* die, Dwarf_Die* out) {
  for (const unsigned at :
       {static_cast<unsigned>(DW_AT_abstract_origin), static_cast<unsigned>(DW_AT_specification)}) {
    Dwarf_Attribute a;
    if (dwarf_attr(die, at, &a) != nullptr && dwarf_formref_die(&a, out) != nullptr) return true;
  }
  return false;
}

// Code ranges of a DIE in DWARF order; empty-or-garbage ranges (start 0, end <= start) are dropped.
std::vector<DwarfAddrRange> dieRanges(Dwarf_Die* die) {
  std::vector<DwarfAddrRange> out;
  Dwarf_Addr base = 0, start = 0, end = 0;
  ptrdiff_t off = 0;
  size_t guard = 0;
  while ((off = dwarf_ranges(die, off, &base, &start, &end)) > 0) {
    if (++guard > (1u << 24)) break;
    if (start == 0 || end <= start) continue;
    out.push_back(DwarfAddrRange{start, end});
  }
  return out;
}

class LibdwSession final : public IDwarfSession {
 public:
  LibdwSession(int fd, Dwarf* dw, uint64_t lineCacheBytes)
      : m_fd(fd), m_dw(dw), m_lineBudget(lineCacheBytes) {}

  DwarfSessionStats stats() const override { return m_stats; }

  ~LibdwSession() override {
    if (m_dw != nullptr) dwarf_end(m_dw);
    if (m_fd >= 0) ::close(m_fd);
  }
  LibdwSession(const LibdwSession&) = delete;
  LibdwSession& operator=(const LibdwSession&) = delete;

  std::optional<DwarfFunction> findFunction(uint64_t addr) override {
    ensureUnits();
    std::optional<DwarfFunction> result;
    m_cuIdx.forEachContaining(addr, [&](const IndexedRange& cr) {
      CuData& cu = m_cus[cr.payload];
      ensureIndexed(cu);
      cu.funcIdx.forEachContaining(addr, [&](const IndexedRange& fr) {
        const FuncRec& f = cu.funcs[fr.payload];
        DwarfFunction out;
        out.id = (static_cast<uint64_t>(cr.payload) << 32) | fr.payload;
        out.entry = f.entry;
        out.piece = DwarfAddrRange{fr.lo, fr.hi};
        out.nameKey = f.dieOffset;
        result = out;
        return true;
      });
      return result.has_value();
    });
    return result;
  }

  std::string_view functionName(uint64_t id) override {
    CuData* cu = nullptr;
    const FuncRec* f = funcFor(id, cu);
    if (f == nullptr) return {};
    return nameOf(f->dieOffset, f->plainTopLevel);
  }

  std::string_view sourceName(uint64_t nameKey) override {
    auto it = m_sourceNames.find(nameKey);
    if (it != m_sourceNames.end()) return it->second;
    std::string_view result;
    Dwarf_Die die;
    if (dwarf_offdie(m_dw, nameKey, &die) != nullptr) result = pool(computeName(&die, false, true));
    m_sourceNames.emplace(nameKey, result);
    return result;
  }

  std::vector<DwarfAddrRange> functionRanges(uint64_t id) override {
    CuData* cu = nullptr;
    const FuncRec* f = funcFor(id, cu);
    if (f == nullptr) return {};
    return std::vector<DwarfAddrRange>(cu->pool.begin() + f->rangeBegin,
                                       cu->pool.begin() + f->rangeBegin + f->rangeCount);
  }

  std::vector<DwarfInlineSite> findInlines(uint64_t id, uint64_t addr) override {
    std::vector<DwarfInlineSite> out;
    CuData* cu = nullptr;
    if (funcFor(id, cu) == nullptr) return out;
    const uint32_t funcIdx = static_cast<uint32_t>(id & 0xFFFFFFFFu);
    std::vector<uint32_t> nodes;
    cu->inlIdx.forEachContaining(addr, [&](const IndexedRange& r) {
      if (cu->inls[r.payload].func == funcIdx &&
          std::find(nodes.begin(), nodes.end(), r.payload) == nodes.end()) {
        nodes.push_back(r.payload);
      }
      return false;
    });
    std::stable_sort(nodes.begin(), nodes.end(),
                     [&](uint32_t a, uint32_t b) { return cu->inls[a].depth > cu->inls[b].depth; });
    out.reserve(nodes.size());
    for (const uint32_t n : nodes) {
      const InlRec& rec = cu->inls[n];
      DwarfInlineSite site;
      site.name = nameOf(rec.nameOffset, false);
      site.nameKey = rec.nameOffset;
      site.callFile = fileName(*cu, rec.callFile);
      site.callLine = rec.callLine;
      out.push_back(site);
    }
    return out;
  }

  std::optional<DwarfLineRow> findLine(uint64_t addr) override {
    ensureUnits();
    std::optional<DwarfLineRow> result;
    m_cuIdx.forEachContaining(addr, [&](const IndexedRange& cr) {
      CuData& cu = m_cus[cr.payload];
      ensureLines(cu);
      const size_t i = rowAtOrBefore(cu, addr);
      if (i == kNoRow || cu.rows[i].end || i + 1 >= cu.rows.size()) return false;
      const LineRow& r = cu.rows[i];
      DwarfLineRow out;
      out.lo = r.addr;
      out.hi = cu.rows[i + 1].addr;
      out.file = fileName(cu, r.file);
      out.line = r.line;
      if (out.hi <= addr) return false;
      result = out;
      return true;
    });
    return result;
  }

  Result<std::vector<DwarfLineRow>> findLines(uint64_t lo, uint64_t hi) override {
    ensureUnits();
    std::vector<DwarfLineRow> out;
    if (hi <= lo) return out;
    m_cuIdx.forEachContaining(lo, [&](const IndexedRange& cr) {
      CuData& cu = m_cus[cr.payload];
      ensureLines(cu);
      size_t i = rowAtOrBefore(cu, lo);
      if (i == kNoRow) i = 0;
      bool any = false;
      for (; i + 1 < cu.rows.size() && cu.rows[i].addr < hi; ++i) {
        const LineRow& r = cu.rows[i];
        if (r.end) continue;
        const uint64_t rowLo = std::max(r.addr, lo);
        const uint64_t rowHi = std::min(cu.rows[i + 1].addr, hi);
        if (rowHi <= rowLo) continue;
        out.push_back(DwarfLineRow{rowLo, rowHi, fileName(cu, r.file), r.line});
        any = true;
      }
      return any;
    });
    return out;
  }

 private:
  static constexpr size_t kNoRow = static_cast<size_t>(-1);

  // --- compilation units
  // ---------------------------------------------------------------------------------------

  void ensureUnits() {
    if (m_unitsBuilt) return;
    m_unitsBuilt = true;
    std::vector<IndexedRange> ranges;
    Dwarf_CU* cu = nullptr;
    Dwarf_CU* next = nullptr;
    while (m_cus.size() < kMaxUnits) {
      Dwarf_Half version = 0;
      uint8_t unitType = 0;
      Dwarf_Die cuDie{}, subDie{};
      if (dwarf_get_units(m_dw, cu, &next, &version, &unitType, &cuDie, &subDie) != 0) break;
      cu = next;
      if (unitType == DW_UT_type || unitType == DW_UT_split_type) continue;
      const int tag = dwarf_tag(&cuDie);
      if (tag != DW_TAG_compile_unit && tag != DW_TAG_partial_unit) continue;
      std::vector<DwarfAddrRange> cuRanges = dieRanges(&cuDie);
      if (cuRanges.empty()) continue;
      const uint32_t idx = static_cast<uint32_t>(m_cus.size());
      m_cus.emplace_back();
      m_cus.back().die = cuDie;
      for (const DwarfAddrRange& r : cuRanges) ranges.push_back(IndexedRange{r.lo, r.hi, idx});
    }
    m_cuIdx.build(std::move(ranges));
  }

  void ensureIndexed(CuData& cu) {
    if (cu.indexed) return;
    cu.indexed = true;
    walk(cu, &cu.die, -1, -1, 0, true);
    std::vector<IndexedRange> funcRanges;
    for (uint32_t i = 0; i < cu.funcs.size(); ++i) {
      const FuncRec& f = cu.funcs[i];
      for (uint32_t k = 0; k < f.rangeCount; ++k) {
        const DwarfAddrRange& r = cu.pool[f.rangeBegin + k];
        funcRanges.push_back(IndexedRange{r.lo, r.hi, i});
      }
    }
    cu.funcIdx.build(std::move(funcRanges));
    std::vector<IndexedRange> inlRanges;
    for (uint32_t i = 0; i < cu.inls.size(); ++i) {
      for (uint32_t k = 0; k < cu.inlRanges[i].second; ++k) {
        const DwarfAddrRange& r = cu.pool[cu.inlRanges[i].first + k];
        inlRanges.push_back(IndexedRange{r.lo, r.hi, i});
      }
    }
    cu.inlIdx.build(std::move(inlRanges));
  }

  // Walks the children of `parent`. `func` is the enclosing physical function (-1 at CU / namespace level),
  // `inl` the enclosing inlined call (-1 directly in a function). `plain`: no scope DIE between the CU and
  // here.
  void walk(CuData& cu, Dwarf_Die* parent, int64_t func, int64_t inl, int level, bool plain) {
    if (level > kMaxDieDepth) return;
    Dwarf_Die child;
    if (dwarf_child(parent, &child) != 0) return;
    do {
      switch (dwarf_tag(&child)) {
        case DW_TAG_subprogram: handleSubprogram(cu, &child, level, plain); break;
        case DW_TAG_inlined_subroutine:
          if (func >= 0) handleInline(cu, &child, func, inl, level);
          break;
        case DW_TAG_lexical_block:
        case DW_TAG_try_block:
        case DW_TAG_catch_block: walk(cu, &child, func, inl, level + 1, plain); break;
        case DW_TAG_namespace:
          if (func < 0) walk(cu, &child, -1, -1, level + 1, false);
          break;
        default: break;
      }
    } while (dwarf_siblingof(&child, &child) == 0);
  }

  void handleSubprogram(CuData& cu, Dwarf_Die* die, int level, bool plain) {
    if (dwarf_hasattr(die, DW_AT_declaration)) return;
    std::vector<DwarfAddrRange> ranges = dieRanges(die);
    if (ranges.empty()) return;  // abstract instance or removed by the linker
    FuncRec f;
    f.dieOffset = dwarf_dieoffset(die);
    f.plainTopLevel = plain;
    f.entry = ranges.front().lo;  // first range in DWARF order: compilers list the hot part first
    Dwarf_Attribute a;
    Dwarf_Addr entryPc = 0;
    if (dwarf_attr(die, DW_AT_entry_pc, &a) != nullptr && dwarf_formaddr(&a, &entryPc) == 0) {
      for (const DwarfAddrRange& r : ranges) {
        if (entryPc >= r.lo && entryPc < r.hi) f.entry = entryPc;
      }
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const DwarfAddrRange& x, const DwarfAddrRange& y) { return x.lo < y.lo; });
    f.rangeBegin = static_cast<uint32_t>(cu.pool.size());
    f.rangeCount = static_cast<uint32_t>(ranges.size());
    cu.pool.insert(cu.pool.end(), ranges.begin(), ranges.end());
    const int64_t idx = static_cast<int64_t>(cu.funcs.size());
    cu.funcs.push_back(f);
    walk(cu, die, idx, -1, level + 1, false);
  }

  void handleInline(CuData& cu, Dwarf_Die* die, int64_t func, int64_t parentInl, int level) {
    std::vector<DwarfAddrRange> ranges = dieRanges(die);
    if (ranges.empty()) return;
    InlRec rec;
    rec.func = static_cast<uint32_t>(func);
    rec.depth = parentInl >= 0 ? cu.inls[static_cast<size_t>(parentInl)].depth + 1 : 1;
    rec.nameOffset = dwarf_dieoffset(die);
    Dwarf_Attribute a;
    Dwarf_Die origin;
    if (dwarf_attr(die, DW_AT_abstract_origin, &a) != nullptr && dwarf_formref_die(&a, &origin) != nullptr) {
      rec.nameOffset = dwarf_dieoffset(&origin);
    }
    uint64_t v = 0;
    if (attrUnsigned(die, DW_AT_call_file, v)) rec.callFile = static_cast<uint32_t>(v);
    if (attrUnsigned(die, DW_AT_call_line, v)) rec.callLine = static_cast<uint32_t>(v);
    const int64_t idx = static_cast<int64_t>(cu.inls.size());
    cu.inls.push_back(rec);
    cu.inlRanges.emplace_back(static_cast<uint32_t>(cu.pool.size()), static_cast<uint32_t>(ranges.size()));
    cu.pool.insert(cu.pool.end(), ranges.begin(), ranges.end());
    walk(cu, die, func, idx, level + 1, false);
  }

  const FuncRec* funcFor(uint64_t id, CuData*& cuOut) {
    const uint64_t cuIdx = id >> 32;
    const uint64_t fIdx = id & 0xFFFFFFFFu;
    if (cuIdx >= m_cus.size()) return nullptr;
    CuData& cu = m_cus[cuIdx];
    if (!cu.indexed || fIdx >= cu.funcs.size()) return nullptr;
    cuOut = &cu;
    return &cu.funcs[fIdx];
  }

  // --- names
  // ---------------------------------------------------------------------------------------------------

  std::string_view pool(std::string_view s) {
    if (s.empty()) return {};
    return *m_pool.emplace(s).first;
  }

  std::string_view nameOf(uint64_t dieOffset, bool plain) {
    auto it = m_names.find(dieOffset);
    if (it != m_names.end()) return it->second;
    std::string_view result;
    Dwarf_Die die;
    if (dwarf_offdie(m_dw, dieOffset, &die) != nullptr) result = pool(computeName(&die, plain));
    m_names.emplace(dieOffset, result);
    return result;
  }

  static std::string scopeComponent(Dwarf_Die* scope) {
    switch (dwarf_tag(scope)) {
      case DW_TAG_namespace: {
        const char* n = dwarf_diename(scope);
        return n != nullptr ? n : "(anonymous namespace)";
      }
      case DW_TAG_structure_type:
      case DW_TAG_class_type:
      case DW_TAG_union_type:
      case DW_TAG_subprogram: {
        const char* n = dwarf_diename(scope);
        return n != nullptr ? n : "";
      }
      default: return {};
    }
  }

  // The linkage name when any DIE of the origin chain has one (the provider demangles it); else the source
  // name qualified with its enclosing namespaces / classes.
  std::string computeName(Dwarf_Die* die, bool plain, bool sourceOnly = false) {
    Dwarf_Die chain[kMaxChain];
    size_t n = 1;
    chain[0] = *die;
    while (n < kMaxChain && followOrigin(&chain[n - 1], &chain[n])) ++n;

    for (size_t i = 0; i < n && !sourceOnly; ++i) {
      const char* s = nullptr;
      if (attrString(&chain[i], DW_AT_linkage_name, s) || attrString(&chain[i], DW_AT_MIPS_linkage_name, s)) {
        return s;
      }
    }
    size_t named = n;
    const char* base = nullptr;
    for (size_t i = 0; i < n; ++i) {
      if (attrString(&chain[i], DW_AT_name, base)) {
        named = i;
        break;
      }
    }
    if (named == n) return {};
    std::string out = base;
    if (named == 0 && plain) return out;

    return qualifiedPrefix(&chain[named]) + out;
  }

  // Records, for every scope-like DIE below `parent` (namespaces, classes, unions, functions), its enclosing
  // scope: one walk per CU instead of one dwarf_getscopes_die() (a walk each) per name.
  void indexScopes(Dwarf_Die* parent, uint64_t parentOff, int level) {
    if (level > kMaxDieDepth) return;
    Dwarf_Die child;
    if (dwarf_child(parent, &child) != 0) return;
    do {
      const int tag = dwarf_tag(&child);
      switch (tag) {
        case DW_TAG_namespace:
        case DW_TAG_structure_type:
        case DW_TAG_class_type:
        case DW_TAG_union_type:
        case DW_TAG_subprogram: {
          const uint64_t off = dwarf_dieoffset(&child);
          m_scopeParent.emplace(off, parentOff);
          indexScopes(&child, off, level + 1);
          break;
        }
        case DW_TAG_lexical_block: indexScopes(&child, parentOff, level + 1); break;
        default: break;
      }
    } while (dwarf_siblingof(&child, &child) == 0);
  }

  // "ns::Cls::" for the DIE `die` (empty at namespace scope of the CU).
  std::string qualifiedPrefix(Dwarf_Die* die) {
    Dwarf_Die cuDie;
    if (dwarf_diecu(die, &cuDie, nullptr, nullptr) == nullptr) return {};
    const uint64_t cuOff = dwarf_dieoffset(&cuDie);
    if (m_scopedCus.insert(cuOff).second) indexScopes(&cuDie, 0, 0);
    std::vector<std::string> parts;
    uint64_t off = dwarf_dieoffset(die);
    for (int guard = 0; guard < 256; ++guard) {
      auto it = m_scopeParent.find(off);
      if (it == m_scopeParent.end() || it->second == 0) break;
      off = it->second;
      auto cached = m_scopeNames.find(off);
      if (cached == m_scopeNames.end()) {
        Dwarf_Die scope;
        std::string c;
        if (dwarf_offdie(m_dw, off, &scope) != nullptr) c = scopeComponent(&scope);
        cached = m_scopeNames.emplace(off, std::move(c)).first;
      }
      if (!cached->second.empty()) parts.push_back(cached->second);
    }
    std::string prefix;
    for (size_t i = parts.size(); i > 0; --i) prefix += parts[i - 1] + "::";
    return prefix;
  }

  // --- lines
  // ---------------------------------------------------------------------------------------------------

  // File table and compilation directory of a CU (small; never evicted).
  void ensureFiles(CuData& cu) {
    if (cu.filesLoaded) return;
    cu.filesLoaded = true;
    const char* dir = nullptr;
    if (attrString(&cu.die, DW_AT_comp_dir, dir)) cu.compDir = dir;
    if (dwarf_getsrcfiles(&cu.die, &cu.files, &cu.fileCount) != 0) {
      cu.files = nullptr;
      cu.fileCount = 0;
    }
    cu.fileNames.assign(cu.fileCount, std::string_view());
  }

  void dropRows(CuData& cu) {
    m_stats.lineBytes -= cu.rows.capacity() * sizeof(LineRow);
    std::vector<LineRow>().swap(cu.rows);
    m_lru.erase(cu.lruPos);
    cu.rowsResident = false;
    ++m_stats.lineEvictions;
  }

  // Makes the flattened line rows of `cu` resident, most recently used first in the LRU, and evicts older CUs
  // while the byte budget is exceeded (never `cu` itself).
  void ensureLines(CuData& cu) {
    ensureFiles(cu);
    const uint32_t self = static_cast<uint32_t>(&cu - m_cus.data());
    if (cu.rowsResident) {
      m_lru.splice(m_lru.begin(), m_lru, cu.lruPos);
      return;
    }
    if (cu.linesFailed) return;
    decodeRows(cu);
    if (cu.rows.empty()) {
      cu.linesFailed = true;
      return;
    }
    cu.rowsResident = true;
    m_lru.push_front(self);
    cu.lruPos = m_lru.begin();
    m_stats.lineBytes += cu.rows.capacity() * sizeof(LineRow);
    ++m_stats.lineLoads;
    while (m_stats.lineBytes > m_lineBudget && m_lru.size() > 1) dropRows(m_cus[m_lru.back()]);
  }

  void decodeRows(CuData& cu) {
    Dwarf_Lines* lines = nullptr;
    size_t n = 0;
    if (dwarf_getsrclines(&cu.die, &lines, &n) != 0 || lines == nullptr) return;
    n = std::min(n, kMaxLineRows);
    cu.rows.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      Dwarf_Line* l = dwarf_onesrcline(lines, i);
      if (l == nullptr) continue;
      LineRow row;
      Dwarf_Addr addr = 0;
      int lineno = 0;
      bool end = false;
      if (dwarf_lineaddr(l, &addr) != 0) continue;
      if (dwarf_lineno(l, &lineno) != 0) lineno = 0;
      if (dwarf_lineendsequence(l, &end) != 0) end = false;
      Dwarf_Files* lf = nullptr;
      size_t fidx = 0;
      if (dwarf_line_file(l, &lf, &fidx) != 0) fidx = 0;
      row.addr = addr;
      row.line = lineno > 0 ? static_cast<uint32_t>(lineno) : 0;
      row.file = static_cast<uint32_t>(fidx);
      row.end = end;
      cu.rows.push_back(row);
    }
    // Sequences are individually ordered; merge them. At equal addresses an end-of-sequence row sorts first,
    // so a sequence that ends where another starts does not hide the start.
    std::stable_sort(cu.rows.begin(), cu.rows.end(), [](const LineRow& a, const LineRow& b) {
      if (a.addr != b.addr) return a.addr < b.addr;
      return a.end && !b.end;
    });
    cu.rows.shrink_to_fit();
  }

  static size_t rowAtOrBefore(const CuData& cu, uint64_t addr) {
    auto it = std::upper_bound(cu.rows.begin(), cu.rows.end(), addr,
                               [](uint64_t v, const LineRow& r) { return v < r.addr; });
    if (it == cu.rows.begin()) return kNoRow;
    return static_cast<size_t>(it - cu.rows.begin()) - 1;
  }

  // File path of a line-table / call-site file index: absolute, joined with the CU's DW_AT_comp_dir when
  // libdw returns a relative one.
  std::string_view fileName(CuData& cu, uint32_t idx) {
    ensureFiles(cu);
    if (cu.files == nullptr || idx >= cu.fileCount) return {};
    std::string_view& slot = cu.fileNames[idx];
    if (!slot.empty()) return slot;
    const char* name = dwarf_filesrc(cu.files, idx, nullptr, nullptr);
    if (name == nullptr || *name == '\0') return {};
    std::string path = name;
    if (path[0] != '/' && !cu.compDir.empty()) {
      path = (std::filesystem::path(cu.compDir) / path).lexically_normal().string();
    }
    slot = pool(path);
    return slot;
  }

  int m_fd;
  Dwarf* m_dw;
  bool m_unitsBuilt = false;
  std::vector<CuData> m_cus;
  RangeIndex m_cuIdx;
  std::unordered_set<std::string> m_pool;  // node-based: string_views into it stay valid
  std::unordered_map<uint64_t, std::string_view> m_names;
  std::unordered_map<uint64_t, std::string_view> m_sourceNames;
  std::unordered_map<uint64_t, uint64_t> m_scopeParent;  // scope-like DIE offset -> enclosing scope (0 = CU)
  std::unordered_set<uint64_t> m_scopedCus;              // CU DIE offsets whose scopes are indexed
  std::unordered_map<uint64_t, std::string> m_scopeNames;
  uint64_t m_lineBudget;
  std::list<uint32_t> m_lru;  // CU indices with resident rows, most recently used first
  DwarfSessionStats m_stats;
};

class LibdwOpener final : public IDwarfOpener {
 public:
  explicit LibdwOpener(uint64_t lineCacheBytes) : m_lineCacheBytes(lineCacheBytes) {}
  Result<std::unique_ptr<IDwarfSession>> open(const ElfFile& elf) override {
    const int fd = ::open(elf.path().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return makeError(ErrorCode::IoError, elf.path().string() + ": cannot open");
    Dwarf* dw = dwarf_begin(fd, DWARF_C_READ);
    if (dw == nullptr) {
      const char* msg = dwarf_errmsg(-1);
      ::close(fd);
      return makeError(ErrorCode::CorruptData, elf.path().string() + ": libdw cannot read the debug info" +
                                                   (msg != nullptr ? std::string(": ") + msg : ""));
    }
    return std::unique_ptr<IDwarfSession>(new LibdwSession(fd, dw, m_lineCacheBytes));
  }

 private:
  uint64_t m_lineCacheBytes;
};

}  // namespace

std::shared_ptr<IDwarfOpener> makeLibdwOpener(uint64_t lineCacheBytes) {
  return std::make_shared<LibdwOpener>(lineCacheBytes);
}

}  // namespace tachyon::symbols
