// DIA-backed ISymbolProvider (WP-22): PDB functions, inline frames, lines and function bounds.
//
// Structure: DiaSymbolProvider is platform-neutral and talks to an IDiaSymbolSession (dia_session.h). The
// real session is implemented over msdia140.dll on Windows (dia_com_session.cpp, loaded registration-free
// through DllGetClassObject); tests substitute a fake.
//
// THREADING POLICY: one session per provider, serialized by a mutex. A provider serves one module and
// SymbolService loads modules in parallel, so there is no contention across modules; resolve() calls on one
// module run one at a time. (A session per thread was rejected: each would re-read the whole PDB, and a
// system DLL PDB is tens of MB.)
#pragma once

#include "symbols/dia/dia_session.h"
#include "symbols/symbol_provider.h"
#include "symbols/symbol_service.h"
#include "symbols/symbol_strings.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef TACHYON_HAVE_DIA
#define TACHYON_HAVE_DIA 0
#endif

namespace tachyon::symbols {

class DiaSymbolProvider final : public ISymbolProvider {
 public:
  // Maps a possibly decorated name to its display name (default: undecorateMsvc).
  using NameFn = std::function<std::string(std::string_view)>;

  // Thread-safe (constructor). `strings` must outlive the provider; every StringIdx it returns indexes it.
  DiaSymbolProvider(SymbolStringTable& strings, std::shared_ptr<IDiaSessionOpener> opener,
                    NameFn nameFn = {});

  // Not thread-safe; see ISymbolProvider. Opens `symbolFile` through the opener and validates it against
  // module.id (GUID + age). Missing or mismatching PDB -> NotFound, damaged PDB -> CorruptData.
  Result<void> load(const Module& module, const std::filesystem::path& symbolFile) override;
  // Thread-safe after load() (mutex-serialized).
  ResolvedAddress resolve(uint64_t rva) override;
  // Thread-safe after load() (mutex-serialized).
  Result<std::vector<LineRange>> linesForFunction(uint64_t functionStartRva) override;
  // Thread-safe after load() (mutex-serialized).
  Result<FunctionBounds> functionBounds(uint64_t rva) override;

  // Thread-safe. Number of distinct functions whose name was interned (for tests).
  uint64_t functionLookups() const;

 private:
  struct FunctionInfo {
    DiaRawFunction raw;
    StringIdx name;
  };

  // All private helpers require m_mutex.
  // One session query (cheap: the session answers from in-memory tables), plus the interned name.
  std::optional<FunctionInfo> functionFor(uint64_t rva);
  StringIdx internName(std::string_view raw);
  StringIdx internFile(const std::string& file);

  SymbolStringTable& m_strings;
  std::shared_ptr<IDiaSessionOpener> m_opener;
  NameFn m_nameFn;

  mutable std::mutex m_mutex;
  std::unique_ptr<IDiaSymbolSession> m_session;
  // Interned display name per function start (+ public flag). This is NOT an address-range cache: extents are
  // never cached (a cached public extent could shadow a private function), every address goes to the session.
  std::unordered_map<uint64_t, StringIdx> m_names;
  std::unordered_map<std::string, StringIdx> m_fileIdx;
  uint64_t m_functionLookups = 0;
};

// Thread-safe. Makes `service` load PDBs through DIA: ".pdb" symbol files get a DiaSymbolProvider, ".json"
// files keep the JsonSymbolProvider, anything else fails. `opener` defaults to makeComDiaOpener() (null on
// non-Windows builds, in which case PDB modules end up Failed). Call before resolveAllAsync(); not
// thread-safe.
void registerDiaProvider(SymbolService& service, std::shared_ptr<IDiaSessionOpener> opener = nullptr);

}  // namespace tachyon::symbols
