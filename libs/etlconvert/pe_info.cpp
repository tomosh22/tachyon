#include "etlconvert/pe_info.h"

#include "core/span_util.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace tachyon::etlconvert {

namespace {

constexpr uint16_t kDosMagic = 0x5A4D;         // "MZ"
constexpr uint32_t kNtSignature = 0x00004550;  // "PE\0\0"
constexpr uint16_t kOptMagic32 = 0x10B;
constexpr uint16_t kOptMagic64 = 0x20B;
constexpr uint32_t kDebugTypeCodeView = 2;
constexpr uint32_t kRsdsSignature = 0x53445352;  // "RSDS"
constexpr size_t kDebugEntrySize = 28;
constexpr size_t kMaxDebugEntries = 64;
constexpr size_t kMaxSections = 96;
constexpr size_t kMaxPdbPath = 1024;
constexpr size_t kMaxFunctions = 4'000'000;
constexpr size_t kDirDebug = 6;
constexpr size_t kDirException = 3;

struct Section {
  uint32_t virtualAddress = 0;
  uint32_t virtualSize = 0;
  uint32_t rawPointer = 0;
  uint32_t rawSize = 0;
};

// Bounds-checked reads on top of the callback.
class Source {
 public:
  Source(const PeReadFn& read, uint64_t size) : m_read(read), m_size(size) {}

  bool readRaw(uint64_t offset, void* dst, size_t n) const {
    if (offset > m_size || m_size - offset < n) return false;
    return m_read(offset, std::span<std::byte>(static_cast<std::byte*>(dst), n));
  }
  template <typename T>
  bool readPod(uint64_t offset, T& out) const {
    return readRaw(offset, &out, sizeof(T));
  }

