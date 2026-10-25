// Real DIA session over msdia140.dll (WP-22, Windows only). msdia140.dll is loaded registration-free: the DLL
// is found next to the executable (the build copies it there), at the DIA SDK path baked in by CMake, or on
// the normal search path; the data source comes from DllGetClassObject -> IClassFactory -> IDiaDataSource, so
// nothing is ever registered in the registry.
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS && TACHYON_HAVE_DIA

#include "symbols/dia/dia_errors.h"
#include "symbols/dia/dia_session.h"
#include "symbols/dia/dia_symbol_table.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>

// The DIA SDK headers are not /W4 clean.
#pragma warning(push, 0)
#include <dia2.h>
#pragma warning(pop)

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tachyon::symbols {
namespace {

template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&& o) noexcept : m_p(std::exchange(o.m_p, nullptr)) {}
  ComPtr& operator=(ComPtr&& o) noexcept {
    if (this != &o) {
      reset();
      m_p = std::exchange(o.m_p, nullptr);
    }
    return *this;
  }
  T* get() const { return m_p; }
  T* operator->() const { return m_p; }
  explicit operator bool() const { return m_p != nullptr; }
  T** put() {
    reset();
    return &m_p;
  }
  void reset() {
    if (m_p != nullptr) {
      m_p->Release();
      m_p = nullptr;
    }
  }

 private:
  T* m_p = nullptr;
};

// COM apartment of the calling thread, entered on first use and left when the thread exits (calls arrive from
// SymbolService worker threads; initializing per call would dominate the cost of a lookup).
class ComScope {
 public:
  ComScope() : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(m_hr)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  HRESULT m_hr;
};

void ensureComInitialized() {
  thread_local ComScope scope;
  (void)scope;
}

std::string wideToUtf8(const wchar_t* w, size_t len) {
  if (w == nullptr || len == 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), out.data(), n, nullptr, nullptr);
  return out;
}

// Takes ownership of (frees) the BSTR.
std::string takeBstr(BSTR b) {
  if (b == nullptr) return {};
  std::string out = wideToUtf8(b, SysStringLen(b));
  SysFreeString(b);
  return out;
}

class DiaLibrary {
 public:
  static std::shared_ptr<DiaLibrary> load(std::string* error) {
    std::vector<std::wstring> candidates;
    wchar_t exe[4096] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n > 0 && n < std::size(exe)) {
      candidates.push_back((std::filesystem::path(exe).parent_path() / L"msdia140.dll").wstring());
    }
#ifdef TACHYON_DIA_DLL_PATH
    candidates.push_back(std::filesystem::path(TACHYON_DIA_DLL_PATH).wstring());
#endif
    std::string tried;
    for (const std::wstring& c : candidates) {
      HMODULE h =
          LoadLibraryExW(c.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (h == nullptr) {
        tried += " " + wideToUtf8(c.c_str(), c.size()) + " (error " + std::to_string(GetLastError()) + ");";
        continue;
      }
      auto fn = reinterpret_cast<GetClassObjectFn>(GetProcAddress(h, "DllGetClassObject"));
      if (fn == nullptr) {
        FreeLibrary(h);
        tried += " " + wideToUtf8(c.c_str(), c.size()) + " (no DllGetClassObject);";
        continue;
      }
      return std::shared_ptr<DiaLibrary>(new DiaLibrary(h, fn));
    }
    if (error != nullptr) *error = "cannot load msdia140.dll; tried:" + tried;
    return nullptr;
  }

  ~DiaLibrary() { FreeLibrary(m_module); }
  DiaLibrary(const DiaLibrary&) = delete;
  DiaLibrary& operator=(const DiaLibrary&) = delete;

  HRESULT createDataSource(ComPtr<IDiaDataSource>& out) const {
    ComPtr<IClassFactory> factory;
    HRESULT hr = m_getClassObject(__uuidof(DiaSource), __uuidof(IClassFactory),
                                  reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) return hr;
    return factory->CreateInstance(nullptr, __uuidof(IDiaDataSource), reinterpret_cast<void**>(out.put()));
  }

