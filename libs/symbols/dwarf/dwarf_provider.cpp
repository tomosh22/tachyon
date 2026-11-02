#include "symbols/dwarf/dwarf_provider.h"

#if TACHYON_HAVE_LIBDW

#include "symbols/demangle.h"
#include "symbols/json_symbol_provider.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace tachyon::symbols {

namespace {

bool hasExtension(const std::filesystem::path& p, const char* ext) {
  return p.extension().string() == ext;
}

bool hasElfMagic(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  char magic[4] = {};
  in.read(magic, 4);
  return in.gcount() == 4 && std::memcmp(magic,
                                         "\x7f"
                                         "ELF",
                                         4) == 0;
}

std::string hexName(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

struct Position {
  std::string_view file;
  uint32_t line = 0;
  bool valid() const { return line != 0 && !file.empty(); }
};

}  // namespace

DwarfSymbolProvider::DwarfSymbolProvider(SymbolStringTable& strings, std::shared_ptr<IDwarfOpener> opener,
                                         NameFn nameFn)
    : m_strings(strings), m_opener(std::move(opener)), m_nameFn(std::move(nameFn)) {
  if (!m_nameFn) m_nameFn = [](std::string_view n) { return demangle(n); };
}

Result<void> DwarfSymbolProvider::load(const Module& module, const std::filesystem::path& symbolFile) {
  Result<ElfFile> opened = ElfFile::open(symbolFile);
  if (!opened.ok()) return opened.error();
  const ElfFile& elf = opened.value();

  if (module.id.kind == 2 && module.id.len > 0) {  // GNU build-id
    const std::optional<std::vector<uint8_t>> bid = elf.buildId();
    if (bid &&
        (bid->size() != module.id.len || !std::equal(bid->begin(), bid->end(), module.id.bytes.begin()))) {
      return makeError(ErrorCode::NotFound, symbolFile.string() + ": build-id does not match the module");
    }
  }

  Result<std::vector<ElfSymbol>> symbols = elf.readCodeSymbols();
  if (!symbols.ok()) return symbols.error();
  ElfSymbolTable table;
  table.build(std::move(symbols).value(), elf.loadBias());

  std::unique_ptr<IDwarfSession> session;
  if (elf.hasDebugInfo() && m_opener) {
    Result<std::unique_ptr<IDwarfSession>> s = m_opener->open(elf);
    if (!s.ok()) return s.error();
    session = std::move(s).value();
  }
  if (!session && table.size() == 0) {
    return makeError(ErrorCode::NotFound, symbolFile.string() + ": no debug info and no code symbols");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  m_bias = elf.loadBias();
  m_table = std::move(table);
  m_session = std::move(session);
  m_functionNames.clear();
  m_rawNames.clear();
  m_files.clear();
  m_loaded = true;
  return Result<void>();
}

StringIdx DwarfSymbolProvider::internRaw(std::string_view raw, uint64_t nameKey) {
  auto it = m_rawNames.find(raw);
  if (it != m_rawNames.end()) return it->second;
  // Preferred: the DIE's qualified source name (no parameter list, like DIA). Otherwise the demangled raw
  // name with its parameter list stripped (symbol-table names, or DIEs without a usable name).
  std::string display;
  if (m_session && nameKey != 0) display.assign(m_session->sourceName(nameKey));
  if (display.empty()) {
    display = m_nameFn(raw);
    if (display != raw) display = stripParameterList(display);  // only names that were mangled
  }
  const StringIdx idx = m_strings.intern(display);
  m_rawNames.emplace(raw, idx);
  return idx;
}

StringIdx DwarfSymbolProvider::internFile(std::string_view file) {
  if (file.empty()) return StringIdx();
  auto it = m_files.find(file);
  if (it != m_files.end()) return it->second;
  const StringIdx idx = m_strings.intern(file);
  m_files.emplace(file, idx);
  return idx;
}

std::optional<DwarfSymbolProvider::FunctionView> DwarfSymbolProvider::functionFor(uint64_t rva) {
  if (rva > UINT64_MAX - m_bias) return std::nullopt;
  const uint64_t addr = rva + m_bias;
  if (m_session) {
    const std::optional<DwarfFunction> hit = m_session->findFunction(addr);
    // Defensive: the answer must contain the address and live above the load bias.
    if (hit && hit->piece.lo <= addr && addr < hit->piece.hi && hit->entry >= m_bias &&
        hit->piece.lo >= m_bias) {
      FunctionView v;
      v.fromDwarf = true;
      v.id = hit->id;
      v.hit = *hit;
      v.startRva = hit->entry - m_bias;
      v.pieceLo = hit->piece.lo - m_bias;
      v.pieceSize = hit->piece.hi - hit->piece.lo;
      auto cached = m_functionNames.find(hit->id);
      if (cached == m_functionNames.end()) {
        const std::string_view raw = m_session->functionName(hit->id);
        StringIdx name;
        if (!raw.empty()) {
          name = internRaw(raw, hit->nameKey);
        } else if (const ElfTableEntry* sym = m_table.find(v.startRva)) {
          name = internRaw(sym->name);
        } else {
          name = m_strings.intern(hexName(v.startRva));
        }
        cached = m_functionNames.emplace(hit->id, name).first;
      }
      v.name = cached->second;
      return v;
    }
  }
  if (const ElfTableEntry* sym = m_table.find(rva)) {
    FunctionView v;
    v.startRva = sym->rva;
    v.size = sym->size;
    v.pieceLo = sym->rva;
    v.pieceSize = sym->size;
    v.name = internRaw(sym->name);
    return v;
  }
  return std::nullopt;
}

ResolvedAddress DwarfSymbolProvider::resolve(uint64_t rva) {
  ResolvedAddress out;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_loaded) return out;
  const std::optional<FunctionView> fn = functionFor(rva);
  if (!fn) return out;
  out.resolved = true;
  out.function = fn->name;
  out.functionStartRva = fn->startRva;
  if (!m_session) return out;  // symbol-table-only module: no lines, no inlines
  const uint64_t addr = rva + m_bias;

  const std::optional<DwarfLineRow> row = m_session->findLine(addr);
  if (row) {
    out.file = internFile(row->file);
    out.line = row->line;
  }
  if (!fn->fromDwarf) return out;

  const std::vector<DwarfInlineSite> sites = m_session->findInlines(fn->id, addr);
  if (sites.empty()) return out;
  // Positions: frame 0 sits at the line-table position, every outer frame at the call site of the one inside
  // it. A frame without a position takes the next outer frame's, the outermost one the line-table position.
  const size_t n = sites.size();
  std::vector<Position> pos(n);
  const Position tablePos{row ? row->file : std::string_view(), row ? row->line : 0};
  pos[0] = tablePos;
  for (size_t k = 1; k < n; ++k) pos[k] = Position{sites[k - 1].callFile, sites[k - 1].callLine};
  for (size_t k = n; k > 0; --k) {
    Position& p = pos[k - 1];
    const Position& outer = k < n ? pos[k] : tablePos;
    if (p.line == 0) p.line = outer.line;
    if (p.file.empty()) p.file = outer.file;
  }
  out.inlines.reserve(n);
  for (size_t k = 0; k < n; ++k) {
    InlineFrame frame;
    frame.function =
        sites[k].name.empty() ? m_strings.intern("<inlined>") : internRaw(sites[k].name, sites[k].nameKey);
    frame.file = internFile(pos[k].file);
    frame.line = pos[k].line;
    out.inlines.push_back(frame);
  }
  return out;
}

Result<std::vector<LineRange>> DwarfSymbolProvider::linesForFunction(uint64_t functionStartRva) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_loaded) return makeError(ErrorCode::InvalidArgument, "provider not loaded");
  const std::optional<FunctionView> fn = functionFor(functionStartRva);
  if (!fn || fn->startRva != functionStartRva) {
    return makeError(ErrorCode::NotFound, "no function starts at the given rva");
  }
  std::vector<LineRange> out;
  if (!m_session) return out;

  std::vector<DwarfAddrRange> ranges;
  if (fn->fromDwarf) {
    ranges = m_session->functionRanges(fn->id);
  } else {
    ranges.push_back(DwarfAddrRange{fn->startRva + m_bias, fn->startRva + m_bias + fn->size});
  }
  for (const DwarfAddrRange& r : ranges) {
    if (r.lo < m_bias) continue;
    Result<std::vector<DwarfLineRow>> rows = m_session->findLines(r.lo, r.hi);
    if (!rows.ok()) {
      if (fn->fromDwarf) return rows.error();
      continue;  // symbol-table function: lines are best effort
    }
    for (const DwarfLineRow& row : rows.value()) {
      if (row.hi <= row.lo || row.lo < m_bias) continue;
      LineRange l;
      l.rva = row.lo - m_bias;
      l.size = static_cast<uint32_t>(std::min<uint64_t>(row.hi - row.lo, UINT32_MAX));
      l.file = internFile(row.file);
      l.line = row.line;
      out.push_back(l);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const LineRange& a, const LineRange& b) { return a.rva < b.rva; });
  // Merge neighbours that continue the same line.
  std::vector<LineRange> merged;
  merged.reserve(out.size());
  for (const LineRange& l : out) {
    if (!merged.empty()) {
      LineRange& last = merged.back();
      if (last.file == l.file && last.line == l.line && last.rva + last.size == l.rva &&
          static_cast<uint64_t>(last.size) + l.size <= UINT32_MAX) {
        last.size += l.size;
        continue;
      }
    }
    merged.push_back(l);
  }
  return merged;
}

