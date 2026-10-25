#include "symbols/json_symbol_provider.h"

#include "symbols/demangle.h"
#include "trace/trace_format.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <numeric>
#include <utility>

namespace tachyon::symbols {
namespace {

using Json = nlohmann::json;

constexpr const char* kSchema = "tachyon-symbols-1";

Error corrupt(const std::filesystem::path& file, const std::string& what) {
  return makeError(ErrorCode::CorruptData, file.string() + ": " + what);
}

Result<Json> parseFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return makeError(ErrorCode::NotFound, "cannot open " + file.string());
  Json doc = Json::parse(in, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) return corrupt(file, "not a JSON object");
  const auto schema = doc.find("schema");
  if (schema == doc.end() || !schema->is_string() || schema->get_ref<const std::string&>() != kSchema) {
    return corrupt(file, std::string("schema is not ") + kSchema);
  }
  const auto mods = doc.find("modules");
  if (mods == doc.end() || !mods->is_array()) return corrupt(file, "missing modules array");
  return doc;
}

// Accepts an unsigned number or a decimal / "0x" hex string.
bool toU64(const Json& j, uint64_t& out) {
  if (j.is_number_unsigned()) {
    out = j.get<uint64_t>();
    return true;
  }
  if (j.is_number_integer()) {
    const int64_t v = j.get<int64_t>();
    if (v < 0) return false;
    out = static_cast<uint64_t>(v);
    return true;
  }
  if (j.is_string()) {
    const std::string& s = j.get_ref<const std::string&>();
    const char* b = s.data();
    const char* e = b + s.size();
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      b += 2;
      base = 16;
    }
    const auto r = std::from_chars(b, e, out, base);
    return r.ec == std::errc() && r.ptr == e;
  }
  return false;
}

bool getU64(const Json& obj, const char* key, uint64_t& out) {
  const auto it = obj.find(key);
  return it != obj.end() && toU64(*it, out);
}