 private:
  const PeReadFn& m_read;
  uint64_t m_size;
};

struct Headers {
  PeInfo info;
  uint64_t optionalHeaderOffset = 0;
  uint32_t numberOfRvaAndSizes = 0;
  uint64_t dataDirOffset = 0;
  std::vector<Section> sections;
};

Result<Headers> parseHeaders(const Source& src) {
  Headers h;
  uint16_t dosMagic = 0;
  uint32_t ntOffset = 0;
  if (!src.readPod(0, dosMagic) || dosMagic != kDosMagic)
    return makeError(ErrorCode::CorruptData, "not a PE file: missing MZ header");
  if (!src.readPod(0x3C, ntOffset)) return makeError(ErrorCode::CorruptData, "truncated DOS header");
  uint32_t sig = 0;
  if (!src.readPod(ntOffset, sig) || sig != kNtSignature)
    return makeError(ErrorCode::CorruptData, "missing PE signature");
  // IMAGE_FILE_HEADER at ntOffset + 4: Machine u16, NumberOfSections u16, TimeDateStamp u32,
  // PointerToSymbolTable u32, NumberOfSymbols u32, SizeOfOptionalHeader u16, Characteristics u16.
  uint16_t machine = 0;
  uint16_t numSections = 0;
  uint16_t optSize = 0;
  uint32_t timeStamp = 0;
  const uint64_t fileHdr = static_cast<uint64_t>(ntOffset) + 4;
  if (!src.readPod(fileHdr, machine) || !src.readPod(fileHdr + 2, numSections) ||
      !src.readPod(fileHdr + 4, timeStamp) || !src.readPod(fileHdr + 16, optSize))
    return makeError(ErrorCode::CorruptData, "truncated file header");
  h.info.machine = machine;
  h.info.timeDateStamp = timeStamp;
  h.optionalHeaderOffset = fileHdr + 20;
  uint16_t optMagic = 0;
  if (!src.readPod(h.optionalHeaderOffset, optMagic))
    return makeError(ErrorCode::CorruptData, "truncated optional header");
  if (optMagic == kOptMagic64) {
    h.info.is64 = true;
  } else if (optMagic != kOptMagic32) {
    return makeError(ErrorCode::CorruptData, "unknown optional header magic");
  }
  // Both layouts: SizeOfImage at +56, CheckSum at +64; NumberOfRvaAndSizes at +108 (PE32+) / +92 (PE32) and
  // the data directories right after it.
  const uint64_t dirCountAt = h.optionalHeaderOffset + (h.info.is64 ? 108 : 92);
  h.dataDirOffset = h.optionalHeaderOffset + (h.info.is64 ? 112 : 96);
  if (!src.readPod(h.optionalHeaderOffset + 56, h.info.sizeOfImage) ||
      !src.readPod(h.optionalHeaderOffset + 64, h.info.checksum) ||
      !src.readPod(dirCountAt, h.numberOfRvaAndSizes))
    return makeError(ErrorCode::CorruptData, "truncated optional header");
  if (numSections > kMaxSections) return makeError(ErrorCode::CorruptData, "too many sections");
  const uint64_t sectionTable = h.optionalHeaderOffset + optSize;
  for (uint16_t i = 0; i < numSections; ++i) {
    // IMAGE_SECTION_HEADER (40 bytes): Name[8], VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData.
    const uint64_t at = sectionTable + static_cast<uint64_t>(i) * 40;
    Section s;
    if (!src.readPod(at + 8, s.virtualSize) || !src.readPod(at + 12, s.virtualAddress) ||
        !src.readPod(at + 16, s.rawSize) || !src.readPod(at + 20, s.rawPointer))
      return makeError(ErrorCode::CorruptData, "truncated section table");
    h.sections.push_back(s);
  }
  return h;
}

// Maps an RVA range to a file offset through the section table; false when it is not backed by file data.
bool rvaToOffset(const Headers& h, uint32_t rva, uint64_t length, uint64_t& offset) {
  for (const Section& s : h.sections) {
    const uint64_t span = std::max<uint64_t>(s.virtualSize, s.rawSize);
    if (rva >= s.virtualAddress && rva - s.virtualAddress < span) {
      const uint64_t within = rva - s.virtualAddress;
      if (within + length > s.rawSize) return false;
      offset = static_cast<uint64_t>(s.rawPointer) + within;
      return true;
    }
  }
  return false;
}

bool dataDirectory(const Source& src, const Headers& h, size_t index, uint32_t& rva, uint32_t& size) {
  if (index >= h.numberOfRvaAndSizes) return false;
  const uint64_t at = h.dataDirOffset + index * 8;
  return src.readPod(at, rva) && src.readPod(at + 4, size) && rva != 0 && size != 0;
}

}  // namespace

Result<PeInfo> parsePe(const PeReadFn& read, uint64_t fileSize) {
  Source src(read, fileSize);
  TACHYON_TRY_ASSIGN(Headers h, parseHeaders(src));
  uint32_t dirRva = 0;
  uint32_t dirSize = 0;
  if (!dataDirectory(src, h, kDirDebug, dirRva, dirSize)) return h.info;
  uint64_t dirOffset = 0;
  const size_t entries = std::min<size_t>(dirSize / kDebugEntrySize, kMaxDebugEntries);
  if (entries == 0 || !rvaToOffset(h, dirRva, entries * kDebugEntrySize, dirOffset)) return h.info;
  for (size_t i = 0; i < entries; ++i) {
    // IMAGE_DEBUG_DIRECTORY: Characteristics, TimeDateStamp, Major, Minor (u16 x2), Type, SizeOfData,
    // AddressOfRawData, PointerToRawData.
    const uint64_t e = dirOffset + i * kDebugEntrySize;
    uint32_t type = 0;
    uint32_t sizeOfData = 0;
    uint32_t rawPtr = 0;
    if (!src.readPod(e + 12, type) || !src.readPod(e + 16, sizeOfData) || !src.readPod(e + 24, rawPtr)) break;
    if (type != kDebugTypeCodeView || sizeOfData < 24 || rawPtr == 0) continue;
    uint32_t magic = 0;
    if (!src.readPod(rawPtr, magic) || magic != kRsdsSignature) continue;
    // RSDS: signature u32, GUID[16], age u32, NUL-terminated UTF-8 path.
    if (!src.readRaw(static_cast<uint64_t>(rawPtr) + 4, h.info.guid, 16) ||
        !src.readPod(static_cast<uint64_t>(rawPtr) + 20, h.info.age))
      continue;
    const uint64_t available = fileSize - rawPtr - 24;  // rawPtr + 24 <= fileSize: age was just read
    const size_t pathBytes =
        static_cast<size_t>(std::min<uint64_t>({sizeOfData - 24, kMaxPdbPath, available}));
    std::string path(pathBytes, '\0');
    if (pathBytes > 0 && !src.readRaw(static_cast<uint64_t>(rawPtr) + 24, path.data(), pathBytes)) continue;
    const size_t nul = path.find('\0');
    if (nul != std::string::npos) path.resize(nul);
    h.info.pdbPath = std::move(path);
    h.info.hasCodeView = true;
    break;
  }
  return h.info;
}

