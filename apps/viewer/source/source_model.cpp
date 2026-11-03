#include "viewer/source/source_model.h"

#include "disasm/disassembler.h"
#include "symbols/symbol_service.h"
#include "viewer/common/function_names.h"
#include "viewer/paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace tachyon::viewer::source {

namespace fs = std::filesystem;
using symbols::FunctionKey;

// ---------------------------------------------------------------------------------------------------------
// Text helpers

namespace {

char lowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isContinuationByte(unsigned char c) {
  return (c & 0xC0u) == 0x80u;
}

// Number of bytes of the UTF-8 sequence that starts with `lead` and fits into the remaining `avail` bytes
// with valid continuation bytes; 1 for anything malformed.
size_t sequenceLength(const char* p, size_t avail) {
  const unsigned char lead = static_cast<unsigned char>(*p);
  size_t want = 1;
  if (lead >= 0xF0 && lead <= 0xF4)
    want = 4;
  else if (lead >= 0xE0)
    want = lead < 0xF0 ? 3 : 1;
  else if (lead >= 0xC2)
    want = lead < 0xE0 ? 2 : 1;
  if (want == 1 || want > avail) return 1;
  for (size_t i = 1; i < want; ++i)
    if (!isContinuationByte(static_cast<unsigned char>(p[i]))) return 1;
  return want;
}

}  // namespace

size_t findInsensitive(std::string_view haystack, std::string_view needle, size_t from) {
  if (needle.empty()) return from <= haystack.size() ? from : std::string_view::npos;
  if (needle.size() > haystack.size()) return std::string_view::npos;
  const char first = lowerAscii(needle[0]);
  const char firstUpper = first >= 'a' && first <= 'z' ? static_cast<char>(first - 'a' + 'A') : first;
  const size_t last = haystack.size() - needle.size();
  for (size_t i = from; i <= last; ++i) {
    const char c = haystack[i];
    if (c != first && c != firstUpper) continue;
    size_t k = 1;
    while (k < needle.size() && lowerAscii(haystack[i + k]) == lowerAscii(needle[k])) ++k;
    if (k == needle.size()) return i;
  }
  return std::string_view::npos;
}

std::string expandForDisplay(std::string_view line, size_t maxColumns) {
  std::string out;
  out.reserve(std::min<size_t>(line.size(), maxColumns) + 8);
  size_t col = 0;
  for (size_t i = 0; i < line.size() && col < maxColumns;) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if (c == '\t') {
      const size_t n = kTabWidth - col % kTabWidth;
      out.append(n, ' ');
      col += n;
      ++i;
    } else if (c < 0x20 || c == 0x7F) {
      out.push_back('.');
      ++col;
      ++i;
    } else if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++col;
      ++i;
    } else {
      out.push_back('?');  // the monospace UI font covers ASCII only; one cell per code point
      ++col;
      i += sequenceLength(line.data() + i, line.size() - i);
    }
  }
  return out;
}

uint32_t displayColumn(std::string_view line, size_t offset) {
  size_t col = 0;
  for (size_t i = 0; i < line.size() && i < offset;) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if (c == '\t') {
      col += kTabWidth - col % kTabWidth;
      ++i;
    } else if (c < 0x80) {
      ++col;
      ++i;
    } else {
      ++col;
      i += sequenceLength(line.data() + i, line.size() - i);
    }
  }
  return static_cast<uint32_t>(col);
}

float heatOf(uint64_t ticks, uint64_t maxTicks) {
  if (ticks == 0 || maxTicks == 0) return 0.0f;
  if (ticks >= maxTicks) return 1.0f;
  return static_cast<float>(std::log1p(static_cast<double>(ticks)) /
                            std::log1p(static_cast<double>(maxTicks)));
}

// ---------------------------------------------------------------------------------------------------------
// SourceText