std::string getString(const Json& obj, const char* key) {
  const auto it = obj.find(key);
  return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool hexValue(char c, uint8_t& v) {
  if (c >= '0' && c <= '9')
    v = static_cast<uint8_t>(c - '0');
  else if (c >= 'a' && c <= 'f')
    v = static_cast<uint8_t>(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F')
    v = static_cast<uint8_t>(c - 'A' + 10);
  else
    return false;
  return true;
}

bool parseId(const Json& mod, ModuleId& id) {
  id = ModuleId{};
  const std::string kind = getString(mod, "id_kind");
  const std::string hex = getString(mod, "id");
  if (kind.empty() && hex.empty()) return true;  // no id
  if (kind == "pdb")
    id.kind = trace::kModuleIdPdb;
  else if (kind == "gnu-build-id")
    id.kind = trace::kModuleIdGnuBuildId;
  else
    return false;
  if (hex.size() % 2 != 0 || hex.size() / 2 > id.bytes.size()) return false;
  id.len = static_cast<uint8_t>(hex.size() / 2);
  for (size_t i = 0; i < id.len; ++i) {
    uint8_t hi = 0, lo = 0;
    if (!hexValue(hex[2 * i], hi) || !hexValue(hex[2 * i + 1], lo)) return false;
    id.bytes[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  uint64_t age = 0;
  (void)getU64(mod, "age", age);
  id.age = id.kind == trace::kModuleIdPdb ? static_cast<uint32_t>(age) : 0;
  return true;
}

JsonModuleIdentity identityOf(const Json& mod) {
  JsonModuleIdentity e;
  e.name = getString(mod, "name");
  e.path = getString(mod, "path");
  if (e.path.empty()) e.path = e.name;
  (void)parseId(mod, e.id);
  uint64_t idx = kInvalidIdx;
  (void)getU64(mod, "module_idx", idx);
  e.moduleIdx = static_cast<uint32_t>(idx);
  return e;
}

std::string lowerFilename(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name;
}

StringIdx internName(SymbolStringTable& strings, const std::string& name) {
  return strings.intern(demangle(name));
}

}  // namespace

bool moduleMatchesIdentity(const Module& module, const JsonModuleIdentity& entry) {
  if (module.id.kind != 0 && module.id.len != 0 && entry.id.kind != 0 && entry.id.len != 0) {
    if (module.id.kind != entry.id.kind || module.id.len != entry.id.len) return false;
    if (!std::equal(module.id.bytes.begin(), module.id.bytes.begin() + module.id.len,
                    entry.id.bytes.begin())) {
      return false;
    }
    return module.id.kind != trace::kModuleIdPdb || module.id.age == entry.id.age;
  }
  return !module.path.empty() && lowerFilename(module.path) == lowerFilename(entry.path);
}

JsonSymbolProvider::JsonSymbolProvider(SymbolStringTable& strings) : m_strings(strings) {}

Result<std::vector<JsonModuleIdentity>> JsonSymbolProvider::readIdentities(
    const std::filesystem::path& symbolsJson) {
  TACHYON_TRY_ASSIGN(Json doc, parseFile(symbolsJson));
  std::vector<JsonModuleIdentity> out;
  for (const Json& mod : doc["modules"]) {
    if (mod.is_object()) out.push_back(identityOf(mod));
  }
  return out;
}

Result<void> JsonSymbolProvider::load(const Module& module, const std::filesystem::path& symbolFile) {
  TACHYON_TRY_ASSIGN(Json doc, parseFile(symbolFile));

  // Prefer an id match over a path-only match.
  const Json* chosen = nullptr;
  for (const Json& mod : doc["modules"]) {
    if (!mod.is_object()) continue;
    const JsonModuleIdentity ident = identityOf(mod);
    if (!moduleMatchesIdentity(module, ident)) continue;
    const bool idMatch = module.id.len != 0 && ident.id.len != 0;
    if (idMatch) {
      chosen = &mod;
      break;
    }
    if (chosen == nullptr) chosen = &mod;
  }
  if (chosen == nullptr) {
    return makeError(ErrorCode::NotFound, symbolFile.string() + " has no module matching " + module.path);
  }

  struct Entry {
    FunctionBounds bounds;
    FunctionData data;
  };
  std::vector<Entry> entries;
  const auto fnsIt = chosen->find("functions");
  if (fnsIt == chosen->end() || !fnsIt->is_array())
    return corrupt(symbolFile, "module has no functions array");
  entries.reserve(fnsIt->size());

  for (const Json& fn : *fnsIt) {
    if (!fn.is_object()) return corrupt(symbolFile, "function is not an object");
    Entry e;
    uint64_t start = 0, size = 0, line = 0;
    if (!getU64(fn, "start_rva", start) || !getU64(fn, "size", size)) {
      return corrupt(symbolFile, "function without start_rva / size");
    }
    (void)getU64(fn, "line", line);
    e.bounds.startRva = start;
    e.bounds.size = size;
    e.bounds.name = internName(m_strings, getString(fn, "name"));
    e.data.file = m_strings.intern(getString(fn, "file"));
    e.data.line = static_cast<uint32_t>(line);

    if (const auto lines = fn.find("lines"); lines != fn.end() && lines->is_array()) {
      e.data.lines.reserve(lines->size());
      for (const Json& l : *lines) {
        uint64_t rva = 0, lsize = 0, lline = 0;
        if (!getU64(l, "rva", rva) || !getU64(l, "size", lsize) || !getU64(l, "line", lline)) {
          return corrupt(symbolFile, "bad line range");
        }
        e.data.lines.push_back(
            LineRange{rva, static_cast<uint32_t>(lsize), e.data.file, static_cast<uint32_t>(lline)});
      }
      std::stable_sort(e.data.lines.begin(), e.data.lines.end(),
                       [](const LineRange& a, const LineRange& b) { return a.rva < b.rva; });
    }
    if (const auto inl = fn.find("inlines"); inl != fn.end() && inl->is_array()) {
      for (const Json& r : *inl) {
        InlineRange ir;
        const auto chain = r.find("chain");
        if (!getU64(r, "rva", ir.rva) || !getU64(r, "size", ir.size) || chain == r.end() ||
            !chain->is_array()) {
          return corrupt(symbolFile, "bad inline range");
        }
        for (const Json& c : *chain) {
          uint64_t cline = 0;
          (void)getU64(c, "line", cline);
          ir.chain.push_back(InlineFrame{internName(m_strings, getString(c, "function")),
                                         m_strings.intern(getString(c, "file")),
                                         static_cast<uint32_t>(cline)});
        }
        e.data.inlines.push_back(std::move(ir));
      }
      std::stable_sort(e.data.inlines.begin(), e.data.inlines.end(),
                       [](const InlineRange& a, const InlineRange& b) { return a.rva < b.rva; });
    }
    entries.push_back(std::move(e));
  }

  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& a, const Entry& b) { return a.bounds.startRva < b.bounds.startRva; });
  std::vector<FunctionBounds> bounds;
  bounds.reserve(entries.size());
  m_data.clear();
  m_data.reserve(entries.size());
  for (Entry& e : entries) {
    bounds.push_back(e.bounds);
    m_data.push_back(std::move(e.data));
  }
  m_table = FunctionRangeTable(std::move(bounds));  // already sorted: stable sort keeps the order of m_data
  return ok();
}

ResolvedAddress JsonSymbolProvider::resolve(uint64_t rva) {
  ResolvedAddress out;
  const size_t i = m_table.findIndex(rva);
  if (i == FunctionRangeTable::kNotFound) return out;
  const FunctionBounds& b = m_table.all()[i];
  const FunctionData& d = m_data[i];
  out.resolved = true;
  out.function = b.name;
  out.functionStartRva = b.startRva;
  if (const LineRange* l = findLineRange(d.lines, rva)) {
    out.file = l->file;
    out.line = l->line;
  } else {
    out.file = d.file;
    out.line = d.line;
  }
  // Inline ranges are sorted by rva; the innermost range starting at or before rva that still covers it.
  auto it = std::upper_bound(d.inlines.begin(), d.inlines.end(), rva,
                             [](uint64_t v, const InlineRange& r) { return v < r.rva; });
  while (it != d.inlines.begin()) {
    --it;
    if (rva - it->rva < it->size) {
      out.inlines = it->chain;
      break;
    }
  }
  return out;
}

Result<std::vector<LineRange>> JsonSymbolProvider::linesForFunction(uint64_t functionStartRva) {
  const size_t i = m_table.findExact(functionStartRva);
  if (i == FunctionRangeTable::kNotFound) {
    return makeError(ErrorCode::NotFound, "no function starts at the given rva");
  }
  return m_data[i].lines;
}

Result<FunctionBounds> JsonSymbolProvider::functionBounds(uint64_t rva) {
  const FunctionBounds* b = m_table.find(rva);
  if (b == nullptr) return makeError(ErrorCode::NotFound, "no function covers the given rva");
  return *b;
}

}  // namespace tachyon::symbols