Result<FunctionBounds> DwarfSymbolProvider::functionBounds(uint64_t rva) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_loaded) return makeError(ErrorCode::InvalidArgument, "provider not loaded");
  const std::optional<FunctionView> fn = functionFor(rva);
  if (!fn) return makeError(ErrorCode::NotFound, "no function covers the given rva");
  FunctionBounds b;
  b.startRva = fn->pieceLo;  // the contiguous piece that contains rva (see the header)
  b.size = fn->pieceSize;
  b.name = fn->name;
  return b;
}

DwarfSessionStats DwarfSymbolProvider::sessionStats() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_session ? m_session->stats() : DwarfSessionStats{};
}

void registerDwarfProvider(SymbolService& service, std::shared_ptr<IDwarfOpener> opener) {
  if (!opener) opener = makeLibdwOpener();
  SymbolStringTable& strings = service.stringSink();
  service.setProviderFactory(
      [&strings, opener = std::move(opener)](
          const Module&, const std::filesystem::path& file) -> std::unique_ptr<ISymbolProvider> {
        if (hasExtension(file, ".json")) return std::make_unique<JsonSymbolProvider>(strings);
        if (hasElfMagic(file)) return std::make_unique<DwarfSymbolProvider>(strings, opener);
        return nullptr;
      });
}

}  // namespace tachyon::symbols

#endif  // TACHYON_HAVE_LIBDW
