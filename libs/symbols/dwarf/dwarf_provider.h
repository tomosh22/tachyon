// DWARF-backed ISymbolProvider for Linux (WP-72): functions, inline chains, lines and function bounds from
// ELF debug info through elfutils libdw, with an ELF .symtab/.dynsym fallback for code without DWARF.
//
// Structure (mirrors the DIA provider): DwarfSymbolProvider is platform-neutral and talks to an IDwarfSession
// (dwarf_session.h). The real session is implemented over libdw (libdw_session.cpp); tests substitute a fake.
//
// RVA MAPPING (binding): the model's Module::base is the load address of the module's first PT_LOAD mapping
// (page aligned), so rva = runtime address - base. The DWARF / ELF virtual address of an rva is
//     dwarfAddress = rva + loadBias,  loadBias = (lowest PT_LOAD p_vaddr) aligned down to 4096
// for ET_DYN (PIE, shared objects: loadBias is normally 0) and ET_EXEC alike (e.g. 0x400000).
//
// FUNCTIONS: from DW_TAG_subprogram DIEs with code (DW_AT_low_pc/high_pc or DW_AT_ranges; a function split
// into hot/cold pieces has several ranges). Its identity is the ENTRY pc: DW_AT_entry_pc when present and
// inside one of the ranges, else the low_pc of the FIRST range in DWARF order (compilers list the hot part
// first). Every piece resolves to that same functionStartRva. Code not covered by DWARF falls back to the ELF
// symbol table (bounded by st_size; st_size 0 is bounded by the next symbol and the section end).
// functionBounds() of any address of a split function reports startRva = entry and the size of the piece that
// holds the entry; linesForFunction() covers all pieces.
//
// NAMES: the linkage name is demangled (Itanium, abi::__cxa_demangle), so C++ functions show their parameter
// list ("fx::Widget::work(int)"); functions without a linkage name show their (qualified) source name. When a
// linkage name cannot be demangled (libstdc++ rejects clang's C++20 constraint manglings) the qualified
// source name from the DIE is used instead of the raw "_Z..." text.
//
// INLINES: innermost first. inlines[0] carries the line-table position of the address; each outer frame
// carries the call-site position (DW_AT_call_file/call_line) of the frame inside it. A frame without a
// position takes the next outer frame's, and the outermost one the line-table position (same convention as
// the DIA provider).
//
// THREADING POLICY: one libdw handle per provider, serialized by a mutex (libdw's lazy caches are not safe
// for concurrent use). SymbolService loads one provider per module, so there is no contention across modules.
#pragma once

#ifndef TACHYON_HAVE_LIBDW
#define TACHYON_HAVE_LIBDW 0
#endif

#include "symbols/symbol_service.h"

#if TACHYON_HAVE_LIBDW

#include "symbols/dwarf/dwarf_session.h"
#include "symbols/dwarf/elf_symbol_table.h"
#include "symbols/symbol_provider.h"
#include "symbols/symbol_strings.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tachyon::symbols {

class DwarfSymbolProvider final : public ISymbolProvider {
 public:
  // Maps a raw (possibly mangled) name to its display name (default: demangle()).
  using NameFn = std::function<std::string(std::string_view)>;

  // Thread-safe (constructor). `strings` must outlive the provider; every StringIdx it returns indexes it.
  DwarfSymbolProvider(SymbolStringTable& strings, std::shared_ptr<IDwarfOpener> opener, NameFn nameFn = {});

  // Not thread-safe; see ISymbolProvider. `symbolFile` is an ELF file (debug file or the binary itself). When
  // module.id is a build-id and the file carries a different one -> NotFound. Unreadable -> IoError, damaged
  // ELF or DWARF -> CorruptData, neither DWARF nor symbols -> NotFound.
  Result<void> load(const Module& module, const std::filesystem::path& symbolFile) override;
  // Thread-safe after load() (mutex-serialized).
  ResolvedAddress resolve(uint64_t rva) override;
  // Thread-safe after load() (mutex-serialized).
  Result<std::vector<LineRange>> linesForFunction(uint64_t functionStartRva) override;
  // Thread-safe after load() (mutex-serialized).
  Result<FunctionBounds> functionBounds(uint64_t rva) override;

  // Thread-safe after load(). The DWARF address of rva 0 (see RVA MAPPING).
  uint64_t loadBias() const { return m_bias; }
  // Thread-safe after load(). True when debug info (DWARF) is in use, false for symbol-table-only modules.
  bool hasDwarf() const { return m_session != nullptr; }

 private:
  struct FunctionView {
    uint64_t startRva = 0;
    uint64_t size = 0;  // size of the piece holding the entry (DWARF) / st_size-or-bounded (symtab)
    StringIdx name;
    bool fromDwarf = false;
    uint64_t id = 0;
    DwarfFunction hit;
  };

  // All private helpers require m_mutex.
  std::optional<FunctionView> functionFor(uint64_t rva);
  StringIdx internRaw(std::string_view raw, uint64_t nameKey = 0);
  StringIdx internFile(std::string_view file);

  SymbolStringTable& m_strings;
  std::shared_ptr<IDwarfOpener> m_opener;
  NameFn m_nameFn;

  mutable std::mutex m_mutex;
  bool m_loaded = false;
  uint64_t m_bias = 0;
  ElfSymbolTable m_table;
  std::unique_ptr<IDwarfSession> m_session;
  std::unordered_map<uint64_t, StringIdx> m_functionNames;     // DWARF function id -> interned display name
  std::unordered_map<std::string_view, StringIdx> m_rawNames;  // raw name -> interned display name
  std::unordered_map<std::string_view, StringIdx> m_files;
};

// Thread-safe. Makes `service` load ELF symbol files through libdw: ELF files (by magic number) get a
// DwarfSymbolProvider, ".json" files keep the JsonSymbolProvider, anything else fails. `opener` defaults to
// makeLibdwOpener(). Call before resolveAllAsync(); not thread-safe.
void registerDwarfProvider(SymbolService& service, std::shared_ptr<IDwarfOpener> opener = nullptr);

}  // namespace tachyon::symbols

#else  // !TACHYON_HAVE_LIBDW

namespace tachyon::symbols {

// No-op when the libdw provider is not compiled in (non-Linux builds, TACHYON_ENABLE_LIBDW=OFF).
inline void registerDwarfProvider(SymbolService&) {}

}  // namespace tachyon::symbols

#endif
