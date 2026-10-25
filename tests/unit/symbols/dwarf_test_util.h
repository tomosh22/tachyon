// Shared helpers of the DWARF provider tests: fixture binaries, source markers, symbol lookup by name and an
// addr2line oracle.
#pragma once

#include "model/model.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "symbols/dwarf/elf_reader.h"
#include "symbols/symbol_strings.h"

#include <gtest/gtest.h>

#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace tachyon::symbols::dwarftest {

inline std::filesystem::path fixture(const std::string& name) {
  return std::filesystem::path(TACHYON_DWARF_FIXTURE_DIR) / name;
}

inline std::filesystem::path fixtureSource(const std::string& name) {
  return std::filesystem::path(TACHYON_DWARF_FIXTURE_SOURCE_DIR) / name;
}

// Line number of the "// @marker" comment in a fixture source file (0 if absent).
inline uint32_t markerLine(const std::string& file, const std::string& marker) {
  std::ifstream in(fixtureSource(file));
  std::string line;
  uint32_t n = 0;
  const std::string needle = "@" + marker;
  while (std::getline(in, line)) {
    ++n;
    const size_t at = line.find("// " + needle);
    if (at != std::string::npos &&
        (at + 3 + needle.size() == line.size() || line[at + 3 + needle.size()] == ' ')) {
      return n;
    }
  }
  return 0;
}

inline model::Module moduleFor(const std::filesystem::path& file) {
  model::Module m;
  m.path = file.string();
  auto elf = ElfFile::open(file);
  if (elf.ok()) {
    if (auto bid = elf.value().buildId()) {
      m.id.kind = 2;
      m.id.len = static_cast<uint8_t>(bid->size());
      std::copy(bid->begin(), bid->end(), m.id.bytes.begin());
    }
  }
  return m;
}

// RVA of the symbol-table entry called `name` (raw / mangled).
inline std::optional<uint64_t> symbolRva(const std::filesystem::path& file, const std::string& name) {
  auto elf = ElfFile::open(file);
  if (!elf.ok()) return std::nullopt;
  auto syms = elf.value().readCodeSymbols();
  if (!syms.ok()) return std::nullopt;
  for (const ElfSymbol& s : syms.value()) {
    if (s.name == name) return s.value - elf.value().loadBias();
  }
  return std::nullopt;
}

inline uint64_t symbolSize(const std::filesystem::path& file, const std::string& name) {
  auto elf = ElfFile::open(file);
  auto syms = elf.value().readCodeSymbols();
  for (const ElfSymbol& s : syms.value()) {
    if (s.name == name) return s.size;
  }
  return 0;
}

struct A2lFrame {
  std::string function;
  std::string file;
  uint32_t line = 0;
};

inline bool haveTool(const char* tool) {
  const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

// Reference symbolizer output for one address: frames innermost first (the last one is the physical
// function). `gnu`: binutils `addr2line -f -i -C -e file 0x<addr>` (reliable for gcc-built binaries;
// binutils 2.42 drops the inline frames of clang's DWARF 5 range lists); otherwise `llvm-symbolizer -i -f -C
// -e file 0x<addr>`.
inline std::vector<A2lFrame> symbolize(const std::filesystem::path& file, uint64_t addr, bool gnu) {
  char cmd[1024];
  if (gnu) {
    std::snprintf(cmd, sizeof(cmd), "addr2line -f -i -C -e '%s' 0x%llx 2>/dev/null", file.string().c_str(),
                  static_cast<unsigned long long>(addr));
  } else {
    std::snprintf(cmd, sizeof(cmd), "llvm-symbolizer -i -f -C -e '%s' 0x%llx 2>/dev/null",
                  file.string().c_str(), static_cast<unsigned long long>(addr));
  }
  std::vector<A2lFrame> out;
  FILE* p = popen(cmd, "r");
  if (p == nullptr) return out;
  std::string all;
  char buf[512];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) all += buf;
  pclose(p);
  std::istringstream ss(all);
  std::string fn, loc;
  while (std::getline(ss, fn) && !fn.empty() && std::getline(ss, loc)) {
    A2lFrame f;
    f.function = fn;
    const size_t sp = loc.find(" (discriminator");
    if (sp != std::string::npos) loc.resize(sp);
    if (!gnu) {  // file:line:column
      const size_t c2 = loc.rfind(':');
      if (c2 != std::string::npos) loc.resize(c2);
    }
    const size_t colon = loc.rfind(':');
    if (colon != std::string::npos) {
      f.file = loc.substr(0, colon);
      f.line = static_cast<uint32_t>(std::strtoul(loc.c_str() + colon + 1, nullptr, 10));
    }
    out.push_back(std::move(f));
  }
  return out;
}