Result<std::shared_ptr<const SourceText>> SourceText::parse(std::string bytes) {
  if (bytes.size() > kMaxSourceFileBytes)
    return makeError(ErrorCode::InvalidArgument, "file is larger than 16 MiB");
  if (std::memchr(bytes.data(), 0, bytes.size()) != nullptr)
    return makeError(ErrorCode::CorruptData, "file looks binary (contains NUL bytes)");
  std::shared_ptr<SourceText> t(new SourceText());
  size_t begin = 0;
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    begin = 3;
  t->m_text = std::move(bytes);
  const std::string& s = t->m_text;
  const size_t n = s.size();
  size_t i = begin;
  uint32_t maxCols = 0;
  while (i < n) {
    const size_t start = i;
    uint32_t col = 0;
    while (i < n && s[i] != '\n' && s[i] != '\r') {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == '\t')
        col += kTabWidth - col % kTabWidth;
      else if (!isContinuationByte(c))
        ++col;
      ++i;
    }
    t->m_lineStart.push_back(static_cast<uint32_t>(start));
    t->m_lineEnd.push_back(static_cast<uint32_t>(i));
    maxCols = std::max(maxCols, col);
    if (t->m_lineEnd.size() > kMaxSourceLines)
      return makeError(ErrorCode::InvalidArgument, "file has more than 2 million lines");
    if (i < n) {
      if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') ++i;
      ++i;
    }
  }
  t->m_lineStart.push_back(static_cast<uint32_t>(n));  // sentinel
  t->m_maxColumns = maxCols;
  return std::shared_ptr<const SourceText>(std::move(t));
}

Result<std::shared_ptr<const SourceText>> SourceText::load(const fs::path& path, analysis::Cancel cancel) {
  std::error_code ec;
  const uintmax_t size = fs::file_size(path, ec);
  if (ec) return makeError(ErrorCode::NotFound, "cannot stat " + pathToUtf8(path) + ": " + ec.message());
  if (size > kMaxSourceFileBytes)
    return makeError(ErrorCode::InvalidArgument,
                     "file is larger than 16 MiB (" + std::to_string(size) + " bytes)");
  std::ifstream in(path, std::ios::binary);
  if (!in) return makeError(ErrorCode::IoError, "cannot open " + pathToUtf8(path));
  std::string bytes;
  bytes.resize(static_cast<size_t>(size));
  constexpr size_t kChunk = size_t{1} << 20;
  size_t done = 0;
  while (done < bytes.size()) {
    if (cancel.requested()) return makeError(ErrorCode::Cancelled, "cancelled");
    const size_t want = std::min(kChunk, bytes.size() - done);
    in.read(bytes.data() + done, static_cast<std::streamsize>(want));
    const size_t got = static_cast<size_t>(in.gcount());
    done += got;
    if (got < want) break;
  }
  bytes.resize(done);
  return parse(std::move(bytes));
}

std::string_view SourceText::line(uint32_t number) const {
  if (number < 1 || number > lineCount()) return {};
  const uint32_t b = m_lineStart[number - 1], e = m_lineEnd[number - 1];
  return std::string_view(m_text).substr(b, e - b);
}

uint32_t SourceText::findNext(std::string_view needle, uint32_t from) const {
  if (needle.empty() || lineCount() == 0) return 0;
  if (needle.find_first_of("\r\n") != std::string_view::npos) return 0;
  if (from < 1) from = 1;
  if (from > lineCount()) return 0;
  const std::string_view all(m_text);
  size_t pos = m_lineStart[from - 1];
  while (true) {
    pos = findInsensitive(all, needle, pos);
    if (pos == std::string_view::npos) return 0;
    // The line holding `pos`: the last start <= pos (the sentinel is excluded).
    const auto it = std::upper_bound(m_lineStart.begin(), m_lineStart.end() - 1, static_cast<uint32_t>(pos));
    const uint32_t number = static_cast<uint32_t>(it - m_lineStart.begin());  // 1-based
    if (number >= 1 && pos + needle.size() <= m_lineEnd[number - 1]) return number;
    // A hit in a terminator gap or a BOM cannot happen for a needle without line breaks, but a match that
    // would run past its line end is skipped.
    ++pos;
  }
}

