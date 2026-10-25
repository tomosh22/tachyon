#include "scenario.h"

#include "prng.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace tachyon::tracegen {

using json = nlohmann::json;

int64_t findFunction(const ModuleSpec& m, uint64_t rva) {
  size_t lo = 0, hi = m.functions.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (m.functions[mid].rva <= rva)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0) return -1;
  const FunctionSpec& f = m.functions[lo - 1];
  return rva < f.rva + f.size ? static_cast<int64_t>(lo - 1) : -1;
}

namespace {

constexpr uint64_t kMaxUs = 1'000'000'000'000ull;

uint64_t alignUp(uint64_t v, uint64_t a) {
  return (v + a - 1) / a * a;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

class Parser {
 public:
  Parser(const json& doc, std::string_view fallback) : m_doc(doc), m_fallback(fallback) {}

  Result<Scenario> run() {
    if (!m_doc.is_object()) return makeError(ErrorCode::InvalidArgument, "scenario must be a JSON object");
    parseHeader();
    if (ok()) parseModules();
    if (ok()) parseStacks();
    if (ok()) parseThreads();
    if (ok()) parseScopes();
    if (ok()) parseFiberRegistrations();
    if (ok()) parseFrameMarks();
    if (ok()) parseMarkers();
    if (ok()) parsePlots();
    if (ok()) parseRenames();
    if (ok()) validate();
    if (!ok()) return makeError(ErrorCode::InvalidArgument, "scenario '" + m_s.name + "': " + m_err);
    return std::move(m_s);
  }

 private:
  bool ok() const { return m_err.empty(); }
  void fail(const std::string& where, const std::string& msg) {
    if (m_err.empty()) m_err = where + ": " + msg;
  }

  const json* find(const json& o, const char* key) const {
    if (!o.is_object()) return nullptr;
    const auto it = o.find(key);
    return it == o.end() ? nullptr : &*it;
  }

  uint64_t asU64(const json& v, const std::string& where) {
    if (v.is_number_unsigned()) return v.get<uint64_t>();
    if (v.is_number_integer()) {
      const int64_t i = v.get<int64_t>();
      if (i >= 0) return static_cast<uint64_t>(i);
    } else if (v.is_string()) {
      const std::string str = v.get<std::string>();
      char* end = nullptr;
      const unsigned long long r = std::strtoull(str.c_str(), &end, 0);
      if (!str.empty() && end && *end == '\0') return r;
    }
    fail(where, "expected a non-negative integer (or \"0x..\" string)");
    return 0;
  }

  uint64_t u64(const json& o, const char* key, uint64_t def, const std::string& where) {
    const json* v = find(o, key);
    return v ? asU64(*v, where + "." + key) : def;
  }
  uint64_t reqU64(const json& o, const char* key, const std::string& where) {
    const json* v = find(o, key);
    if (!v) {
      fail(where, std::string("missing required key '") + key + "'");
      return 0;
    }
    return asU64(*v, where + "." + key);
  }
  std::string str(const json& o, const char* key, const std::string& def, const std::string& where) {
    const json* v = find(o, key);
    if (!v) return def;
    if (!v->is_string()) {
      fail(where + "." + key, "expected a string");
      return def;
    }
    return v->get<std::string>();
  }
  std::string reqStr(const json& o, const char* key, const std::string& where) {
    if (!find(o, key)) {
      fail(where, std::string("missing required key '") + key + "'");
      return {};
    }
    return str(o, key, "", where);
  }
  bool boolean(const json& o, const char* key, bool def, const std::string& where) {
    const json* v = find(o, key);
    if (!v) return def;
    if (!v->is_boolean()) {
      fail(where + "." + key, "expected a boolean");
      return def;
    }
    return v->get<bool>();
  }
  const json* array(const json& o, const char* key, const std::string& where) {
    const json* v = find(o, key);
    if (!v) return nullptr;
    if (!v->is_array()) {
      fail(where + "." + key, "expected an array");
      return nullptr;
    }
    return v;
  }

  // Microseconds -> ticks, exact for any clock frequency.
  uint64_t toTicks(uint64_t us) const {
    return (us / 1'000'000) * m_s.clockHz + (us % 1'000'000) * m_s.clockHz / 1'000'000;
  }
  uint64_t us(const json& o, const char* key, uint64_t def, const std::string& where) {
    const json* v = find(o, key);
    if (!v) return toTicks(def);
    const uint64_t x = asU64(*v, where + "." + key);
    if (x > kMaxUs) {
      fail(where + "." + key, "microsecond value too large");
      return 0;
    }
    return toTicks(x);
  }
  uint64_t reqUs(const json& o, const char* key, const std::string& where) {
    if (!find(o, key)) {
      fail(where, std::string("missing required key '") + key + "'");
      return 0;
    }
    return us(o, key, 0, where);
  }
  std::pair<uint64_t, uint64_t> usRange(const json& o, const char* key, const std::string& where) {
    const json* v = find(o, key);
    if (!v) {
      fail(where, std::string("missing required key '") + key + "'");
      return {0, 0};
    }
    if (!v->is_array() || v->size() != 2) {
      fail(where + "." + key, "expected [lo, hi]");
      return {0, 0};
    }
    const uint64_t lo = asU64((*v)[0], where + "." + key), hi = asU64((*v)[1], where + "." + key);
    if (lo > hi || hi > kMaxUs) {
      fail(where + "." + key, "invalid range");
      return {0, 0};
    }
    return {toTicks(lo), toTicks(hi)};
  }

  // ---- header
  // ----------------------------------------------------------------------------------------------
  void parseHeader() {
    const json& d = m_doc;
    const std::string w = "scenario";
    m_s.name = str(d, "name", std::string(m_fallback), w);
    m_s.seed = u64(d, "seed", 1, w);
    const std::string plat = str(d, "platform", "windows", w);
    if (plat == "windows")
      m_s.platform = 1;
    else if (plat == "linux")
      m_s.platform = 2;
    else
      fail(w + ".platform", "must be \"windows\" or \"linux\"");
    m_s.clockHz = u64(d, "clock_frequency_hz", 10'000'000, w);
    m_s.samplingHz = static_cast<uint32_t>(u64(d, "sampling_hz", 8000, w));
    if (m_s.clockHz == 0 || m_s.samplingHz == 0 || m_s.clockHz % m_s.samplingHz != 0)
      fail(w, "clock_frequency_hz must be a positive multiple of sampling_hz");
    if (!ok()) return;
    m_s.sampleInterval = m_s.clockHz / m_s.samplingHz;
    m_s.startTs = u64(d, "start_ts", 1'000'000, w);
    if (find(d, "duration_us"))
      m_s.duration = us(d, "duration_us", 0, w);
    else if (find(d, "duration_ms"))
      m_s.duration = toTicks(u64(d, "duration_ms", 0, w) * 1000);
    else if (find(d, "duration_s"))
      m_s.duration = toTicks(u64(d, "duration_s", 0, w) * 1'000'000);
    else
      fail(w, "one of duration_s / duration_ms / duration_us is required");
    if (const json* p = find(d, "partial")) {
      if (p->is_boolean()) {
        m_s.partial = p->get<bool>();
      } else if (p->is_object()) {
        m_s.partial = true;
        m_s.tornTail = boolean(*p, "torn_tail", false, w + ".partial");
        if (find(*p, "crash_at_us")) m_s.duration = us(*p, "crash_at_us", 0, w + ".partial");
        if (find(*p, "crash_at_ms")) m_s.duration = toTicks(u64(*p, "crash_at_ms", 0, w + ".partial") * 1000);
      } else {
        fail(w + ".partial", "expected boolean or object");
      }
    }
    if (m_s.duration == 0) fail(w, "duration must be > 0");
    m_s.chunkRecords = u64(d, "chunk_records", 1'000'000, w);
    if (m_s.chunkRecords == 0 || m_s.chunkRecords > 1'048'576)
      fail(w + ".chunk_records", "must be in 1..1048576");
    m_s.eventsLost = u64(d, "events_lost", 0, w);
    m_s.detail = boolean(d, "detail", false, w);
    m_s.blend = boolean(d, "blend", false, w);
    if (const json* p = find(d, "process")) {
      m_s.pid = static_cast<uint32_t>(u64(*p, "pid", 1000, "process"));
      m_s.parentPid = static_cast<uint32_t>(u64(*p, "parent_pid", 1, "process"));
      m_s.processName = str(*p, "name", "game", "process");
      m_s.commandLine = str(*p, "command_line", m_s.processName, "process");
    } else {
      m_s.pid = 1000;
      m_s.parentPid = 1;
      m_s.processName = "game";
      m_s.commandLine = "game";
    }
  }

  // ---- modules
  // ---------------------------------------------------------------------------------------------
  void fillDefaultLines(FunctionSpec& f) {
    if (!f.lines.empty()) return;
    const uint32_t k = std::clamp<uint32_t>(f.size / 32, 1, 8);
    const uint32_t step = f.size / k;
    for (uint32_t j = 0; j < k; ++j) {
      LineSpec l;
      l.rva = f.rva + static_cast<uint64_t>(j) * step;
      l.size = (j + 1 == k) ? f.size - j * step : step;
      l.line = f.line + j * 2;
      f.lines.push_back(l);
    }
  }

  void parseFunctionExplicit(const json& fj, ModuleSpec& m, uint64_t& nextRva, const std::string& w) {
    FunctionSpec f;
    f.name = reqStr(fj, "name", w);
    f.rva = u64(fj, "rva", nextRva, w);
    f.size = static_cast<uint32_t>(u64(fj, "size", 0x100, w));
    f.file = str(fj, "file", f.name + ".cpp", w);
    f.line = static_cast<uint32_t>(u64(fj, "line", 1, w));
    if (f.size == 0) fail(w, "function size must be > 0");
    if (const json* ls = array(fj, "lines", w)) {
      for (size_t i = 0; i < ls->size() && ok(); ++i) {
        const json& lj = (*ls)[i];
        const std::string lw = w + ".lines[" + std::to_string(i) + "]";
        LineSpec l;
        l.rva = f.rva + u64(lj, "rva_offset", 0, lw);
        l.size = static_cast<uint32_t>(reqU64(lj, "size", lw));
        l.line = static_cast<uint32_t>(reqU64(lj, "line", lw));
        f.lines.push_back(l);
      }
    }
    if (const json* is = array(fj, "inlines", w)) {
      for (size_t i = 0; i < is->size() && ok(); ++i) {
        const json& ij = (*is)[i];
        const std::string iw = w + ".inlines[" + std::to_string(i) + "]";
        InlineSpec in;
        in.rva = f.rva + u64(ij, "rva_offset", 0, iw);
        in.size = static_cast<uint32_t>(reqU64(ij, "size", iw));
        if (const json* cs = array(ij, "chain", iw)) {
          for (const json& cj : *cs) {
            InlineFrameSpec fr;
            fr.function = reqStr(cj, "function", iw);
            fr.file = str(cj, "file", f.file, iw);
            fr.line = static_cast<uint32_t>(u64(cj, "line", 1, iw));
            in.chain.push_back(std::move(fr));
          }
        }
        if (in.chain.empty()) fail(iw, "inline chain must not be empty");
        f.inlines.push_back(std::move(in));
      }
    }
    nextRva = alignUp(f.rva + f.size, 16);
    m.functions.push_back(std::move(f));
  }

  void parseFunctionsGenerated(const json& g, uint32_t moduleIdx, ModuleSpec& m, uint64_t& nextRva,
                               const std::string& w) {
    const uint64_t count = u64(g, "count", 0, w);
    const uint64_t minSize = u64(g, "min_size", 64, w), maxSize = u64(g, "max_size", 1024, w);
    const std::string prefix = str(g, "name_prefix", m.name + "::fn", w);
    const uint64_t files = std::max<uint64_t>(1, u64(g, "files", 4, w));
    const uint64_t inlinePct = u64(g, "inline_pct", 0, w);
    if (minSize < 16 || maxSize < minSize) fail(w, "invalid min_size/max_size");
    if (count > 100000) fail(w, "too many generated functions");
    if (!ok()) return;
    Prng rng(streamSeed(m_s.seed, "functions", moduleIdx));
    for (uint64_t i = 0; i < count; ++i) {
      FunctionSpec f;
      f.name = prefix + std::to_string(i);
      f.rva = nextRva;
      f.size = static_cast<uint32_t>(alignUp(rng.range(minSize, maxSize), 16));
      f.file = "src/" + m.name + "/file" + std::to_string(i % files) + ".cpp";
      f.line = static_cast<uint32_t>(10 + (i * 37) % 900);
      if (rng.below(100) < inlinePct) {
        const uint64_t n = rng.range(1, 3);
        for (uint64_t k = 0; k < n; ++k) {
          InlineSpec in;
          in.size = static_cast<uint32_t>(rng.range(8, std::min<uint64_t>(64, f.size / 2)));
          in.rva = f.rva + rng.below(f.size - in.size + 1) / 4 * 4;
          const uint64_t depth = rng.range(1, 2);
          for (uint64_t c = 0; c < depth; ++c) {
            InlineFrameSpec fr;
            fr.function = f.name + "::inl" + std::to_string(k) + "_" + std::to_string(c);
            fr.file = "src/" + m.name + "/inline" + std::to_string(rng.below(files)) + ".h";
            fr.line = static_cast<uint32_t>(rng.range(1, 400));
            in.chain.push_back(std::move(fr));
          }
          f.inlines.push_back(std::move(in));
        }
      }
      nextRva = alignUp(f.rva + f.size, 16);
      m.functions.push_back(std::move(f));
    }
  }

  void parseModules() {
    const json* mods = array(m_doc, "modules", "scenario");
    if (!mods) return;
    for (size_t mi = 0; mi < mods->size() && ok(); ++mi) {
      const json& mj = (*mods)[mi];
      const std::string w = "modules[" + std::to_string(mi) + "]";
      ModuleSpec m;
      m.name = reqStr(mj, "name", w);
      m.path = str(mj, "path", m.name, w);
      m.base =
          u64(mj, "base", (m_s.platform == 1 ? 0x140000000ull : 0x7f0000000000ull) + mi * 0x10000000ull, w);
      const std::string kind = str(mj, "id_kind", m_s.platform == 1 ? "pdb" : "gnu", w);
      m.idKind = kind == "pdb" ? 1 : 2;
      if (kind != "pdb" && kind != "gnu") fail(w + ".id_kind", "must be \"pdb\" or \"gnu\"");
      const size_t idLen = m.idKind == 1 ? 16 : 20;
      const std::string idHex = str(mj, "id", "", w);
      if (idHex.empty()) {
        uint64_t st = streamSeed(m_s.seed, "module-id", mi);
        for (size_t i = 0; i < idLen; ++i) m.id.push_back(static_cast<uint8_t>(splitmix64(st) >> 24));
      } else {
        if (idHex.size() % 2 != 0 || idHex.size() / 2 > 32) fail(w + ".id", "invalid hex id");
        for (size_t i = 0; i + 1 < idHex.size() && ok(); i += 2) {
          char* end = nullptr;
          const std::string byte = idHex.substr(i, 2);
          const unsigned long v = std::strtoul(byte.c_str(), &end, 16);
          if (*end != '\0') fail(w + ".id", "invalid hex id");
          m.id.push_back(static_cast<uint8_t>(v));
        }
      }
      m.age = static_cast<uint32_t>(u64(mj, "age", m.idKind == 1 ? 1 : 0, w));
      uint64_t nextRva = 0x1000;
      if (const json* fs = array(mj, "functions", w)) {
        for (size_t fi = 0; fi < fs->size() && ok(); ++fi)
          parseFunctionExplicit((*fs)[fi], m, nextRva, w + ".functions[" + std::to_string(fi) + "]");
      }
      if (const json* g = find(mj, "functions_generated"))
        parseFunctionsGenerated(*g, static_cast<uint32_t>(mi), m, nextRva, w + ".functions_generated");
      if (!ok()) return;
      std::stable_sort(m.functions.begin(), m.functions.end(),
                       [](const FunctionSpec& a, const FunctionSpec& b) { return a.rva < b.rva; });
      for (size_t i = 0; i + 1 < m.functions.size(); ++i)
        if (m.functions[i].rva + m.functions[i].size > m.functions[i + 1].rva)
          fail(w, "functions '" + m.functions[i].name + "' and '" + m.functions[i + 1].name + "' overlap");
      for (FunctionSpec& f : m.functions) fillDefaultLines(f);
      const uint64_t end = m.functions.empty() ? 0x1000 : m.functions.back().rva + m.functions.back().size;
      m.size = u64(mj, "size", alignUp(end, 0x1000) + 0x1000, w);
      for (const ModuleSpec& other : m_s.modules)
        if (other.name == m.name) fail(w, "duplicate module name '" + m.name + "'");
      for (size_t fi = 0; fi < m.functions.size(); ++fi)
        m_fnIndex[m.name + "!" + m.functions[fi].name] = {static_cast<uint32_t>(mi),
                                                          static_cast<uint32_t>(fi)};
      m_modIndex[m.name] = static_cast<uint32_t>(mi);
      m_s.modules.push_back(std::move(m));
    }
  }

  // ---- stacks
  // ---------------------------------------------------------------------------------------------- Resolves
  // "mod!func", "mod!func+0x10" or "abs:0x7ffe0000" to a frame.
  StackFrame parseFrameString(const std::string& text, Prng* rng, const std::string& where) {
    StackFrame fr;
    if (text.rfind("abs:", 0) == 0) {
      fr.module = kUnmappedModule;
      fr.value = asU64(json(text.substr(4)), where);
      return fr;
    }
    const size_t bang = text.find('!');
    if (bang == std::string::npos) {
      fail(where, "frame '" + text + "' must be \"module!function[+offset]\" or \"abs:<addr>\"");
      return fr;
    }
    std::string fn = text;
    bool hasOffset = false;
    uint64_t offset = 0;
    const size_t plus = text.rfind('+');
    if (plus != std::string::npos && plus > bang) {
      fn = text.substr(0, plus);
      offset = asU64(json(text.substr(plus + 1)), where);
      hasOffset = true;
    }
    const auto it = m_fnIndex.find(fn);
    if (it == m_fnIndex.end()) {
      fail(where, "unknown function '" + fn + "'");
      return fr;
    }
    fr.module = it->second.first;
    fr.function = it->second.second;
    const FunctionSpec& f = m_s.modules[fr.module].functions[fr.function];
    if (!hasOffset) offset = rng ? rng->below(f.size) : std::min<uint64_t>(16, f.size - 1);
    if (offset >= f.size) fail(where, "offset beyond function size in '" + text + "'");
    fr.value = f.rva + offset;
    return fr;
  }

  StackFrame parseFrameJson(const json& v, const std::string& where) {
    if (v.is_string()) return parseFrameString(v.get<std::string>(), nullptr, where);
    StackFrame fr;
    if (v.is_object() && find(v, "abs")) {
      fr.module = kUnmappedModule;
      fr.value = reqU64(v, "abs", where);
      return fr;
    }
    fail(where, "frame must be a string or {\"abs\": addr}");
    return fr;
  }

  void parseStacks() {
    if (const json* ss = array(m_doc, "stacks", "scenario")) {
      for (size_t i = 0; i < ss->size() && ok(); ++i) {
        const json& sj = (*ss)[i];
        const std::string w = "stacks[" + std::to_string(i) + "]";
        StackSpec st;
        st.name = reqStr(sj, "name", w);
        if (const json* fs = array(sj, "frames", w)) {
          for (size_t k = 0; k < fs->size() && ok(); ++k)
            st.rootToLeaf.push_back(parseFrameJson((*fs)[k], w + ".frames[" + std::to_string(k) + "]"));
        } else {
          fail(w, "missing 'frames'");
        }
        addStack(std::move(st), w);
      }
    }
    if (const json* gs = array(m_doc, "stack_gen", "scenario")) {
      for (size_t gi = 0; gi < gs->size() && ok(); ++gi) {
        const json& g = (*gs)[gi];
        const std::string w = "stack_gen[" + std::to_string(gi) + "]";
        const std::string prefix = reqStr(g, "prefix", w);
        const uint64_t count = reqU64(g, "count", w);
        const uint64_t minD = u64(g, "min_depth", 4, w), maxD = u64(g, "max_depth", 12, w);
        std::vector<uint32_t> mods;
        if (const json* ms = array(g, "modules", w)) {
          for (const json& mn : *ms) {
            const auto it = mn.is_string() ? m_modIndex.find(mn.get<std::string>()) : m_modIndex.end();
            if (it == m_modIndex.end())
              fail(w + ".modules", "unknown module");
            else
              mods.push_back(it->second);
          }
        } else {
          for (uint32_t m = 0; m < m_s.modules.size(); ++m) mods.push_back(m);
        }
        if (!ok()) return;
        if (mods.empty() || minD < 1 || maxD < minD || count > 100000) {
          fail(w, "invalid stack_gen parameters");
          return;
        }
        std::string root = str(g, "root", "", w);
        for (uint64_t i = 0; i < count && ok(); ++i) {
          Prng rng(streamSeed(m_s.seed, "stackgen:" + prefix, i));
          StackSpec st;
          st.name = prefix + std::to_string(i);
          const uint64_t depth = rng.range(minD, maxD);
          for (uint64_t d = 0; d < depth; ++d) {
            if (d == 0 && !root.empty()) {
              st.rootToLeaf.push_back(parseFrameString(root, nullptr, w + ".root"));
              continue;
            }
            const uint32_t mi = mods[rng.below(mods.size())];
            const ModuleSpec& m = m_s.modules[mi];
            if (m.functions.empty()) {
              fail(w, "module '" + m.name + "' has no functions");
              return;
            }
            StackFrame fr;
            fr.module = mi;
            fr.function = static_cast<uint32_t>(rng.below(m.functions.size()));
            fr.value = m.functions[fr.function].rva + rng.below(m.functions[fr.function].size);
            st.rootToLeaf.push_back(fr);
          }
          addStack(std::move(st), w);
        }
      }
    }
  }

  void addStack(StackSpec st, const std::string& where) {
    if (st.rootToLeaf.empty()) {
      fail(where, "stack '" + st.name + "' has no frames");
      return;
    }
    if (m_stackIndex.count(st.name)) {
      fail(where, "duplicate stack name '" + st.name + "'");
      return;
    }
    m_stackIndex[st.name] = static_cast<uint32_t>(m_s.stacks.size());
    m_s.stacks.push_back(std::move(st));
  }

  int32_t stackRef(const json& v, const std::string& where) {
    if (!v.is_string()) {
      fail(where, "stack reference must be a string");
      return -1;
    }
    const auto it = m_stackIndex.find(v.get<std::string>());
    if (it == m_stackIndex.end()) {
      fail(where, "unknown stack '" + v.get<std::string>() + "'");
      return -1;
    }
    return static_cast<int32_t>(it->second);
  }
  int32_t optStack(const json& o, const char* key, const std::string& where) {
    const json* v = find(o, key);
    return v ? stackRef(*v, where + "." + key) : -1;
  }

  // ---- threads
  // ---------------------------------------------------------------------------------------------
  std::vector<MixEntry> parseMix(const json& v, uint32_t tid, const std::string& where) {
    std::vector<MixEntry> mix;
    if (v.is_array()) {
      for (size_t i = 0; i < v.size() && ok(); ++i) {
        const json& e = v[i];
        const std::string w = where + "[" + std::to_string(i) + "]";
        MixEntry me;
        const json* sref = find(e, "stack");
        if (!sref) {
          fail(w, "missing 'stack'");
          break;
        }
        const int32_t si = stackRef(*sref, w + ".stack");
        if (si < 0) break;
        me.stack = static_cast<uint32_t>(si);
        me.weight = static_cast<uint32_t>(u64(e, "weight", 1, w));
        if (me.weight == 0) fail(w, "weight must be > 0");
        mix.push_back(me);
      }
    } else if (v.is_object() && find(v, "gen")) {
      const json& g = *find(v, "gen");
      const std::string prefix = reqStr(g, "prefix", where + ".gen");
      const uint64_t pick = reqU64(g, "pick", where + ".gen");
      std::vector<uint32_t> candidates;
      for (uint32_t i = 0; i < m_s.stacks.size(); ++i)
        if (m_s.stacks[i].name.rfind(prefix, 0) == 0) candidates.push_back(i);
      if (candidates.empty() || pick == 0) {
        fail(where + ".gen", "no stacks match prefix or pick == 0");
        return mix;
      }
      Prng rng(streamSeed(m_s.seed, "mixgen:" + prefix, tid));
      const size_t n = std::min<size_t>(candidates.size(), pick);
      for (size_t i = 0; i < n; ++i) {
        const size_t j = i + rng.below(candidates.size() - i);
        std::swap(candidates[i], candidates[j]);
        mix.push_back(MixEntry{candidates[i], static_cast<uint32_t>(1000 / (i + 1))});
      }
    } else {
      fail(where, "expected an array of {stack, weight} or {\"gen\": {...}}");
    }
    return mix;
  }

  void parsePattern(const json& pj, PatternSpec& p, const std::string& w) {
    p.present = true;
    std::tie(p.runLo, p.runHi) = usRange(pj, "run_us", w);
    std::tie(p.waitLo, p.waitHi) = usRange(pj, "wait_us", w);
    if (find(pj, "latency_us")) std::tie(p.latencyLo, p.latencyHi) = usRange(pj, "latency_us", w);
    if (const json* rs = array(pj, "wait_reasons", w))
      for (const json& r : *rs) p.waitReasons.push_back(static_cast<uint32_t>(asU64(r, w + ".wait_reasons")));
    if (const json* bs = array(pj, "blocking_stacks", w))
      for (const json& b : *bs) p.blockingStacks.push_back(stackRef(b, w + ".blocking_stacks"));
    if (const json* ts = array(pj, "waker_tids", w))
      for (const json& t : *ts) p.wakerTids.push_back(static_cast<uint32_t>(asU64(t, w + ".waker_tids")));
    p.readyPct = static_cast<uint32_t>(u64(pj, "ready_pct", 0, w));
    p.preemptPct = static_cast<uint32_t>(u64(pj, "preempt_pct", 0, w));
    p.wakerStack = optStack(pj, "waker_stack", w);
    p.priority = static_cast<uint32_t>(u64(pj, "priority", 8, w));
    if (p.runHi == 0 || p.waitHi == 0) fail(w, "run_us / wait_us ranges must be > 0");
    if (p.readyPct > 100 || p.preemptPct > 100) fail(w, "percentages must be <= 100");
    if (p.readyPct > 0 && (p.wakerTids.empty() || p.latencyLo == 0)) fail(w, "ready_pct needs waker_tids");
    if (p.waitReasons.empty()) p.waitReasons.push_back(0);
  }

  void parseThreadInstance(const json& tj, uint32_t tid, uint64_t index, uint64_t globalIndex,
                           const std::string& w) {
    ThreadSpec t;
    t.tid = tid;
    t.name = replaceAll(
        replaceAll(str(tj, "name", "thread-" + std::to_string(tid), w), "{i}", std::to_string(index)),
        "{tid}", std::to_string(tid));
    t.cpu = static_cast<uint32_t>(u64(tj, "cpu", globalIndex % 8, w));
    t.startOffset = us(tj, "start_us", 0, w);
    t.fromStart = boolean(tj, "from_start", false, w);
    if (const json* mj = find(tj, "mix")) t.mix = parseMix(*mj, tid, w + ".mix");
    if (const json* is = array(tj, "intervals", w)) {
      // An element {"group": [...], "repeat": N} expands to N copies of the listed intervals.
      std::vector<const json*> flat;
      for (size_t i = 0; i < is->size() && ok(); ++i) {
        const json& e = (*is)[i];
        const json* g = find(e, "group");
        if (!g) {
          flat.push_back(&e);
          continue;
        }
        const uint64_t repeat = u64(e, "repeat", 1, w + ".intervals[" + std::to_string(i) + "]");
        if (!g->is_array() || repeat == 0 || repeat * g->size() > 1'000'000) {
          fail(w + ".intervals[" + std::to_string(i) + "]", "invalid group / repeat");
          break;
        }
        for (uint64_t r = 0; r < repeat; ++r)
          for (const json& sub : *g) flat.push_back(&sub);
      }
      for (size_t i = 0; i < flat.size() && ok(); ++i)
        parseInterval(*flat[i], t, i + 1 == flat.size(), w + ".intervals[" + std::to_string(i) + "]");
    }
    if (const json* pj = find(tj, "pattern")) parsePattern(*pj, t.pattern, w + ".pattern");
    if (const json* es = array(tj, "extra_samples", w)) {
      for (size_t i = 0; i < es->size() && ok(); ++i) {
        const json& ej = (*es)[i];
        const std::string ew = w + ".extra_samples[" + std::to_string(i) + "]";
        ExtraSample e;
        e.at = reqUs(ej, "at_us", ew);
        const json* sr = find(ej, "stack");
        if (!sr) {
          fail(ew, "missing 'stack'");
          break;
        }
        const int32_t si = stackRef(*sr, ew + ".stack");
        if (si < 0) break;
        e.stack = static_cast<uint32_t>(si);
        t.extraSamples.push_back(e);
      }
      std::stable_sort(t.extraSamples.begin(), t.extraSamples.end(),
                       [](const ExtraSample& a, const ExtraSample& b) { return a.at < b.at; });
    }
    if (const json* fj = find(tj, "fiber")) parseThreadFibers(*fj, t, w + ".fiber");
    if (!ok()) return;
    if (t.fromStart && t.startOffset != 0) fail(w, "from_start threads cannot have start_us");
    if (t.fromStart && !t.intervals.empty() && t.intervals.front().state != SegState::Run)
      fail(w, "from_start requires the first interval to be running");
    if (t.mix.empty() && (!t.intervals.empty() || t.pattern.present)) fail(w, "thread has no 'mix'");
    m_s.threads.push_back(std::move(t));
  }

  void parseInterval(const json& ij, ThreadSpec& t, bool last, const std::string& w) {
    IntervalSpec iv;
    const std::string st = reqStr(ij, "state", w);
    if (st == "running")
      iv.state = SegState::Run;
    else if (st == "waiting")
      iv.state = SegState::Wait;
    else if (st == "ready")
      iv.state = SegState::Ready;
    else
      fail(w + ".state", "must be running, waiting or ready");
    if (find(ij, "dur_us")) {
      iv.duration = us(ij, "dur_us", 0, w);
      if (iv.duration == 0) fail(w + ".dur_us", "must be > 0");
    } else {
      iv.toEnd = true;
      if (!last) fail(w, "only the last interval may omit dur_us");
    }
    iv.waitReason = static_cast<uint8_t>(u64(ij, "wait_reason", 0, w));
    iv.waitMode = static_cast<uint8_t>(u64(ij, "wait_mode", 0, w));
    iv.priority = static_cast<uint8_t>(u64(ij, "priority", 8, w));
    iv.blockingStack = optStack(ij, "blocking_stack", w);
    iv.readyAfter = us(ij, "ready_after_us", 0, w);
    iv.wakerTid = static_cast<uint32_t>(u64(ij, "waker_tid", 0, w));
    iv.wakerStack = optStack(ij, "waker_stack", w);
    if (iv.readyAfter != 0) {
      if (iv.state != SegState::Wait) fail(w, "ready_after_us only applies to waiting intervals");
      if (!iv.toEnd && iv.readyAfter >= iv.duration) fail(w, "ready_after_us must be < dur_us");
    }
    if (const json* mj = find(ij, "mix")) {
      if (iv.state != SegState::Run) fail(w, "mix override only applies to running intervals");
      t.mixOverrides.push_back(parseMix(*mj, t.tid, w + ".mix"));
      iv.mixOverride = static_cast<int32_t>(t.mixOverrides.size() - 1);
    }
    if (!ok()) return;
    if (!t.intervals.empty()) {
      const bool prevRun = t.intervals.back().state == SegState::Run;
      if (prevRun == (iv.state == SegState::Run)) fail(w, "intervals must alternate running / off-cpu");
    } else if (iv.state != SegState::Run) {
      fail(w, "the first interval must be running");
    }
    t.intervals.push_back(std::move(iv));
  }

  void parseThreads() {
    const json* ts = array(m_doc, "threads", "scenario");
    if (!ts) return;
    uint64_t global = 0;
    for (size_t i = 0; i < ts->size() && ok(); ++i) {
      const json& tj = (*ts)[i];
      const std::string w = "threads[" + std::to_string(i) + "]";
      const uint64_t count = u64(tj, "count", 1, w);
      const uint64_t base = find(tj, "tid") ? reqU64(tj, "tid", w) : reqU64(tj, "tid_base", w);
      if (count == 0 || count > 4096) {
        fail(w, "count must be in 1..4096");
        return;
      }
      for (uint64_t k = 0; k < count && ok(); ++k, ++global)
        parseThreadInstance(tj, static_cast<uint32_t>(base + k), k, global, w);
    }
  }

  // Fiber description attached to a thread: initial fiber, switches.
  void parseThreadFibers(const json& fj, const ThreadSpec& t, const std::string& w) {
    const uint64_t initial = reqU64(fj, "initial", w);
    const uint64_t initialAt = us(fj, "initial_us", 0, w);
    m_s.fiberEvents.push_back(FiberEventInst{initialAt, t.tid, 3, initial, 0});
    uint64_t current = initial;
    uint64_t lastEnd = initialAt;
    if (const json* sw = array(fj, "switches", w)) {
      for (size_t i = 0; i < sw->size() && ok(); ++i) {
        const json& sj = (*sw)[i];
        const std::string sw_ = w + ".switches[" + std::to_string(i) + "]";
        const uint64_t at = reqUs(sj, "at_us", sw_);
        const uint64_t gap = us(sj, "gap_us", 1, sw_);
        const uint64_t to = reqU64(sj, "to", sw_);
        if (gap == 0 || at <= lastEnd) {
          fail(sw_, "switches must be ordered, with gap_us > 0");
          return;
        }
        m_s.fiberEvents.push_back(FiberEventInst{at, t.tid, 2, current, to});
        m_s.fiberEvents.push_back(FiberEventInst{at + gap, t.tid, 3, to, current});
        current = to;
        lastEnd = at + gap;
      }
    }
  }

  void parseFiberRegistrations() {
    const json* fs = array(m_doc, "fibers", "scenario");
    if (!fs) return;
    for (size_t i = 0; i < fs->size() && ok(); ++i) {
      const json& fj = (*fs)[i];
      const std::string w = "fibers[" + std::to_string(i) + "]";
      const uint64_t id = reqU64(fj, "id", w);
      const uint32_t tid = static_cast<uint32_t>(reqU64(fj, "tid", w));
      m_s.fiberEvents.push_back(FiberEventInst{us(fj, "register_us", 0, w), tid, 0, id, 0});
      if (find(fj, "unregister_us"))
        m_s.fiberEvents.push_back(FiberEventInst{us(fj, "unregister_us", 0, w), tid, 1, id, 0});
    }
  }

  // ---- scopes
  // ----------------------------------------------------------------------------------------------
  void parseScope(const json& sj, uint64_t parentBegin, uint64_t parentEnd, uint32_t tid, uint64_t fiber,
                  uint32_t depth, Prng& rng, const std::string& w) {
    if (!ok()) return;
    const uint32_t myTid = find(sj, "tid") ? static_cast<uint32_t>(reqU64(sj, "tid", w)) : tid;
    const uint64_t myFiber = u64(sj, "fiber", fiber, w);
    const std::string id = reqStr(sj, "id", w);
    const uint64_t beginRel = us(sj, "begin_us", 0, w);
    const uint64_t dur = reqUs(sj, "dur_us", w);
    uint64_t count = 1, period = 0, jitter = 0;
    if (const json* rj = find(sj, "repeat")) {
      count = reqU64(*rj, "count", w + ".repeat");
      period = reqUs(*rj, "period_us", w + ".repeat");
      jitter = us(*rj, "dur_jitter_us", 0, w + ".repeat");
      if (count == 0 || count > 10'000'000 || period == 0) fail(w + ".repeat", "invalid repeat");
    }
    const std::string callerText = str(sj, "caller", "", w);
    if (dur == 0 && ok()) fail(w, "dur_us must be > 0");
    if (!ok()) return;
    StackFrame caller;
    if (!callerText.empty()) caller = parseFrameString(callerText, nullptr, w + ".caller");
    const std::string data = str(sj, "data", "", w);
    const uint32_t color = static_cast<uint32_t>(u64(sj, "color", 0xff808080u, w));
    for (uint64_t rep = 0; rep < count && ok(); ++rep) {
      ScopeInstance si;
      si.tid = myTid;
      si.id = id;
      si.data = data;
      si.color = color;
      si.depth = depth;
      si.fiber = myFiber;
      si.begin = parentBegin + beginRel + rep * period;
      si.end = si.begin + dur + (jitter ? rng.range(0, jitter) : 0);
      if (depth > 0 && si.end > parentEnd) {
        fail(w, "child scope '" + id + "' extends beyond its parent");
        return;
      }
      if (!callerText.empty()) {
        si.callerModule = caller.module;
        si.callerFunction = caller.function;
        si.callerAddr =
            caller.module == kUnmappedModule ? caller.value : m_s.modules[caller.module].base + caller.value;
      }
      m_s.scopes.push_back(si);
      if (const json* cs = array(sj, "children", w)) {
        for (size_t c = 0; c < cs->size() && ok(); ++c)
          parseScope((*cs)[c], si.begin, si.end, myTid, myFiber, depth + 1, rng,
                     w + ".children[" + std::to_string(c) + "]");
      }
    }
  }

  void parseScopes() {
    const json* ss = array(m_doc, "scopes", "scenario");
    if (!ss) return;
    Prng rng(streamSeed(m_s.seed, "scopes"));
    for (size_t i = 0; i < ss->size() && ok(); ++i)
      parseScope((*ss)[i], 0, 0, 0, 0, 0, rng, "scopes[" + std::to_string(i) + "]");
  }

  // ---- frame marks / markers / plots / renames
  // --------------------------------------------------------------
  void parseFrameMarks() {
    const json* fs = array(m_doc, "frame_marks", "scenario");
    if (!fs) return;
    Prng rng(streamSeed(m_s.seed, "framemarks"));
    for (size_t i = 0; i < fs->size() && ok(); ++i) {
      const json& fj = (*fs)[i];
      const std::string w = "frame_marks[" + std::to_string(i) + "]";
      const uint32_t tid = static_cast<uint32_t>(reqU64(fj, "tid", w));
      const std::string name = str(fj, "name", "Frame", w);
      const uint64_t start = us(fj, "start_us", 0, w);
      const uint64_t period = reqUs(fj, "period_us", w);
      const uint64_t jitter = us(fj, "jitter_us", 0, w);
      const uint64_t count = reqU64(fj, "count", w);
      if (period == 0 || jitter >= period || count > 10'000'000) {
        fail(w, "need period_us > 0, jitter_us < period_us");
        return;
      }
      // "hitches": [{"frame": 59, "extra_us": 34000}] lengthens frame 59 (every later mark shifts by
      // extra_us).
      std::vector<std::pair<uint64_t, uint64_t>> hitches;
      if (const json* hs = array(fj, "hitches", w)) {
        for (const json& hj : *hs)
          hitches.push_back({reqU64(hj, "frame", w + ".hitches"), reqUs(hj, "extra_us", w + ".hitches")});
      }
      for (uint64_t n = 0; n < count; ++n) {
        FrameMarkInst f;
        uint64_t shift = 0;
        for (const auto& hitch : hitches)
          if (hitch.first < n) shift += hitch.second;
        f.ts = start + n * period + shift + (jitter ? rng.range(0, jitter) : 0);
        f.tid = tid;
        f.name = name;
        f.number = n;
        m_s.frameMarks.push_back(std::move(f));
      }
    }
  }

  void parseMarkers() {
    const json* ms = array(m_doc, "markers", "scenario");
    if (!ms) return;
    for (size_t i = 0; i < ms->size() && ok(); ++i) {
      const json& mj = (*ms)[i];
      const std::string w = "markers[" + std::to_string(i) + "]";
      MarkerInst m;
      m.ts = reqUs(mj, "at_us", w);
      m.tid = static_cast<uint32_t>(u64(mj, "tid", 0, w));
      m.name = reqStr(mj, "name", w);
      m.data = str(mj, "data", "", w);
      m.color = static_cast<uint32_t>(u64(mj, "color", 0xffffffffu, w));
      m_s.markers.push_back(std::move(m));
    }
  }

  void parsePlots() {
    const json* ps = array(m_doc, "plots", "scenario");
    if (!ps) return;
    for (size_t i = 0; i < ps->size() && ok(); ++i) {
      const json& pj = (*ps)[i];
      const std::string w = "plots[" + std::to_string(i) + "]";
      const std::string name = reqStr(pj, "name", w);
      const std::string kind = str(pj, "kind", "noise", w);
      const uint64_t start = us(pj, "start_us", 0, w);
      const uint64_t period = reqUs(pj, "period_us", w);
      const uint64_t count = reqU64(pj, "count", w);
      const uint64_t lo = u64(pj, "min", 0, w), hi = u64(pj, "max", 100, w);
      if (period == 0 || hi < lo || count > 10'000'000 || (kind != "noise" && kind != "ramp")) {
        fail(w, "invalid plot (period_us > 0, min <= max, kind noise|ramp)");
        return;
      }
      Prng rng(streamSeed(m_s.seed, "plot:" + name));
      for (uint64_t n = 0; n < count; ++n) {
        PlotInst p;
        p.ts = start + n * period;
        p.name = name;
        // Quarter-unit resolution keeps every value exactly representable as a double.
        const uint64_t q =
            kind == "noise" ? rng.range(lo * 4, hi * 4) : lo * 4 + (n * 4) % ((hi - lo) * 4 + 1);
        p.value = static_cast<double>(q) / 4.0;
        m_s.plots.push_back(std::move(p));
      }
    }
  }

  void parseRenames() {
    const json* rs = array(m_doc, "thread_names", "scenario");
    if (!rs) return;
    for (size_t i = 0; i < rs->size() && ok(); ++i) {
      const json& rj = (*rs)[i];
      const std::string w = "thread_names[" + std::to_string(i) + "]";
      ThreadRename r;
      r.ts = reqUs(rj, "at_us", w);
      r.tid = static_cast<uint32_t>(reqU64(rj, "tid", w));
      r.name = reqStr(rj, "name", w);
      m_s.renames.push_back(std::move(r));
    }
  }

  // ---- validation
  // ------------------------------------------------------------------------------------------
  bool hasThread(uint32_t tid) const {
    for (const ThreadSpec& t : m_s.threads)
      if (t.tid == tid) return true;
    return false;
  }

  void validate() {
    for (size_t i = 0; i < m_s.threads.size(); ++i)
      for (size_t j = i + 1; j < m_s.threads.size(); ++j)
        if (m_s.threads[i].tid == m_s.threads[j].tid)
          fail("threads", "duplicate tid " + std::to_string(m_s.threads[i].tid));
    for (const ThreadSpec& t : m_s.threads) {
      if (t.startOffset >= m_s.duration)
        fail("threads", "thread " + std::to_string(t.tid) + " starts after the trace end");
      for (const ExtraSample& e : t.extraSamples)
        if (e.at >= m_s.duration) fail("threads", "extra sample beyond trace end");
    }
    for (const ScopeInstance& s : m_s.scopes) {
      if (!hasThread(s.tid)) fail("scopes", "scope '" + s.id + "' uses unknown tid " + std::to_string(s.tid));
      if (s.end >= m_s.duration) fail("scopes", "scope '" + s.id + "' ends at or after the trace end");
    }
    for (const FiberEventInst& e : m_s.fiberEvents) {
      if (!hasThread(e.tid)) fail("fibers", "fiber event on unknown tid " + std::to_string(e.tid));
      if (e.ts >= m_s.duration) fail("fibers", "fiber event at or after the trace end");
    }
    for (const FrameMarkInst& f : m_s.frameMarks)
      if (f.ts >= m_s.duration) fail("frame_marks", "frame mark beyond trace end");
    for (const MarkerInst& mk : m_s.markers)
      if (mk.ts >= m_s.duration) fail("markers", "marker beyond trace end");
    for (const PlotInst& p : m_s.plots)
      if (p.ts >= m_s.duration) fail("plots", "plot sample beyond trace end");
    for (const ThreadRename& r : m_s.renames)
      if (r.ts >= m_s.duration || !hasThread(r.tid)) fail("thread_names", "invalid rename");
    validateFibers();
    if (m_s.tornTail && (m_s.blend || !m_s.frameMarks.empty()))
      fail("scenario", "torn_tail scenarios cannot use blend or frame marks");
    if (m_s.partial && (!m_s.scopes.empty() || !m_s.fiberEvents.empty()))
      fail("scenario", "partial scenarios cannot contain scopes or fibers");
  }

  // Scopes that name a fiber must begin and end while that fiber is the running fiber of their thread.
  void validateFibers() {
    std::map<std::pair<uint32_t, uint64_t>, std::vector<std::pair<uint64_t, uint64_t>>> periods;
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> since;
    std::vector<FiberEventInst> evs = m_s.fiberEvents;
    std::stable_sort(evs.begin(), evs.end(),
                     [](const FiberEventInst& a, const FiberEventInst& b) { return a.ts < b.ts; });
    for (const FiberEventInst& e : evs) {
      if (e.kind == 3) {
        since[{e.tid, e.fiber}] = e.ts;
      } else if (e.kind == 2 && since.count({e.tid, e.fiber})) {
        periods[{e.tid, e.fiber}].push_back({since[{e.tid, e.fiber}], e.ts});
        since.erase({e.tid, e.fiber});
      }
    }
    for (const auto& [key, start] : since) periods[key].push_back({start, UINT64_MAX});
    for (const ScopeInstance& si : m_s.scopes) {
      if (si.fiber == 0) continue;
      const auto it = periods.find({si.tid, si.fiber});
      bool beginOk = false, endOk = false;
      if (it != periods.end())
        for (const auto& [a, b] : it->second) {
          beginOk = beginOk || (si.begin >= a && si.begin < b);
          endOk = endOk || (si.end > a && si.end < b);
        }
      if (!beginOk || !endOk)
        fail("scopes", "scope '" + si.id + "' on fiber " + std::to_string(si.fiber) +
                           " does not begin/end while that fiber is running on thread " +
                           std::to_string(si.tid));
    }
  }

  const json& m_doc;
  std::string m_fallback;
  Scenario m_s;
  std::string m_err;
  std::map<std::string, std::pair<uint32_t, uint32_t>> m_fnIndex;
  std::map<std::string, uint32_t> m_modIndex;
  std::map<std::string, uint32_t> m_stackIndex;
};

}  // namespace

Result<Scenario> parseScenario(const json& doc, std::string_view fallbackName) {
  Parser p(doc, fallbackName);
  return p.run();
}

Result<Scenario> loadScenario(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return makeError(ErrorCode::IoError, "cannot open scenario " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  const json doc = json::parse(ss.str(), nullptr, false);
  if (doc.is_discarded()) return makeError(ErrorCode::CorruptData, "scenario " + path + " is not valid JSON");
  const std::string stem = std::filesystem::path(path).stem().string();
  return parseScenario(doc, stem);
}

}  // namespace tachyon::tracegen