 private:
  using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
  DiaLibrary(HMODULE module, GetClassObjectFn fn) : m_module(module), m_getClassObject(fn) {}

  HMODULE m_module;
  GetClassObjectFn m_getClassObject;
};

std::optional<DiaRawLine> toRawLine(IDiaLineNumber* ln) {
  DWORD rva = 0;
  DWORD length = 0;
  DWORD number = 0;
  if (FAILED(ln->get_relativeVirtualAddress(&rva))) return std::nullopt;
  ln->get_length(&length);
  ln->get_lineNumber(&number);
  DiaRawLine out;
  out.rva = rva;
  out.size = length;
  out.line = number;
  ComPtr<IDiaSourceFile> file;
  if (SUCCEEDED(ln->get_sourceFile(file.put())) && file) {
    BSTR name = nullptr;
    if (SUCCEEDED(file->get_fileName(&name))) out.file = takeBstr(name);
  }
  return out;
}

class ComSession final : public IDiaSymbolSession {
 public:
  ComSession(std::shared_ptr<DiaLibrary> lib, ComPtr<IDiaDataSource> source, ComPtr<IDiaSession> session)
      : m_lib(std::move(lib)), m_source(std::move(source)), m_session(std::move(session)) {}

  // Enumerates the global function and public symbols and the code section contributions once into a
  // DiaSymbolTable, so address lookups are binary searches in this process instead of a DIA query per address
  // (a failing findSymbolByRVA costs ~190 us). Fails (instead of yielding empty tables) when DIA refuses.
  HRESULT buildTables() {
    ensureComInitialized();
    ComPtr<IDiaSymbol> global;
    HRESULT hr = m_session->get_globalScope(global.put());
    if (FAILED(hr) || !global) return FAILED(hr) ? hr : E_FAIL;
    std::vector<DiaTableEntry> privates;
    std::vector<DiaTableEntry> publics;
    hr = enumerate(global.get(), SymTagFunction, /*isPublic=*/false, privates);
    if (FAILED(hr)) return hr;
    hr = enumerate(global.get(), SymTagPublicSymbol, /*isPublic=*/true, publics);
    if (FAILED(hr)) return hr;
    std::vector<DiaCodeRange> code = codeContributions();
    m_table.build(std::move(privates), std::move(publics), std::move(code));
    return S_OK;
  }

  std::optional<DiaRawFunction> findFunction(uint64_t rva) override { return m_table.find(rva); }

  std::optional<DiaRawLine> findLine(uint64_t rva) override {
    if (rva > std::numeric_limits<DWORD>::max()) return std::nullopt;
    ensureComInitialized();
    ComPtr<IDiaEnumLineNumbers> lines;
    if (m_session->findLinesByRVA(static_cast<DWORD>(rva), 1, lines.put()) != S_OK || !lines) {
      return std::nullopt;
    }
    ComPtr<IDiaLineNumber> ln;
    ULONG fetched = 0;
    if (lines->Next(1, ln.put(), &fetched) != S_OK || fetched != 1) return std::nullopt;
    return toRawLine(ln.get());
  }

