// Reading code bytes of a module from its image file on disk (PE and ELF64), by module-relative address.
#include "disasm/disassembler.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <new>

namespace tachyon::disasm {

namespace {

constexpr uint64_t kPage = 4096;

struct File {
  std::ifstream in;
  uint64_t size = 0;

  bool open(const std::filesystem::path& p) {
    in.open(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) return false;
    size = static_cast<uint64_t>(end);
    return true;
  }

  // Reads up to n bytes at `offset`; returns the number read.
  size_t readAt(uint64_t offset, void* dst, size_t n) {
    if (offset >= size) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, size - offset));
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return in ? n : static_cast<size_t>(in.gcount());
  }

  bool readExact(uint64_t offset, void* dst, size_t n) { return readAt(offset, dst, n) == n; }
};

template <typename T>
T load(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

// One mapped piece of the image: [rva, rva + mapped) holds file bytes [fileOffset, fileOffset + fileBytes),
// the rest of the mapped range is zero.
struct Segment {
  uint64_t rva = 0;
  uint64_t mapped = 0;
  uint64_t fileOffset = 0;
  uint64_t fileBytes = 0;
};

Result<std::vector<uint8_t>> extract(File& f, const std::vector<Segment>& segs, uint64_t rva, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(std::min<size_t>(size, size_t{1} << 24));
  uint64_t cur = rva;
  size_t remaining = size;
  bool first = true;
  while (remaining > 0) {
    const Segment* hit = nullptr;
    for (const Segment& s : segs) {
      if (cur >= s.rva && cur - s.rva < s.mapped) {
        hit = &s;
        break;
      }
    }
    if (hit == nullptr) {
      if (first) return makeError(ErrorCode::NotFound, "rva is not mapped by the image");
      break;
    }
    first = false;
    const uint64_t within = cur - hit->rva;
    const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining, hit->mapped - within));
    const size_t base = out.size();
    out.resize(base + take, 0);
    if (within < hit->fileBytes) {
      const size_t fromFile = static_cast<size_t>(std::min<uint64_t>(take, hit->fileBytes - within));
      const size_t got = f.readAt(hit->fileOffset + within, out.data() + base, fromFile);
      if (got < fromFile) {  // truncated file: stop at what exists
        out.resize(base + got);
        break;
      }
    }
    cur += take;
    remaining -= take;
  }
  return out;
}

Result<std::vector<Segment>> peSegments(File& f, const uint8_t* head, size_t headLen) {
  if (headLen < 0x40) return makeError(ErrorCode::CorruptData, "truncated DOS header");
  const uint32_t peOff = load<uint32_t>(head + 0x3C);
  uint8_t ph[24 + 0x60];
  if (!f.readExact(peOff, ph, 24) || std::memcmp(ph, "PE\0\0", 4) != 0)
    return makeError(ErrorCode::CorruptData, "missing PE signature");
  const uint16_t numSections = load<uint16_t>(ph + 6);
  const uint16_t optSize = load<uint16_t>(ph + 20);
  if (numSections == 0 || numSections > 96 || optSize < 64)
    return makeError(ErrorCode::CorruptData, "implausible PE header");
  uint8_t opt[64];
  if (!f.readExact(peOff + 24, opt, sizeof opt))
    return makeError(ErrorCode::CorruptData, "truncated PE header");
  const uint16_t magic = load<uint16_t>(opt);
  if (magic != 0x20B && magic != 0x10B) return makeError(ErrorCode::CorruptData, "unknown optional header");
  const uint32_t sizeOfHeaders = load<uint32_t>(opt + 60);
  std::vector<Segment> segs;
  segs.push_back(Segment{0, sizeOfHeaders, 0, sizeOfHeaders});
  const uint64_t tableOff = uint64_t{peOff} + 24 + optSize;
  for (uint32_t i = 0; i < numSections; ++i) {
    uint8_t sh[40];
    if (!f.readExact(tableOff + uint64_t{i} * 40, sh, sizeof sh))
      return makeError(ErrorCode::CorruptData, "truncated section table");
    const uint32_t virtualSize = load<uint32_t>(sh + 8);
    const uint32_t virtualAddress = load<uint32_t>(sh + 12);
    const uint32_t rawSize = load<uint32_t>(sh + 16);
    const uint32_t rawPtr = load<uint32_t>(sh + 20);
    const uint64_t mapped = virtualSize != 0 ? virtualSize : rawSize;
    segs.push_back(Segment{virtualAddress, mapped, rawPtr, std::min<uint64_t>(rawSize, mapped)});
  }
  // Sections first: they take precedence over the header range if the two overlap.
  std::rotate(segs.begin(), segs.begin() + 1, segs.end());
  return segs;
}