uint32_t SourceText::findPrev(std::string_view needle, uint32_t from) const {
  if (needle.empty() || lineCount() == 0) return 0;
  if (from > lineCount()) from = lineCount();
  for (uint32_t l = from; l >= 1; --l) {
    if (findInsensitive(line(l), needle) != std::string_view::npos) return l;
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------------------
// SourceView

std::vector<uint32_t> SourceView::instructionsOfLine(uint32_t line) const {
  std::vector<uint32_t> out;
  if (line + 1 >= lineInstrBegin.size()) return out;
  out.assign(lineInstrOrder.begin() + lineInstrBegin[line],
             lineInstrOrder.begin() + lineInstrBegin[line + 1]);
  return out;
}

// ---------------------------------------------------------------------------------------------------------
// Remap derivation

namespace {

struct Comp {
  size_t begin, end;  // byte range of the component in the original string
};

bool isSepChar(char c) {
  return c == '/' || c == '\\';
}

std::vector<Comp> splitComps(std::string_view s) {
  std::vector<Comp> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isSepChar(s[i])) ++i;
    const size_t b = i;
    while (i < s.size() && !isSepChar(s[i])) ++i;
    if (i > b) out.push_back({b, i});
  }
  return out;
}

bool looksWindowsPath(std::string_view s) {
  return (s.size() >= 2 && s[1] == ':') || s.find('\\') != std::string_view::npos;
}

struct Derived {
  size_t common = 0;       // trailing components shared
  std::string fromPrefix;  // debug path without the shared tail
  std::string toPrefix;    // chosen path without the shared tail
};

Derived derive(std::string_view debugPath, const std::string& chosen) {
  Derived d;
  const std::vector<Comp> a = splitComps(debugPath), b = splitComps(chosen);
  const bool ignoreCase = looksWindowsPath(debugPath) || looksWindowsPath(chosen);
  const auto same = [&](std::string_view x, std::string_view y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
      if (ignoreCase ? lowerAscii(x[i]) != lowerAscii(y[i]) : x[i] != y[i]) return false;
    return true;
  };
  size_t k = 0;
  while (k < a.size() && k < b.size()) {
    const Comp& ca = a[a.size() - 1 - k];
    const Comp& cb = b[b.size() - 1 - k];
    if (!same(debugPath.substr(ca.begin, ca.end - ca.begin),
              std::string_view(chosen).substr(cb.begin, cb.end - cb.begin)))
      break;
    ++k;
  }
  d.common = k;
  const auto prefixOf = [](std::string_view s, const std::vector<Comp>& comps, size_t keep) {
    if (keep == 0) {
      // Nothing is left of the path but its root: "/" of an absolute POSIX path, nothing for a relative one.
      return !comps.empty() && comps[0].begin > 0 ? std::string(s.substr(0, comps[0].begin)) : std::string();
    }
    std::string p(s.substr(0, comps[keep - 1].end));
    if (p.size() == 2 && p[1] == ':') p.push_back('\\');  // a bare drive designator
    return p;
  };
  if (k == 0) {
    d.fromPrefix = std::string(debugPath);  // no shared tail: map the whole path
    d.toPrefix = chosen;
    return d;
  }
  d.fromPrefix = prefixOf(debugPath, a, a.size() - k);
  d.toPrefix = prefixOf(chosen, b, b.size() - k);
  return d;
}

}  // namespace

std::optional<symbols::SourceRemapRule> deriveRemapRule(std::string_view debugPath, const fs::path& chosen) {
  const std::string chosenUtf8 = pathToUtf8(chosen);
  const Derived d = derive(debugPath, chosenUtf8);
  if (d.fromPrefix.empty() || d.toPrefix.empty()) return std::nullopt;
  return symbols::SourceRemapRule{d.fromPrefix, d.toPrefix};
}

