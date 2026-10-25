// Platform-independent PE (portable executable) reader: the parts needed to identify a module.
#pragma once

#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tachyon::etlconvert {

struct PeFunctionRange {
  uint32_t beginRva = 0;
  uint32_t endRva = 0;
};

// Identity of a PE image as stored in its headers.
struct PeInfo {
  bool is64 = false;
  uint16_t machine = 0;
  uint32_t timeDateStamp = 0;  // IMAGE_FILE_HEADER.TimeDateStamp
  uint32_t sizeOfImage = 0;    // IMAGE_OPTIONAL_HEADER.SizeOfImage
  uint32_t checksum = 0;
  bool hasCodeView = false;  // an IMAGE_DEBUG_TYPE_CODEVIEW "RSDS" record was found
  uint8_t guid[16] = {};     // PDB GUID in on-disk (GUID struct, little-endian) byte order
  uint32_t age = 0;
  std::string pdbPath;
};

// Random access to the file image: copies dst.size() bytes at `offset`; false if out of range.
using PeReadFn = std::function<bool(uint64_t offset, std::span<std::byte> dst)>;

// Thread-safe: pure function (the reader callback must be callable). Parses DOS header, NT headers, section
// table and the CodeView entry of the debug directory. A PE without a CodeView record yields hasCodeView =
// false (not an error). Every read is bounds-checked against `fileSize`; malformed input returns CorruptData.
Result<PeInfo> parsePe(const PeReadFn& read, uint64_t fileSize);
// Thread-safe: pure function.
Result<PeInfo> parsePe(std::span<const std::byte> image);
// Thread-safe: reads a file. NotFound when it cannot be opened.
Result<PeInfo> parsePeFile(const std::filesystem::path& path);

// Thread-safe: pure function. Reads the x64 exception directory (.pdata RUNTIME_FUNCTION table) as function
// extents, sorted by begin RVA. Empty for images without one.
Result<std::vector<PeFunctionRange>> readPeFunctionRanges(const PeReadFn& read, uint64_t fileSize);
// Thread-safe: reads a file.
Result<std::vector<PeFunctionRange>> readPeFunctionRangesFromFile(const std::filesystem::path& path);

}  // namespace tachyon::etlconvert
