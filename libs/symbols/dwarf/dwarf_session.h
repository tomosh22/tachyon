// Backend seam of the DWARF symbol provider (WP-72). DwarfSymbolProvider (dwarf_provider.h) holds all
// platform-neutral logic (RVA mapping, symtab fallback, name interning and demangling, inline position
// fallbacks, locking, error mapping); everything that touches libdw sits behind these interfaces, so the
// provider is unit-tested with fakes and the real session (libdw_session.cpp) is tested on the fixture
// binaries.
//
// ADDRESS SPACE: all addresses in this interface are DWARF (link-time virtual) addresses. The provider maps
// RVAs with `address = rva + ElfFile::loadBias()`.
//
// STRING LIFETIME: string_views returned by a session stay valid for the session's lifetime.
#pragma once

#include "core/result.h"
#include "symbols/dwarf/elf_reader.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace tachyon::symbols {

struct DwarfAddrRange {
  uint64_t lo = 0;
  uint64_t hi = 0;  // exclusive
};

// A physical function (DW_TAG_subprogram with code) containing the queried address.
struct DwarfFunction {
  uint64_t id = 0;     // opaque, stable for the session's lifetime (names the function in the calls below)
  uint64_t entry = 0;  // entry pc: DW_AT_entry_pc if inside the function, else the low_pc of the first range
  DwarfAddrRange piece;  // the contiguous piece of the function that contains the queried address
  uint64_t nameKey = 0;  // names the function's DIE for IDwarfSession::sourceName()
};

// One inlined call, innermost first in findInlines(). `callFile`/`callLine` is the position of the call in
// the enclosing function (DW_AT_call_file / DW_AT_call_line); empty / 0 when absent.
struct DwarfInlineSite {
  std::string_view name;  // raw (possibly mangled) name of the inlined function; empty when unknown
  std::string_view callFile;
  uint32_t callLine = 0;
  uint64_t nameKey = 0;  // names the inlined function's DIE for IDwarfSession::sourceName()
};

// A line-table row range [lo, hi).
struct DwarfLineRow {
  uint64_t lo = 0;
  uint64_t hi = 0;
  std::string_view file;
  uint32_t line = 0;
};

// One open ELF/DWARF object. NOT thread-safe: DwarfSymbolProvider serializes every call with its mutex.
class IDwarfSession {
 public:
  virtual ~IDwarfSession() = default;
  // The innermost function with code containing addr.
  virtual std::optional<DwarfFunction> findFunction(uint64_t addr) = 0;
  // Raw name of a function: the linkage name when present (the provider demangles it), else a qualified
  // source name; empty if the DIE carries none.
  virtual std::string_view functionName(uint64_t id) = 0;
  // Qualified source name ("ns::Foo::bar") of the DIE behind a DwarfFunction / DwarfInlineSite `nameKey`,
  // without parameter types. The provider falls back to it when a linkage name cannot be demangled (e.g.
  // clang's C++20 constraint manglings, which libstdc++'s demangler rejects). Empty when unknown.
  virtual std::string_view sourceName(uint64_t nameKey) {
    (void)nameKey;
    return {};
  }
  // All code pieces of the function, sorted by lo.
  virtual std::vector<DwarfAddrRange> functionRanges(uint64_t id) = 0;
  // Inlined calls active at addr inside function `id`, innermost first; empty if none.
  virtual std::vector<DwarfInlineSite> findInlines(uint64_t id, uint64_t addr) = 0;
  // The line-table row covering addr.
  virtual std::optional<DwarfLineRow> findLine(uint64_t addr) = 0;
  // Every line-table row range intersecting [lo, hi), clipped to it, ascending.
  virtual Result<std::vector<DwarfLineRow>> findLines(uint64_t lo, uint64_t hi) = 0;
};

// Opens debug info. Thread-safe: open() may be called concurrently (SymbolService loads modules in parallel).
class IDwarfOpener {
 public:
  virtual ~IDwarfOpener() = default;
  // Opens the DWARF of `elf`. CorruptData for damaged debug info; NotFound when the file carries none.
  virtual Result<std::unique_ptr<IDwarfSession>> open(const ElfFile& elf) = 0;
};

// Thread-safe. The libdw-backed opener, or nullptr when libdw is not compiled in.
std::shared_ptr<IDwarfOpener> makeLibdwOpener();

}  // namespace tachyon::symbols