LocateOutcome locateOutcome(std::string_view debugPath, const fs::path& chosen, bool chosenIsDirectory) {
  LocateOutcome out;
  if (chosenIsDirectory) {
    out.root = chosen;
    return out;
  }
  const std::string chosenUtf8 = pathToUtf8(chosen);
  const Derived d = derive(debugPath, chosenUtf8);
  if (!d.fromPrefix.empty() && !d.toPrefix.empty()) {
    out.rule = symbols::SourceRemapRule{d.fromPrefix, d.toPrefix};
  } else if (!d.toPrefix.empty()) {
    out.root = pathFromUtf8(d.toPrefix);  // the debug path is just the shared tail: search under the parent
  } else {
    out.root = chosen.parent_path();
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------
// buildSourceView

namespace {

std::string hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

Result<std::shared_ptr<const SourceView>> cancelledResult() {
  return makeError(ErrorCode::Cancelled, "cancelled");
}

std::shared_ptr<const SourceView> messageView(std::shared_ptr<SourceView> v, std::string message) {
  v->kind = ViewKind::Message;
  v->message = std::move(message);
  return v;
}

// The line a frame at `rva` belongs to, by the rules of analysis::detail::FunctionMatcher::lineOf (own file
// of the function; call site of an inline chain; nearest preceding own-file row for code from other files).
uint32_t lineOfAddress(const symbols::ResolvedAddress& ra, StringIdx ownFile, bool haveFile,
                       const std::vector<symbols::LineRange>& ownRows, uint64_t rva) {
  if (!ra.resolved) return 0;
  if (!ra.inlines.empty() && ra.callSiteLine != 0 && ra.callSiteFile.valid()) {
    if (!haveFile || ra.callSiteFile == ownFile) return ra.callSiteLine;
  }
  if (!ra.file.valid()) return 0;
  if (!haveFile || ra.file == ownFile) return ra.line;
  if (ownRows.empty()) return 0;
  auto it = std::upper_bound(ownRows.begin(), ownRows.end(), rva,
                             [](uint64_t v, const symbols::LineRange& l) { return v < l.rva; });
  if (it != ownRows.begin()) --it;
  return it->line;
}

}  // namespace

Result<std::shared_ptr<const SourceView>> buildSourceView(const analysis::QueryContext& ctx,
                                                          const BuildInputs& in, analysis::Cancel cancel) {
  auto v = std::make_shared<SourceView>();
  v->key = in.key;
  FunctionKey key = in.key;
  const model::TraceModel& model = ctx.model();
  const symbols::SymbolService& svc = ctx.symbols();

  const common::NameParts names = common::splitFunctionName(&ctx, key);
  v->functionName = names.function;
  v->moduleName = names.module;

  // ---- keys that have nothing to show ----
  if (key.isScope()) {
    return messageView(v, "\"" + names.function +
                              "\" is an instrumentation scope; scopes have no source code "
                              "or disassembly.");
  }
  if (key == analysis::kRootKey || key == analysis::kKernelKey) {
    return messageView(v, key == analysis::kRootKey ? "The call graph root has no source code."
                                                    : "Merged kernel frames have no source code.");
  }
  if (!key.module.valid()) {
    return messageView(
        v, "Address " + hex(key.value) + " is not inside any loaded module; there is nothing to show.");
  }
  if (key.module.get() >= model.modules.size()) return messageView(v, "Unknown module for this function.");
  if (key.isInline()) {
    return messageView(v, "\"" + names.function +
                              "\" is an inlined function and has no code of its own; select "
                              "the function that contains it.");
  }
  const model::Module& module = model.modules[key.module.get()];

  analysis::Filter filter = in.filter;
  filter.subtreeRoot.reset();
  filter.function.reset();

  symbols::ISymbolProvider* prov = svc.provider(key.module);
  const symbols::SymbolStringTable& strings = svc.symbolStrings();

  symbols::ResolvedAddress entry;
  bool resolved = false;
  if (prov != nullptr) {
    entry = prov->resolve(key.value);
    resolved = entry.resolved;
    if (resolved && entry.functionStartRva != key.value) {
      // A key from before the module resolved ({module, rva} of a frame): its function is known now.
      key = FunctionKey{key.module, entry.functionStartRva};
      v->key = key;
      entry = prov->resolve(key.value);
    }
  }
  v->resolved = resolved;

  // ---- the function's range and own lines ----
  uint64_t startRva = key.value, sizeBytes = kUnresolvedWindowBytes;
  std::vector<symbols::LineRange> ownRows;  // rows of the function in its own file, sorted by rva
  StringIdx ownFile;
  bool haveOwnFile = false;
  if (resolved) {
    ownFile = entry.file;
    haveOwnFile = entry.file.valid();
    v->debugPath = std::string(strings.get(entry.file));
    if (Result<symbols::FunctionBounds> b = prov->functionBounds(key.value); b.ok() && b->size > 0) {
      startRva = b->startRva;
      sizeBytes = b->size;
    } else {
      v->note = "The symbol file gives no size for this function; showing the first " +
                std::to_string(kUnresolvedWindowBytes) + " bytes.";
    }
    if (Result<std::vector<symbols::LineRange>> rows = prov->linesForFunction(key.value); rows.ok()) {
      bool outside = false;
      for (const symbols::LineRange& r : *rows) {
        if (r.rva < startRva || r.rva >= startRva + sizeBytes) outside = true;
        if (r.line != 0 && (!haveOwnFile || r.file == ownFile)) ownRows.push_back(r);
      }
      std::stable_sort(
          ownRows.begin(), ownRows.end(),
          [](const symbols::LineRange& a, const symbols::LineRange& b) { return a.rva < b.rva; });
      if (outside)
        v->note =
            "Split function: its code is in several ranges; showing the piece that contains the entry "
            "point (" +
            hex(startRva) + ", " + std::to_string(sizeBytes) + " bytes).";
    }
    // The function's extent in its file: from the line of its entry to the last row at or after it. Rows
    // above the entry are inlined callees defined earlier in the same file, not part of the function's text.
    if (entry.line != 0) {
      v->firstLine = entry.line;
      v->lastLine = entry.line;
      for (const symbols::LineRange& r : ownRows)
        if (r.line >= v->firstLine) v->lastLine = std::max(v->lastLine, r.line);
    } else if (!ownRows.empty()) {
      v->firstLine = UINT32_MAX;
      for (const symbols::LineRange& r : ownRows) {
        v->firstLine = std::min(v->firstLine, r.line);
        v->lastLine = std::max(v->lastLine, r.line);
      }
    }
  } else {
    v->note = "No symbols for this address (" + names.full + "); showing " +
              std::to_string(kUnresolvedWindowBytes) + " bytes from " + hex(key.value) + ".";
  }
  if (cancel.requested()) return cancelledResult();

  // ---- queries ----
  std::shared_ptr<const std::vector<analysis::LineAgg>> lineAggs;
  if (resolved) {
    auto r = analysis::linesForFunctionShared(ctx, filter, key, cancel);
    if (!r.ok()) {
      if (r.error().code == ErrorCode::Cancelled) return cancelledResult();
      return messageView(v, "Line query failed: " + r.error().toString());
    }
    lineAggs = *r;
  }
  auto ia = analysis::instructionsForFunctionShared(ctx, filter, key, cancel);
  if (!ia.ok()) {
    if (ia.error().code == ErrorCode::Cancelled) return cancelledResult();
    return messageView(v, "Instruction query failed: " + ia.error().toString());
  }
  const std::vector<analysis::InstructionAgg>& instrAggs = **ia;

  // ---- source file ----
  if (resolved) {
    if (v->debugPath.empty()) {
      v->sourceProblem = "The symbol file has no source file information for this function.";
    } else if (!in.locator) {
      v->sourceProblem = "Source file not found: " + v->debugPath;
    } else {
      Result<fs::path> located = in.locator->locate(v->debugPath);
      if (!located.ok()) {
        v->sourceProblem = "Source file not found: " + v->debugPath;
      } else {
        Result<std::shared_ptr<const SourceText>> text = SourceText::load(*located, cancel);
        if (!text.ok()) {
          if (text.error().code == ErrorCode::Cancelled) return cancelledResult();
          v->sourceProblem = "Cannot show " + pathToUtf8(*located) + ": " + text.error().message;
          v->localPath = pathToUtf8(*located);
        } else {
          v->file = *text;
          v->localPath = pathToUtf8(*located);
        }
      }
    }
  } else {
    v->sourceProblem = "No symbols for this address, so there is no source information.";
  }
  if (cancel.requested()) return cancelledResult();

  // ---- line times ----
  if (lineAggs) {
    const uint32_t count = v->file ? v->file->lineCount() : 0;
    if (v->file) v->lines.assign(static_cast<size_t>(count) + 1, LineTime{});
    uint64_t best = 0;
    for (const analysis::LineAgg& a : *lineAggs) {
      v->total.ticks += a.ticks;
      v->total.running += a.runningTicks;
      v->total.ready += a.readyTicks;
      v->total.waiting += a.waitingTicks;
      if (a.line == 0) {
        v->unknownLineTicks += a.ticks;
      } else if (a.line > count) {
        v->pastEndTicks += a.ticks;
      } else {
        v->lines[a.line] = LineTime{a.ticks, a.runningTicks, a.readyTicks, a.waitingTicks};
        if (a.ticks > best) {
          best = a.ticks;
          v->hottestLine = a.line;
        }
      }
    }
  }

  // ---- disassembly ----
  {
    const uint64_t want = std::min(sizeBytes, kMaxDisassemblyBytes);
    if (sizeBytes > kMaxDisassemblyBytes) {
      v->note += (v->note.empty() ? "" : "  ") + std::string(
                                                     "The function is larger than 1 MiB; only its first "
                                                     "1 MiB is disassembled.");
    }
    Result<std::vector<uint8_t>> bytes =
        disasm::readModuleBytes(module, startRva, static_cast<size_t>(want), in.moduleImageOverride);
    if (!bytes.ok()) {
      v->instructionsProblem = "Cannot read the code of " +
                               (module.path.empty() ? std::string("the module") : module.path) + ": " +
                               bytes.error().message;
    } else if (bytes->empty()) {
      v->instructionsProblem = "The module image has no code at " + hex(startRva) + ".";
    } else {
      const auto symbolize = [&](uint64_t target) -> std::string {
        if (prov == nullptr) return {};
        const symbols::ResolvedAddress ra = prov->resolve(target);
        if (!ra.resolved) return {};
        std::string name(strings.get(ra.function));
        if (name.empty()) return {};
        if (target != ra.functionStartRva) name += "+" + hex(target - ra.functionStartRva);
        return name;
      };
      const std::vector<disasm::Instruction> decoded = disasm::disassemble(*bytes, startRva, 64, symbolize);
      v->haveInstructions = true;
      v->startRva = startRva;
      v->sizeBytes = bytes->size();
      v->instructions.reserve(decoded.size());
      for (size_t i = 0; i < decoded.size(); ++i) {
        if ((i & 255u) == 0 && cancel.requested()) return cancelledResult();
        const disasm::Instruction& d = decoded[i];
        InstructionRow row;
        row.rva = d.rva;
        row.length = d.length;
        row.bytes = d.bytes;
        row.text = d.text;
        row.isCall = d.isCall;
        row.valid = d.valid;
        row.afterCall = i > 0 && decoded[i - 1].isCall;
        if (resolved && prov != nullptr)
          row.line = lineOfAddress(prov->resolve(d.rva), ownFile, haveOwnFile, ownRows, d.rva);
        v->instructions.push_back(std::move(row));
      }
      // Times: both lists are sorted by address; an address inside an instruction belongs to it.
      size_t at = 0;
      for (const analysis::InstructionAgg& a : instrAggs) {
        while (at < v->instructions.size() && v->instructions[at].rva + v->instructions[at].length <= a.rva)
          ++at;
        if (at < v->instructions.size() && a.rva >= v->instructions[at].rva) {
          v->instructions[at].ticks += a.ticks;
          v->instructionTicks += a.ticks;
        } else {
          v->otherTicks += a.ticks;
        }
      }
      uint64_t best = 0;
      for (size_t i = 0; i < v->instructions.size(); ++i) {
        if (v->instructions[i].ticks > best) {
          best = v->instructions[i].ticks;
          v->hottestInstruction = static_cast<uint32_t>(i);
        }
      }
    }
  }
  if (v->haveInstructions == false) {
    for (const analysis::InstructionAgg& a : instrAggs) v->otherTicks += a.ticks;
  }

  // ---- mixed mode: instructions grouped by source line ----
  if (v->file && v->haveInstructions) {
    const uint32_t count = v->file->lineCount();
    v->lineInstrBegin.assign(static_cast<size_t>(count) + 2, 0);
    for (const InstructionRow& r : v->instructions)
      if (r.line >= 1 && r.line <= count) ++v->lineInstrBegin[r.line + 1];
    for (size_t i = 1; i < v->lineInstrBegin.size(); ++i) v->lineInstrBegin[i] += v->lineInstrBegin[i - 1];
    v->lineInstrOrder.assign(v->lineInstrBegin.back(), 0);
    std::vector<uint32_t> fill(v->lineInstrBegin.begin(), v->lineInstrBegin.end() - 1);
    for (size_t i = 0; i < v->instructions.size(); ++i) {
      const uint32_t l = v->instructions[i].line;
      if (l >= 1 && l <= count) v->lineInstrOrder[fill[l]++] = static_cast<uint32_t>(i);
    }
  }

  v->kind = v->file ? ViewKind::Source : ViewKind::Disassembly;
  return std::shared_ptr<const SourceView>(std::move(v));
}

}  // namespace tachyon::viewer::source
