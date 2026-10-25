#include "symbols/symbol_keys.h"

#include "trace/trace_format.h"

#include <cstdio>

namespace tachyon::symbols {

namespace {

void appendHex(std::string& out, uint32_t value, int minDigits, bool upper) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), upper ? "%0*X" : "%0*x", minDigits, static_cast<unsigned>(value));
  out += buf;
}

void appendByte(std::string& out, uint8_t b, bool upper) {
  appendHex(out, b, 2, upper);
}

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::string pathFileName(std::string_view path) {
  const size_t pos = path.find_last_of("/\\");
  return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

std::string pathStem(std::string_view path) {
  std::string name = pathFileName(path);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot != 0) name.resize(dot);
  return name;
}

std::string pdbKey(const model::ModuleId& id) {
  if (id.kind != trace::kModuleIdPdb) return {};
  const auto& b = id.bytes;
  std::string out;
  out.reserve(40);
  // Data1 (u32), Data2 (u16), Data3 (u16) are stored little-endian; Data4 is a byte array.
  for (size_t i = 4; i-- > 0;) appendByte(out, b[i], true);
  for (size_t i = 6; i-- > 4;) appendByte(out, b[i], true);
  for (size_t i = 8; i-- > 6;) appendByte(out, b[i], true);
  for (size_t i = 8; i < 16; ++i) appendByte(out, b[i], true);
  appendHex(out, id.age, 1, true);
  return out;
}

std::string peImageKey(const model::ModuleId& id) {
  if (id.kind != trace::kModuleIdPeTimestampSize || id.len < 8) return {};
  std::string out;
  appendHex(out, readU32(id.bytes.data()), 8, true);
  appendHex(out, readU32(id.bytes.data() + 4), 1, true);
  return out;
}

std::string buildIdHex(const model::ModuleId& id) {
  if (id.kind != trace::kModuleIdGnuBuildId || id.len == 0 || id.len > id.bytes.size()) return {};
  std::string out;
  for (size_t i = 0; i < id.len; ++i) appendByte(out, id.bytes[i], false);
  return out;
}

std::string pdbFileName(const model::Module& module) {
  const std::string stem = pathStem(module.path);
  return stem.empty() ? std::string() : stem + ".pdb";
}

std::string SymbolKey::relativePath() const {
  if (scheme == Scheme::DebugInfod) return "buildid/" + key + "/debuginfo";
  return name + "/" + key + "/" + name;
}

std::optional<SymbolKey> symbolKeyFor(const model::Module& module) {
  SymbolKey k;
  switch (module.id.kind) {
    case trace::kModuleIdPdb:
      k.name = pdbFileName(module);
      k.key = pdbKey(module.id);
      break;
    case trace::kModuleIdPeTimestampSize:
      k.name = pathFileName(module.path);
      k.key = peImageKey(module.id);
      break;
    case trace::kModuleIdGnuBuildId:
      k.scheme = SymbolKey::Scheme::DebugInfod;
      k.name = "debuginfo";
      k.key = buildIdHex(module.id);
      break;
    default: return std::nullopt;
  }
  if (k.name.empty() || k.key.empty() || k.name == "." || k.name == "..") return std::nullopt;
  return k;
}

}  // namespace tachyon::symbols
