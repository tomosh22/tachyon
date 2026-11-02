// Minimal, bounds-checked ELF64 little-endian reader for the DWARF symbol provider and the symbol locator
// (WP-72). It reads headers, section data, the GNU build-id, .gnu_debuglink and the symbol tables; DWARF
// itself is read by libdw. Pure C++ (no libdw, no mmap): every read goes through std::ifstream and is checked
// against the file size, so a truncated or mutated file yields CorruptData and never an out-of-bounds access.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::symbols {

constexpr uint32_t kElfShtProgbits = 1;
constexpr uint32_t kElfShtSymtab = 2;
constexpr uint32_t kElfShtNobits = 8;
constexpr uint32_t kElfShtDynsym = 11;
constexpr uint64_t kElfShfExecinstr = 0x4;
constexpr uint64_t kElfShfCompressed = 0x800;
constexpr uint16_t kElfEtExec = 2;
constexpr uint16_t kElfEtDyn = 3;

struct ElfSection {
  std::string name;
  uint32_t type = 0;
  uint64_t flags = 0;
  uint64_t addr = 0;  // link-time virtual address
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t link = 0;
  uint32_t info = 0;
  uint64_t entsize = 0;
};

// A code symbol from .symtab / .dynsym.
struct ElfSymbol {
  std::string name;         // as stored (mangled)
  uint64_t value = 0;       // link-time virtual address
  uint64_t size = 0;        // st_size (0 = unknown)
  uint64_t sectionEnd = 0;  // link-time end address of the containing section
  uint8_t binding = 0;      // STB_LOCAL 0, STB_GLOBAL 1, STB_WEAK 2
};

struct ElfDebugLink {
  std::string name;
  uint32_t crc = 0;
};

class ElfFile {
 public:
  // Thread-safe. Opens and validates `path`: NotFound / IoError when unreadable, CorruptData for a truncated
  // or inconsistent file, Unsupported for anything but ELF64 little-endian. Section data is read on demand.
  static Result<ElfFile> open(const std::filesystem::path& path);

  // Thread-safe (const). ET_EXEC 2, ET_DYN 3, ...
  uint16_t type() const { return m_type; }
  uint16_t machine() const { return m_machine; }
  uint64_t fileSize() const { return m_fileSize; }
  const std::filesystem::path& path() const { return m_path; }
  const std::vector<ElfSection>& sections() const { return m_sections; }
  // Thread-safe. nullptr if there is no section of that name.
  const ElfSection* findSection(const std::string& name) const;

  // Thread-safe. Lowest PT_LOAD p_vaddr aligned down to 4096: the DWARF address of "RVA 0" (0 for PIE
  // binaries, e.g. 0x400000 for a non-PIE executable). 0 when there is no PT_LOAD.
  uint64_t loadBias() const { return m_loadBias; }

  // Thread-safe. True when .debug_info (or .zdebug_info) holds data in the file (not NOBITS).
  bool hasDebugInfo() const { return m_hasDebugInfo; }

  // Thread-safe. Reads a section's file bytes (NOBITS gives an empty vector). `maxBytes` bounds the
  // allocation (OutOfMemory-style protection against forged sizes): larger sections yield CorruptData.
  Result<std::vector<uint8_t>> readSection(const ElfSection& section, uint64_t maxBytes = (1ull << 31)) const;

  // Thread-safe. GNU build-id bytes (from .note.gnu.build-id) or nullopt.
  std::optional<std::vector<uint8_t>> buildId() const;
  // Thread-safe. .gnu_debuglink contents or nullopt.
  std::optional<ElfDebugLink> debugLink() const;

  // Thread-safe. Code symbols (FUNC, IFUNC, and NOTYPE in executable sections) of .symtab, else .dynsym.
  // CorruptData when the table or its string table is inconsistent.
  Result<std::vector<ElfSymbol>> readCodeSymbols() const;

 private:
  std::filesystem::path m_path;
  uint64_t m_fileSize = 0;
  uint16_t m_type = 0;
  uint16_t m_machine = 0;
  uint64_t m_loadBias = 0;
  bool m_hasDebugInfo = false;
  std::vector<ElfSection> m_sections;
};

// Thread-safe: pure function. CRC-32 (IEEE 802.3, as used by .gnu_debuglink) continuing from `crc`.
uint32_t elfCrc32(const uint8_t* data, size_t size, uint32_t crc = 0);
// Thread-safe. CRC-32 of a whole file (streamed); nullopt when it cannot be read.
std::optional<uint32_t> elfFileCrc32(const std::filesystem::path& path);

}  // namespace tachyon::symbols