  std::vector<DiaRawInline> findInlines(const DiaRawFunction& function, uint64_t rva) override {
    std::vector<DiaRawInline> out;
    if (rva > std::numeric_limits<DWORD>::max()) return out;
    ensureComInitialized();
    const DWORD rva32 = static_cast<DWORD>(rva);
    IDiaSymbol* parent = functionSymbol(function.startRva);
    if (parent == nullptr) return out;
    ComPtr<IDiaEnumSymbols> frames;
    if (parent->findInlineFramesByRVA(rva32, frames.put()) != S_OK || !frames) return out;
    for (;;) {
      ComPtr<IDiaSymbol> inl;
      ULONG fetched = 0;
      if (frames->Next(1, inl.put(), &fetched) != S_OK || fetched != 1) break;
      DiaRawInline frame;
      BSTR name = nullptr;
      if (SUCCEEDED(inl->get_name(&name))) frame.name = takeBstr(name);
      ComPtr<IDiaEnumLineNumbers> lines;
      if (m_session->findInlineeLinesByRVA(inl.get(), rva32, 1, lines.put()) == S_OK && lines) {
        ComPtr<IDiaLineNumber> ln;
        ULONG got = 0;
        if (lines->Next(1, ln.put(), &got) == S_OK && got == 1) {
          if (const auto raw = toRawLine(ln.get())) {
            frame.file = raw->file;
            frame.line = raw->line;
          }
        }
      }
      out.push_back(std::move(frame));
    }
    return out;
  }

  Result<std::vector<DiaRawLine>> findLines(uint64_t rva, uint64_t size) override {
    std::vector<DiaRawLine> out;
    if (rva > std::numeric_limits<DWORD>::max() || size > std::numeric_limits<DWORD>::max()) return out;
    ensureComInitialized();
    ComPtr<IDiaEnumLineNumbers> lines;
    const HRESULT hr =
        m_session->findLinesByRVA(static_cast<DWORD>(rva), static_cast<DWORD>(size), lines.put());
    if (FAILED(hr)) return makeError(ErrorCode::Internal, "findLinesByRVA failed");
    if (!lines) return out;
    for (;;) {
      ComPtr<IDiaLineNumber> ln;
      ULONG fetched = 0;
      if (lines->Next(1, ln.put(), &fetched) != S_OK || fetched != 1) break;
      if (const auto raw = toRawLine(ln.get())) out.push_back(*raw);
    }
    return out;
  }

 private:
  // Code section contributions (per object file); public symbol extents are clipped to them.
  std::vector<DiaCodeRange> codeContributions() {
    std::vector<DiaCodeRange> out;
    ComPtr<IDiaEnumTables> tables;
    if (FAILED(m_session->getEnumTables(tables.put())) || !tables) return out;
    for (;;) {
      ComPtr<IDiaTable> table;
      ULONG fetched = 0;
      if (tables->Next(1, table.put(), &fetched) != S_OK || fetched != 1) break;
      ComPtr<IDiaEnumSectionContribs> contribs;
      if (FAILED(table->QueryInterface(__uuidof(IDiaEnumSectionContribs),
                                       reinterpret_cast<void**>(contribs.put()))) ||
          !contribs) {
        continue;
      }
      for (;;) {
        ComPtr<IDiaSectionContrib> c;
        ULONG got = 0;
        if (contribs->Next(1, c.put(), &got) != S_OK || got != 1) break;
        BOOL isCode = FALSE;
        DWORD rva = 0;
        DWORD length = 0;
        if (FAILED(c->get_code(&isCode)) || !isCode) continue;
        if (FAILED(c->get_relativeVirtualAddress(&rva)) || FAILED(c->get_length(&length)) || length == 0)
          continue;
        out.push_back({rva, length});
      }
      break;
    }
    return out;
  }

  // Appends the global children with `tag`. Public symbols that are not code (data) are skipped.
  HRESULT enumerate(IDiaSymbol* global, enum SymTagEnum tag, bool isPublic, std::vector<DiaTableEntry>& out) {
    ComPtr<IDiaEnumSymbols> en;
    const HRESULT hr = global->findChildren(tag, nullptr, nsNone, en.put());
    if (FAILED(hr)) return hr;
    if (!en) return E_FAIL;
    for (;;) {
      ComPtr<IDiaSymbol> sym;
      ULONG fetched = 0;
      if (en->Next(1, sym.put(), &fetched) != S_OK || fetched != 1) break;
      DWORD rva = 0;
      if (FAILED(sym->get_relativeVirtualAddress(&rva)) || rva == 0) continue;
      ULONGLONG length = 0;
      if (isPublic) {
        BOOL isFunction = FALSE;
        BOOL isCode = FALSE;
        sym->get_function(&isFunction);
        sym->get_code(&isCode);
        if (!isFunction && !isCode) continue;
      } else {
        sym->get_length(&length);
        if (length == 0) continue;
      }
      DiaTableEntry e;
      e.rva = rva;
      e.size = static_cast<uint32_t>(std::min<ULONGLONG>(length, 0xFFFFFFFFull));
      BSTR name = nullptr;
      // Public names stay decorated here; DiaSymbolProvider undecorates each distinct function once.
      if (SUCCEEDED(sym->get_name(&name))) e.name = takeBstr(name);
      out.push_back(std::move(e));
    }
    return S_OK;
  }