Result<PeInfo> parsePe(std::span<const std::byte> image) {
  PeReadFn read = [image](uint64_t offset, std::span<std::byte> dst) {
    if (offset > image.size() || image.size() - offset < dst.size()) return false;
    if (!dst.empty()) std::memcpy(dst.data(), image.data() + offset, dst.size());
    return true;
  };
  return parsePe(read, image.size());
}

Result<std::vector<PeFunctionRange>> readPeFunctionRanges(const PeReadFn& read, uint64_t fileSize) {
  Source src(read, fileSize);
  TACHYON_TRY_ASSIGN(Headers h, parseHeaders(src));
  std::vector<PeFunctionRange> out;
  uint32_t rva = 0;
  uint32_t size = 0;
  uint64_t offset = 0;
  if (!h.info.is64 || !dataDirectory(src, h, kDirException, rva, size)) return out;
  // RUNTIME_FUNCTION: BeginAddress, EndAddress, UnwindData (u32 each).
  const size_t count = std::min<size_t>(size / 12, kMaxFunctions);
  if (count == 0 || !rvaToOffset(h, rva, count * 12, offset)) return out;
  std::vector<uint32_t> raw(count * 3);
  if (!src.readRaw(offset, raw.data(), raw.size() * sizeof(uint32_t)))
    return makeError(ErrorCode::CorruptData, "truncated exception directory");
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) out.push_back(PeFunctionRange{raw[i * 3], raw[i * 3 + 1]});
  std::sort(out.begin(), out.end(),
            [](const PeFunctionRange& a, const PeFunctionRange& b) { return a.beginRva < b.beginRva; });
  return out;
}

namespace {
struct FileSource {
  std::ifstream file;
  uint64_t size = 0;
  PeReadFn reader() {
    return [this](uint64_t offset, std::span<std::byte> dst) {
      file.clear();
      file.seekg(static_cast<std::streamoff>(offset));
      file.read(reinterpret_cast<char*>(dst.data()), static_cast<std::streamsize>(dst.size()));
      return static_cast<bool>(file);
    };
  }
};

Result<void> openSource(const std::filesystem::path& path, FileSource& fs) {
  fs.file.open(path, std::ios::binary);
  if (!fs.file) return makeError(ErrorCode::NotFound, "cannot open " + path.string());
  fs.file.seekg(0, std::ios::end);
  const std::streamoff end = fs.file.tellg();
  if (end < 0) return makeError(ErrorCode::IoError, "cannot size " + path.string());
  fs.size = static_cast<uint64_t>(end);
  return ok();
}
}  // namespace

Result<PeInfo> parsePeFile(const std::filesystem::path& path) {
  FileSource fs;
  TACHYON_TRY(openSource(path, fs));
  return parsePe(fs.reader(), fs.size);
}

Result<std::vector<PeFunctionRange>> readPeFunctionRangesFromFile(const std::filesystem::path& path) {
  FileSource fs;
  TACHYON_TRY(openSource(path, fs));
  return readPeFunctionRanges(fs.reader(), fs.size);
}

}  // namespace tachyon::etlconvert