// llvm-symbolizer over many addresses at once (addresses are passed on stdin, one invocation). One frame list
// per address, innermost first; an unresolved address yields a single frame named "??".
inline std::vector<std::vector<A2lFrame>> symbolizeBatch(const std::filesystem::path& file,
                                                         const std::vector<uint64_t>& addrs,
                                                         bool shortNames = false) {
  const std::filesystem::path in =
      std::filesystem::temp_directory_path() / ("tachyon-symbolizer-in-" + std::to_string(getpid()));
  {
    std::ofstream out(in);
    for (const uint64_t a : addrs) out << "0x" << std::hex << a << "\n";
  }
  const std::string cmd = std::string("llvm-symbolizer -i ") + (shortNames ? "--functions=short" : "-f") +
                          " -C -e '" + file.string() + "' < '" + in.string() + "' 2>/dev/null";
  std::vector<std::vector<A2lFrame>> result;
  FILE* p = popen(cmd.c_str(), "r");
  if (p != nullptr) {
    std::string all;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) all += buf;
    pclose(p);
    std::istringstream ss(all);
    std::string fn, loc;
    std::vector<A2lFrame> frames;
    while (std::getline(ss, fn)) {
      if (fn.empty()) {  // end of one address
        result.push_back(std::move(frames));
        frames.clear();
        continue;
      }
      if (!std::getline(ss, loc)) break;
      A2lFrame f;
      f.function = fn;
      const size_t c2 = loc.rfind(':');
      if (c2 != std::string::npos) loc.resize(c2);  // drop the column
      const size_t colon = loc.rfind(':');
      if (colon != std::string::npos) {
        f.file = loc.substr(0, colon);
        f.line = static_cast<uint32_t>(std::strtoul(loc.c_str() + colon + 1, nullptr, 10));
      }
      frames.push_back(std::move(f));
    }
  }
  std::error_code ec;
  std::filesystem::remove(in, ec);
  return result;
}

// First line (1-based) of `file` containing `needle`, 0 if none.
inline uint32_t lineContaining(const std::filesystem::path& file, const std::string& needle) {
  std::ifstream in(file);
  std::string line;
  uint32_t n = 0;
  while (std::getline(in, line)) {
    ++n;
    if (line.find(needle) != std::string::npos) return n;
  }
  return 0;
}

inline std::string baseName(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

// Writes a minimal ELF64 file: one PT_LOAD at `loadVaddr` (omitted when loadVaddr == ~0), a non-empty
// .debug_info section (so providers consult their DWARF backend) and no symbol table. For tests that supply
// the DWARF through a fake session and need full control over the load bias and over where no symbols exist.
inline std::filesystem::path writeSyntheticElf(const std::filesystem::path& path, uint64_t loadVaddr) {
  auto put16 = [](std::string& b, uint16_t v) { b.append(reinterpret_cast<const char*>(&v), 2); };
  auto put32 = [](std::string& b, uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); };
  auto put64 = [](std::string& b, uint64_t v) { b.append(reinterpret_cast<const char*>(&v), 8); };
  const bool hasLoad = loadVaddr != ~0ull;
  const std::string shstr = std::string("\0.debug_info\0.shstrtab\0", 24);
  const uint64_t phoff = 64, phsize = hasLoad ? 56 : 0;
  const uint64_t strOff = phoff + phsize, dbgOff = strOff + shstr.size(), shoff = (dbgOff + 8 + 7) & ~7ull;
  std::string b;
  b.append(
      "\x7f"
      "ELF\x02\x01\x01\0\0\0\0\0\0\0\0\0",
      16);
  put16(b, loadVaddr == 0 ? 3 : 2);  // ET_DYN for bias 0, else ET_EXEC
  put16(b, 62);
  put32(b, 1);
  put64(b, 0);
  put64(b, hasLoad ? phoff : 0);
  put64(b, shoff);
  put32(b, 0);
  put16(b, 64);
  put16(b, 56);
  put16(b, hasLoad ? 1 : 0);
  put16(b, 64);
  put16(b, 3);
  put16(b, 2);
  if (hasLoad) {
    put32(b, 1);  // PT_LOAD
    put32(b, 5);
    put64(b, 0);
    put64(b, loadVaddr);
    put64(b, loadVaddr);
    put64(b, dbgOff + 8);
    put64(b, dbgOff + 8);
    put64(b, 0x1000);
  }
  b += shstr;
  b.append(8, '\x01');  // .debug_info (content is never parsed: the session is a fake)
  b.resize(shoff, '\0');
  auto shdr = [&](uint32_t name, uint32_t type, uint64_t off, uint64_t size) {
    put32(b, name);
    put32(b, type);
    put64(b, 0);
    put64(b, 0);
    put64(b, off);
    put64(b, size);
    put32(b, 0);
    put32(b, 0);
    put64(b, 1);
    put64(b, 0);
  };
  shdr(0, 0, 0, 0);
  shdr(1, 1, dbgOff, 8);
  shdr(13, 3, strOff, shstr.size());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(b.data(), static_cast<std::streamsize>(b.size()));
  return path;
}

// Loaded provider over a fixture with its own string table.
struct Loaded {
  SymbolStringTable strings;
  std::unique_ptr<DwarfSymbolProvider> provider;
  std::filesystem::path file;
  Result<void> status = Result<void>();

  explicit Loaded(const std::filesystem::path& f, std::shared_ptr<IDwarfOpener> opener = nullptr) : file(f) {
    provider = std::make_unique<DwarfSymbolProvider>(strings, opener ? opener : makeLibdwOpener());
    status = provider->load(moduleFor(f), f);
  }
  std::string name(StringIdx i) const { return std::string(strings.get(i)); }
};

}  // namespace tachyon::symbols::dwarftest
