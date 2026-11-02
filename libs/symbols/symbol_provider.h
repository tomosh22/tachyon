// Symbol provider interface (spec 3.6).
//
// STRING TABLE: every StringIdx in ResolvedAddress, InlineFrame, LineRange and FunctionBounds indexes the
// symbol string table owned by the SymbolService (SymbolService::symbolStrings()). It is NOT the trace's own
// TraceModel::strings. Mixing the two tables yields wrong names without any error.
#pragma once

#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace tachyon::symbols {

using model::Module;
using model::ModuleId;
using model::SymbolState;

// One level of an inline chain. Strings: symbol string table.
struct InlineFrame {
  StringIdx function;
  StringIdx file;
  uint32_t line = 0;
};

// Result of resolving a module-relative address. Strings: symbol string table.
struct ResolvedAddress {
  StringIdx function;  // physical (outermost) function containing the address
  uint64_t functionStartRva = 0;
  StringIdx file;
  uint32_t line = 0;
  std::vector<InlineFrame> inlines;  // innermost first; empty when the address is not inside an inlined call
  // Position, in the PHYSICAL function's own source, of the call that the outermost inline frame was expanded
  // from (the "call site" of the whole inline chain). Filled only when `inlines` is non-empty; callSiteLine
  // is 0 and callSiteFile invalid when the provider cannot tell. JSON: the line-table position (ra.file/line,
  // which is already in the function's file there). DWARF: DW_AT_call_file/call_line of the outermost inline
  // site. DIA: best effort, the last line row of the function's own file at or before the address. Note that
  // `line`/`file` above is the line-table position (inside the innermost inlined callee for DWARF/DIA), and
  // inlines[k].line for k >= 1 is a position inside that frame's function.
  StringIdx callSiteFile;
  uint32_t callSiteLine = 0;
  bool resolved = false;  // false: no function covers the address (gap, out of range, unknown module)
};

// A source line covering [rva, rva + size). Strings: symbol string table.
struct LineRange {
  uint64_t rva = 0;
  uint32_t size = 0;
  StringIdx file;
  uint32_t line = 0;
};

// A function's extent. Strings: symbol string table.
struct FunctionBounds {
  uint64_t startRva = 0;
  uint64_t size = 0;
  StringIdx name;
};

// Thread-safe: pure function. The shared "not resolved" value returned for unresolved lookups.
const ResolvedAddress& unresolvedAddress();

// One provider instance serves one module. Providers intern strings into the SymbolService's string table,
// handed to them at construction (see JsonSymbolProvider).
class ISymbolProvider {
 public:
  virtual ~ISymbolProvider() = default;

  // Not thread-safe; call once, before any other member. Reads the symbol file for `module` (matched by
  // ModuleId, then by path) and builds the lookup tables. Returns NotFound when the file holds no matching
  // module, CorruptData / IoError for unreadable files.
  virtual Result<void> load(const Module& module, const std::filesystem::path& symbolFile) = 0;

  // Thread-safe after load() succeeds. Resolves a module-relative address; resolved=false for gaps / out of
  // range.
  virtual ResolvedAddress resolve(uint64_t rva) = 0;

  // Thread-safe after load() succeeds. Line ranges of the function starting exactly at functionStartRva,
  // sorted by rva. NotFound if no function starts there.
  virtual Result<std::vector<LineRange>> linesForFunction(uint64_t functionStartRva) = 0;

  // Thread-safe after load() succeeds. Bounds of the function containing rva; NotFound for gaps / out of
  // range.
  virtual Result<FunctionBounds> functionBounds(uint64_t rva) = 0;
};

}  // namespace tachyon::symbols