Result<std::vector<Segment>> elfSegments(File& f, const uint8_t* head, size_t headLen) {
  if (headLen < 64) return makeError(ErrorCode::CorruptData, "truncated ELF header");
  if (head[4] != 2 || head[5] != 1)
    return makeError(ErrorCode::Unsupported, "only little-endian ELF64 is supported");
  const uint64_t phoff = load<uint64_t>(head + 32);
  const uint16_t phentsize = load<uint16_t>(head + 54);
  const uint16_t phnum = load<uint16_t>(head + 56);
  if (phentsize < 56 || phnum == 0 || phnum > 1024)
    return makeError(ErrorCode::CorruptData, "bad program headers");
  struct Load {
    uint64_t vaddr, offset, filesz, memsz;
  };
  std::vector<Load> loads;
  for (uint16_t i = 0; i < phnum; ++i) {
    uint8_t ph[56];
    if (!f.readExact(phoff + uint64_t{i} * phentsize, ph, sizeof ph))
      return makeError(ErrorCode::CorruptData, "truncated program headers");
    if (load<uint32_t>(ph) != 1) continue;  // PT_LOAD
    loads.push_back(Load{load<uint64_t>(ph + 16), load<uint64_t>(ph + 8), load<uint64_t>(ph + 32),
                         load<uint64_t>(ph + 40)});
  }
  if (loads.empty()) return makeError(ErrorCode::CorruptData, "no PT_LOAD segment");
  uint64_t lowest = UINT64_MAX;
  for (const Load& l : loads) lowest = std::min(lowest, l.vaddr);
  const uint64_t shift = lowest & ~(kPage - 1);  // vaddr = rva + shift
  std::vector<Segment> segs;
  for (const Load& l : loads) {
    if (l.memsz == 0) continue;
    segs.push_back(Segment{l.vaddr - shift, l.memsz, l.offset, std::min(l.filesz, l.memsz)});
  }
  return segs;
}

}  // namespace

Result<std::vector<uint8_t>> readImageBytes(const std::filesystem::path& path, uint64_t rva, size_t size) {
  if (size > kMaxReadBytes)
    return makeError(ErrorCode::InvalidArgument, "requested size exceeds the 64 MiB read limit");
  File f;
  if (!f.open(path)) return makeError(ErrorCode::IoError, "cannot open " + path.string());
  uint8_t head[64];
  const size_t headLen = f.readAt(0, head, sizeof head);
  Result<std::vector<Segment>> segs =
      makeError(ErrorCode::Unsupported, "not a PE or ELF image: " + path.string());
  if (headLen >= 2 && head[0] == 'M' && head[1] == 'Z') {
    segs = peSegments(f, head, headLen);
  } else if (headLen >= 4 && std::memcmp(head,
                                         "\x7f"
                                         "ELF",
                                         4) == 0) {
    segs = elfSegments(f, head, headLen);
  }
  if (!segs) return segs.error();
  try {
    return extract(f, *segs, rva, size);
  } catch (const std::bad_alloc&) {
    return makeError(ErrorCode::OutOfMemory, "cannot allocate the requested image bytes");
  }
}

Result<std::vector<uint8_t>> readModuleBytes(const model::Module& module, uint64_t rva, size_t size,
                                             const std::filesystem::path& pathOverride) {
  return readImageBytes(pathOverride.empty() ? std::filesystem::path(module.path) : pathOverride, rva, size);
}

}  // namespace tachyon::disasm
