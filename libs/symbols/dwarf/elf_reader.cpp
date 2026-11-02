#include "symbols/dwarf/elf_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <system_error>

namespace tachyon::symbols {

namespace fs = std::filesystem;

namespace {

constexpr size_t kEhdrSize = 64;
constexpr size_t kPhdrSize = 56;
constexpr size_t kShdrSize = 64;
constexpr size_t kSymSize = 24;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kShtNote = 7;
constexpr uint32_t kShtStrtab = 3;
constexpr uint32_t kShtNull = 0;
constexpr uint32_t kMaxSections = 1u << 20;
constexpr uint64_t kMaxStringTable = 1ull << 30;
constexpr uint64_t kPageMask = 0xFFFull;

template <typename T>
T load(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

Error corrupt(const fs::path& path, const std::string& what) {
  return makeError(ErrorCode::CorruptData, path.string() + ": " + what);
}

// Reads exactly n bytes at `offset`; false if outside the file or short.
bool readAt(std::ifstream& in, uint64_t fileSize, uint64_t offset, uint64_t n, void* dst) {
  if (offset > fileSize || n > fileSize - offset) return false;
  if (n == 0) return true;
  in.clear();
  in.seekg(static_cast<std::streamoff>(offset));
  in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
  return in.good() || (in.eof() && static_cast<uint64_t>(in.gcount()) == n);
}

std::string boundedString(const std::vector<uint8_t>& table, uint64_t offset) {
  if (offset >= table.size()) return {};
  const char* begin = reinterpret_cast<const char*>(table.data()) + offset;
  const size_t room = table.size() - static_cast<size_t>(offset);
  return std::string(begin, ::strnlen(begin, room));
}

const std::array<uint32_t, 256>& crcTable() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  return table;
}

}  // namespace

uint32_t elfCrc32(const uint8_t* data, size_t size, uint32_t crc) {
  const auto& table = crcTable();
  uint32_t c = ~crc;
  for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

std::optional<uint32_t> elfFileCrc32(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<uint8_t> buf(1u << 20);
  uint32_t crc = 0;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    crc = elfCrc32(buf.data(), static_cast<size_t>(got), crc);
  }
  if (in.bad()) return std::nullopt;
  return crc;
}

Result<ElfFile> ElfFile::open(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) {
    return makeError(ErrorCode::NotFound, path.string() + ": not a readable file");
  }
  const uint64_t fileSize = static_cast<uint64_t>(fs::file_size(path, ec));
  if (ec) return makeError(ErrorCode::IoError, path.string() + ": cannot determine the file size");
  std::ifstream in(path, std::ios::binary);
  if (!in) return makeError(ErrorCode::IoError, path.string() + ": cannot open");

  uint8_t eh[kEhdrSize];
  if (fileSize < 4) return corrupt(path, "too small to be an ELF file");
  if (!readAt(in, fileSize, 0, 4, eh) || std::memcmp(eh,
                                                     "\x7f"
                                                     "ELF",
                                                     4) != 0) {
    return corrupt(path, "not an ELF file");
  }
  if (fileSize < kEhdrSize || !readAt(in, fileSize, 0, kEhdrSize, eh))
    return corrupt(path, "truncated ELF header");
  if (eh[4] != 2 || eh[5] != 1)
    return makeError(ErrorCode::Unsupported, path.string() + ": not ELF64 little-endian");

  ElfFile out;
  out.m_path = path;
  out.m_fileSize = fileSize;
  out.m_type = load<uint16_t>(eh + 16);
  out.m_machine = load<uint16_t>(eh + 18);
  const uint64_t phoff = load<uint64_t>(eh + 32);
  const uint64_t shoff = load<uint64_t>(eh + 40);
  const uint16_t phentsize = load<uint16_t>(eh + 54);
  const uint16_t phnum = load<uint16_t>(eh + 56);
  const uint16_t shentsize = load<uint16_t>(eh + 58);
  uint32_t shnum = load<uint16_t>(eh + 60);
  uint32_t shstrndx = load<uint16_t>(eh + 62);

  // Program headers: the lowest PT_LOAD address defines the RVA base.
  if (phnum != 0) {
    if (phentsize < kPhdrSize) return corrupt(path, "bad program header entry size");
    if (phoff > fileSize || static_cast<uint64_t>(phnum) * phentsize > fileSize - phoff) {
      return corrupt(path, "program headers outside the file");
    }
    std::vector<uint8_t> ph(static_cast<size_t>(phnum) * phentsize);
    if (!readAt(in, fileSize, phoff, ph.size(), ph.data()))
      return corrupt(path, "cannot read program headers");
    bool any = false;
    uint64_t lowest = 0;
    for (uint32_t i = 0; i < phnum; ++i) {
      const uint8_t* p = ph.data() + static_cast<size_t>(i) * phentsize;
      if (load<uint32_t>(p) != kPtLoad) continue;
      const uint64_t vaddr = load<uint64_t>(p + 16);
      lowest = any ? std::min(lowest, vaddr) : vaddr;
      any = true;
    }
    out.m_loadBias = any ? lowest & ~kPageMask : 0;
  }