  // The function symbol starting at `start` (needed as the parent of inline queries), cached per function.
  IDiaSymbol* functionSymbol(uint64_t start) {
    auto it = m_symbols.find(start);
    if (it != m_symbols.end()) return it->second.get();
    ComPtr<IDiaSymbol> sym;
    if (m_session->findSymbolByRVA(static_cast<DWORD>(start), SymTagFunction, sym.put()) != S_OK || !sym) {
      m_symbols.emplace(start, ComPtr<IDiaSymbol>());
      return nullptr;
    }
    IDiaSymbol* raw = sym.get();
    m_symbols.emplace(start, std::move(sym));
    return raw;
  }

  std::shared_ptr<DiaLibrary> m_lib;  // declared first: destroyed last, after the COM objects below
  ComPtr<IDiaDataSource> m_source;
  ComPtr<IDiaSession> m_session;
  DiaSymbolTable m_table;
  std::unordered_map<uint64_t, ComPtr<IDiaSymbol>> m_symbols;
};

class ComOpener final : public IDiaSessionOpener {
 public:
  explicit ComOpener(std::shared_ptr<DiaLibrary> lib) : m_lib(std::move(lib)) {}

  Result<std::unique_ptr<IDiaSymbolSession>> open(const model::Module& module,
                                                  const std::filesystem::path& pdbFile) override {
    std::error_code ec;
    if (!std::filesystem::exists(pdbFile, ec)) {
      return mapDiaLoadError(dia_hr::kEPdbNotFound, pdbFile);
    }
    ensureComInitialized();
    ComPtr<IDiaDataSource> source;
    HRESULT hr = m_lib->createDataSource(source);
    if (FAILED(hr)) return mapDiaLoadError(static_cast<uint32_t>(hr), pdbFile);
    if (const auto identity = diaIdentityFor(module.id)) {
      GUID guid;
      std::memcpy(&guid, identity->guid.data(), sizeof(guid));
      hr = source->loadAndValidateDataFromPdb(pdbFile.c_str(), &guid, 0, identity->age);
    } else {
      hr = source->loadDataFromPdb(pdbFile.c_str());
    }
    if (FAILED(hr)) return mapDiaLoadError(static_cast<uint32_t>(hr), pdbFile);
    ComPtr<IDiaSession> session;
    hr = source->openSession(session.put());
    if (FAILED(hr)) return mapDiaLoadError(static_cast<uint32_t>(hr), pdbFile);
    auto result = std::make_unique<ComSession>(m_lib, std::move(source), std::move(session));
    hr = result->buildTables();
    if (FAILED(hr)) return mapDiaLoadError(static_cast<uint32_t>(hr), pdbFile);
    return std::unique_ptr<IDiaSymbolSession>(std::move(result));
  }

 private:
  std::shared_ptr<DiaLibrary> m_lib;
};

}  // namespace

std::shared_ptr<IDiaSessionOpener> makeComDiaOpener(std::string* error) {
  std::shared_ptr<DiaLibrary> lib = DiaLibrary::load(error);
  if (!lib) return nullptr;
  return std::make_shared<ComOpener>(std::move(lib));
}

}  // namespace tachyon::symbols

#endif  // TACHYON_PLATFORM_WINDOWS && TACHYON_HAVE_DIA
