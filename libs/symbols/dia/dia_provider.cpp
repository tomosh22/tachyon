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
  m_names.clear();
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

std::optional<DiaSymbolProvider::FunctionInfo> DiaSymbolProvider::functionFor(uint64_t rva) {
  std::optional<DiaRawFunction> raw = m_session->findFunction(rva);
  if (!raw) return std::nullopt;
  // Defensive: a bounded answer must contain the address.
  if (raw->size > 0 && (rva < raw->startRva || rva - raw->startRva >= raw->size)) return std::nullopt;
  const uint64_t key = raw->startRva | (raw->isPublicSymbol ? (1ull << 63) : 0ull);
  auto it = m_names.find(key);
  if (it == m_names.end()) {
    ++m_functionLookups;
    it = m_names.emplace(key, internName(raw->name)).first;
  }
  FunctionInfo info;
  info.name = it->second;
  info.raw = std::move(*raw);
  return info;
}

ResolvedAddress DiaSymbolProvider::resolve(uint64_t rva) {
  ResolvedAddress out;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_session) return out;
  const std::optional<FunctionInfo> fn = functionFor(rva);
  if (!fn) return out;
  out.resolved = true;
  out.function = fn->name;
  out.functionStartRva = fn->raw.startRva;
  if (fn->raw.isPublicSymbol) return out;  // public symbols carry neither lines nor inline sites

  const std::optional<DiaRawLine> line = m_session->findLine(rva);
  if (line) {
    out.file = internFile(line->file);
    out.line = line->line;
  }
  std::vector<DiaRawInline> inlines = m_session->findInlines(fn->raw, rva);
  // A frame without a position of its own takes the next outer frame's (the call site is inside it), and the
  // outermost one the physical function's line-table position.
  for (size_t i = inlines.size(); i > 0; --i) {
    DiaRawInline& frame = inlines[i - 1];
    if (frame.line != 0 && !frame.file.empty()) continue;
    if (i < inlines.size()) {
      const DiaRawInline& outer = inlines[i];
      if (frame.line == 0) frame.line = outer.line;
      if (frame.file.empty()) frame.file = outer.file;
    } else if (line) {
      if (frame.line == 0) frame.line = line->line;
      if (frame.file.empty()) frame.file = line->file;
    }
  }
  for (const DiaRawInline& in : inlines) {
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
  const std::optional<FunctionInfo> fn = functionFor(functionStartRva);
  if (!fn || fn->raw.startRva != functionStartRva) {
    return makeError(ErrorCode::NotFound, "no function starts at the given rva");
  }
  std::vector<LineRange> out;
  if (fn->raw.size == 0) return out;  // public symbol / unbounded: no line information
  const uint64_t start = fn->raw.startRva;
  const uint64_t end = start + fn->raw.size;
  auto lines = m_session->findLines(start, fn->raw.size);
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
  const std::optional<FunctionInfo> fn = functionFor(rva);
  if (!fn) return makeError(ErrorCode::NotFound, "no function covers the given rva");
  FunctionBounds b;
  b.startRva = fn->raw.startRva;
  b.size = fn->raw.size;  // 0 = unbounded public symbol (no known end)
  b.name = fn->name;
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