  // Section headers.
  if (shoff == 0) return out;  // no sections (stripped section table): usable for the load bias only
  if (shentsize < kShdrSize) return corrupt(path, "bad section header entry size");
  if (shoff > fileSize || fileSize - shoff < kShdrSize)
    return corrupt(path, "section headers outside the file");
  if (shnum == 0 || shstrndx == 0xFFFF) {
    uint8_t s0[kShdrSize];
    if (!readAt(in, fileSize, shoff, kShdrSize, s0)) return corrupt(path, "cannot read section 0");
    if (shnum == 0)
      shnum = static_cast<uint32_t>(std::min<uint64_t>(load<uint64_t>(s0 + 32), kMaxSections + 1));
    if (shstrndx == 0xFFFF) shstrndx = load<uint32_t>(s0 + 40);
  }
  if (shnum > kMaxSections) return corrupt(path, "too many sections");
  if (static_cast<uint64_t>(shnum) * shentsize > fileSize - shoff)
    return corrupt(path, "section headers outside the file");
  if (shnum == 0) return out;
  if (shstrndx >= shnum) return corrupt(path, "bad section name table index");

  std::vector<uint8_t> sh(static_cast<size_t>(shnum) * shentsize);
  if (!readAt(in, fileSize, shoff, sh.size(), sh.data())) return corrupt(path, "cannot read section headers");
  std::vector<uint32_t> nameOffsets(shnum);
  out.m_sections.resize(shnum);
  for (uint32_t i = 0; i < shnum; ++i) {
    const uint8_t* p = sh.data() + static_cast<size_t>(i) * shentsize;
    ElfSection& s = out.m_sections[i];
    nameOffsets[i] = load<uint32_t>(p);
    s.type = load<uint32_t>(p + 4);
    s.flags = load<uint64_t>(p + 8);
    s.addr = load<uint64_t>(p + 16);
    s.offset = load<uint64_t>(p + 24);
    s.size = load<uint64_t>(p + 32);
    s.link = load<uint32_t>(p + 40);
    s.info = load<uint32_t>(p + 44);
    s.entsize = load<uint64_t>(p + 56);
    if (s.type != kElfShtNobits && s.type != kShtNull && s.size != 0) {
      if (s.offset > fileSize || s.size > fileSize - s.offset) {
        return corrupt(path, "section " + std::to_string(i) + " extends past the end of the file");
      }
    }
  }
  {
    const ElfSection& strs = out.m_sections[shstrndx];
    if (strs.type != kShtStrtab) return corrupt(path, "section name table is not a string table");
    std::vector<uint8_t> names;
    auto r = out.readSection(strs, kMaxStringTable);
    if (!r.ok()) return r.error();
    names = std::move(r).value();
    for (uint32_t i = 0; i < shnum; ++i) out.m_sections[i].name = boundedString(names, nameOffsets[i]);
  }
  for (const ElfSection& s : out.m_sections) {
    if ((s.name == ".debug_info" || s.name == ".zdebug_info") && s.type != kElfShtNobits && s.size != 0) {
      out.m_hasDebugInfo = true;
    }
  }
  return out;
}

