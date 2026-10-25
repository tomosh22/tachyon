#include "symbols/dia/dia_provider.h"

#include "symbols/dia/dia_errors.h"
#include "symbols/dia/msvc_undecorate.h"
#include "symbols/json_symbol_provider.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tachyon::symbols {

DiaSymbolProvider::DiaSymbolProvider(SymbolStringTable& strings, std::shared_ptr<IDiaSessionOpener> opener,
                                     NameFn nameFn)
    : m_strings(strings), m_opener(std::move(opener)), m_nameFn(std::move(nameFn)) {
  if (!m_nameFn) m_nameFn = [](std::string_view n) { return undecorateMsvc(n); };
}

Result<void> DiaSymbolProvider::load(const Module& module, const std::filesystem::path& symbolFile) {
  if (!m_opener) return makeError(ErrorCode::Unsupported, "DIA is not available in this build");
  auto opened = m_opener->open(module, symbolFile);
  if (!opened.ok()) return opened.error();
  std::lock_guard<std::mutex> lock(m_mutex);
  m_session = std::move(opened).value();
  m_functions.clear();
  m_fileIdx.clear();
  return Result<void>();
}

StringIdx DiaSymbolProvider::internName(std::string_view raw) {
  return m_strings.intern(m_nameFn(raw));
}

StringIdx DiaSymbolProvider::internFile(const std::string& file) {
  if (file.empty()) return StringIdx();
  auto it = m_fileIdx.find(file);
  if (it != m_fileIdx.end()) return it->second;
  const StringIdx idx = m_strings.intern(file);
  m_fileIdx.emplace(file, idx);
  return idx;
}

const DiaSymbolProvider::CachedFunction* DiaSymbolProvider::functionFor(uint64_t rva) {
  auto it = m_functions.upper_bound(rva);
  if (it != m_functions.begin()) {
    --it;
    if (rva - it->second.startRva < it->second.size) return &it->second;
  }
  ++m_functionLookups;
  const std::optional<DiaRawFunction> raw = m_session->findFunction(rva);
  if (!raw) return nullptr;
  if (raw->size > 0 && (rva < raw->startRva || rva - raw->startRva >= raw->size)) return nullptr;
  CachedFunction cf;
  cf.startRva = raw->startRva;
  cf.size = raw->size;
  cf.name = internName(raw->name);
  cf.isPublicSymbol = raw->isPublicSymbol;
  if (cf.size == 0) {
    // Extent unknown: never cached as a range, only remembered for the duration of this call.
    m_scratch = cf;
    return &m_scratch;
  }
  auto [pos, inserted] = m_functions.emplace(cf.startRva, cf);
  if (!inserted) {
    m_scratch = cf;  // overlapping duplicate start (e.g. a hot/cold split); answer without caching
    return &m_scratch;
  }
  return &pos->second;
}

ResolvedAddress DiaSymbolProvider::resolve(uint64_t rva) {
  ResolvedAddress out;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_session) return out;
  const CachedFunction* cf = functionFor(rva);
  if (cf == nullptr) return out;
  out.resolved = true;
  out.function = cf->name;
  out.functionStartRva = cf->startRva;
  if (cf->isPublicSymbol) return out;  // public symbols carry neither lines nor inline sites

  if (const std::optional<DiaRawLine> line = m_session->findLine(rva)) {
    out.file = internFile(line->file);
    out.line = line->line;
  }
  DiaRawFunction raw;
  raw.startRva = cf->startRva;
  raw.size = cf->size;
  for (DiaRawInline& in : m_session->findInlines(raw, rva)) {
    InlineFrame frame;
    frame.function = internName(in.name);
    frame.file = internFile(in.file);
    frame.line = in.line;
    out.inlines.push_back(frame);
  }
  return out;
}

Result<std::vector<LineRange>> DiaSymbolProvider::linesForFunction(uint64_t functionStartRva) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_session) return makeError(ErrorCode::InvalidArgument, "provider not loaded");
  const CachedFunction* cf = functionFor(functionStartRva);
  if (cf == nullptr || cf->startRva != functionStartRva) {
    return makeError(ErrorCode::NotFound, "no function starts at the given rva");
  }
  std::vector<LineRange> out;
  if (cf->size == 0) return out;
  const uint64_t start = cf->startRva;
  const uint64_t end = start + cf->size;
  auto lines = m_session->findLines(start, cf->size);
  if (!lines.ok()) return lines.error();
  out.reserve(lines.value().size());
  for (const DiaRawLine& l : lines.value()) {
    if (l.rva < start || l.rva >= end) continue;
    LineRange r;
    r.rva = l.rva;
    r.size = l.size;
    r.file = internFile(l.file);
    r.line = l.line;
    out.push_back(r);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const LineRange& a, const LineRange& b) { return a.rva < b.rva; });
  return out;
}

Result<FunctionBounds> DiaSymbolProvider::functionBounds(uint64_t rva) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_session) return makeError(ErrorCode::InvalidArgument, "provider not loaded");
  const CachedFunction* cf = functionFor(rva);
  if (cf == nullptr) return makeError(ErrorCode::NotFound, "no function covers the given rva");
  FunctionBounds b;
  b.startRva = cf->startRva;
  b.size = cf->size;
  b.name = cf->name;
  return b;
}

uint64_t DiaSymbolProvider::functionLookups() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_functionLookups;
}

namespace {

bool hasExtension(const std::filesystem::path& p, const char* ext) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e == ext;
}

}  // namespace

#if !TACHYON_HAVE_DIA
std::shared_ptr<IDiaSessionOpener> makeComDiaOpener(std::string* error) {
  if (error != nullptr) *error = "DIA support is not compiled into this build";
  return nullptr;
}
#endif

void registerDiaProvider(SymbolService& service, std::shared_ptr<IDiaSessionOpener> opener) {
  if (!opener) opener = makeComDiaOpener();
  SymbolStringTable& strings = service.stringSink();
  service.setProviderFactory(
      [&strings, opener = std::move(opener)](
          const Module&, const std::filesystem::path& file) -> std::unique_ptr<ISymbolProvider> {
        if (hasExtension(file, ".json")) return std::make_unique<JsonSymbolProvider>(strings);
        if (hasExtension(file, ".pdb") && opener) return std::make_unique<DiaSymbolProvider>(strings, opener);
        return nullptr;
      });
}

}  // namespace tachyon::symbols