const ElfSection* ElfFile::findSection(const std::string& name) const {
  for (const ElfSection& s : m_sections) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

Result<std::vector<uint8_t>> ElfFile::readSection(const ElfSection& section, uint64_t maxBytes) const {
  if (section.type == kElfShtNobits || section.size == 0) return std::vector<uint8_t>();
  if (section.size > maxBytes) return corrupt(m_path, "section '" + section.name + "' is implausibly large");
  if (section.offset > m_fileSize || section.size > m_fileSize - section.offset) {
    return corrupt(m_path, "section '" + section.name + "' extends past the end of the file");
  }
  std::ifstream in(m_path, std::ios::binary);
  if (!in) return makeError(ErrorCode::IoError, m_path.string() + ": cannot open");
  std::vector<uint8_t> data(static_cast<size_t>(section.size));
  if (!readAt(in, m_fileSize, section.offset, section.size, data.data())) {
    return corrupt(m_path, "cannot read section '" + section.name + "'");
  }
  return data;
}

std::optional<std::vector<uint8_t>> ElfFile::buildId() const {
  const ElfSection* s = findSection(".note.gnu.build-id");
  if (s == nullptr || s->type != kShtNote || s->size > (1u << 20)) return std::nullopt;
  auto data = readSection(*s, 1u << 20);
  if (!data.ok()) return std::nullopt;
  const std::vector<uint8_t>& d = data.value();
  size_t pos = 0;
  while (d.size() - pos >= 12) {
    const uint32_t namesz = load<uint32_t>(d.data() + pos);
    const uint32_t descsz = load<uint32_t>(d.data() + pos + 4);
    const uint32_t type = load<uint32_t>(d.data() + pos + 8);
    pos += 12;
    const uint64_t nameEnd = (static_cast<uint64_t>(namesz) + 3) & ~3ull;
    const uint64_t descEnd = (static_cast<uint64_t>(descsz) + 3) & ~3ull;
    if (nameEnd > d.size() - pos) return std::nullopt;
    const size_t nameAt = pos;
    pos += static_cast<size_t>(nameEnd);
    if (descEnd > d.size() - pos && descsz > d.size() - pos) return std::nullopt;
    if (type == 3 && namesz == 4 && std::memcmp(d.data() + nameAt, "GNU", 4) == 0 && descsz != 0 &&
        descsz <= 64) {
      return std::vector<uint8_t>(d.begin() + static_cast<std::ptrdiff_t>(pos),
                                  d.begin() + static_cast<std::ptrdiff_t>(pos + descsz));
    }
    pos += static_cast<size_t>(std::min<uint64_t>(descEnd, d.size() - pos));
  }
  return std::nullopt;
}

std::optional<ElfDebugLink> ElfFile::debugLink() const {
  const ElfSection* s = findSection(".gnu_debuglink");
  if (s == nullptr || s->size < 8 || s->size > 4096) return std::nullopt;
  auto data = readSection(*s, 4096);
  if (!data.ok()) return std::nullopt;
  const std::vector<uint8_t>& d = data.value();
  const char* begin = reinterpret_cast<const char*>(d.data());
  const size_t len = ::strnlen(begin, d.size());
  if (len == 0 || len >= d.size()) return std::nullopt;
  const size_t crcAt = (len + 1 + 3) & ~static_cast<size_t>(3);
  if (crcAt + 4 > d.size()) return std::nullopt;
  ElfDebugLink out;
  out.name.assign(begin, len);
  out.crc = load<uint32_t>(d.data() + crcAt);
  return out;
}

Result<std::vector<ElfSymbol>> ElfFile::readCodeSymbols() const {
  const ElfSection* table = nullptr;
  for (const uint32_t wanted : {kElfShtSymtab, kElfShtDynsym}) {
    for (const ElfSection& s : m_sections) {
      if (s.type == wanted && s.size != 0) {
        table = &s;
        break;
      }
    }
    if (table != nullptr) break;
  }
  std::vector<ElfSymbol> out;
  if (table == nullptr) return out;
  if (table->entsize != kSymSize || table->link >= m_sections.size() || table->size % kSymSize != 0) {
    return corrupt(m_path, "bad symbol table '" + table->name + "'");
  }
  const ElfSection& strSection = m_sections[table->link];
  if (strSection.type != kShtStrtab) return corrupt(m_path, "symbol string table is not a string table");
  auto syms = readSection(*table, 1ull << 30);
  if (!syms.ok()) return syms.error();
  auto strs = readSection(strSection, kMaxStringTable);
  if (!strs.ok()) return strs.error();

  const std::vector<uint8_t>& sb = syms.value();
  const size_t count = sb.size() / kSymSize;
  for (size_t i = 1; i < count; ++i) {  // entry 0 is the null symbol
    const uint8_t* p = sb.data() + i * kSymSize;
    const uint32_t nameOff = load<uint32_t>(p);
    const uint8_t info = p[4];
    const uint16_t shndx = load<uint16_t>(p + 6);
    const uint64_t value = load<uint64_t>(p + 8);
    const uint64_t size = load<uint64_t>(p + 16);
    const uint8_t type = info & 0xF;
    if (shndx == 0 || shndx >= 0xFF00 || shndx >= m_sections.size() || value == 0) continue;
    const ElfSection& sec = m_sections[shndx];
    if ((sec.flags & kElfShfExecinstr) == 0) continue;
    if (type != 2 /*FUNC*/ && type != 10 /*IFUNC*/ && type != 0 /*NOTYPE*/) continue;
    std::string name = boundedString(strs.value(), nameOff);
    if (name.empty() || (type == 0 && name[0] == '$')) continue;  // ARM mapping symbols
    ElfSymbol sym;
    sym.name = std::move(name);
    sym.value = value;
    sym.size = size;
    sym.sectionEnd = sec.addr + sec.size;
    sym.binding = info >> 4;
    out.push_back(std::move(sym));
  }
  return out;
}

}  // namespace tachyon::symbols
