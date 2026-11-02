// x86 disassembly (WP-34): a thin Zydis wrapper plus reading code bytes from a module's file on disk.
// Independent of libs/symbols: call targets are symbolized through a caller-supplied callback.
#pragma once

#include "core/result.h"
#include "model/model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tachyon::disasm {

inline constexpr size_t kMaxInstructionLength = 15;

struct Instruction {
  uint64_t rva = 0;                                    // module-relative address of the first byte
  uint8_t length = 0;                                  // 1..15
  std::array<uint8_t, kMaxInstructionLength> bytes{};  // the first `length` entries are valid
  // Intel syntax, lower case, e.g. "mov rbp, rsp". Relative branch/call targets are printed as absolute RVAs
  // ("call 0x1230"), followed by " <name>" when the symbolizer returned a name.
  std::string text;
  bool valid = true;  // false: the bytes did not decode; text is "db 0x.." and length is 1
  bool isCall = false;
  bool isBranch = false;   // jmp / jcc / loop / jrcxz (direct or indirect), not calls or returns
  bool hasTarget = false;  // a direct (relative immediate) call or branch: targetRva is set
  uint64_t targetRva = 0;
};

// Returns a display name for the code at `targetRva` (e.g. the function containing it), or an empty string.
using SymbolizeFn = std::function<std::string(uint64_t targetRva)>;

// Thread-safe (no shared state). Decodes `bytes` as code located at `baseRva` (bitness 64 or 32; any other
// value gives an empty result). Never fails: undecodable bytes (garbage, or a truncated final instruction)
// become one-byte `db 0x..` pseudo instructions with valid = false and decoding resumes at the next byte.
// `symbolize` (may be empty) is consulted for the direct target of every call and branch.
std::vector<Instruction> disassemble(std::span<const uint8_t> bytes, uint64_t baseRva, int bitness = 64,
                                     const SymbolizeFn& symbolize = {});

// Thread-safe. Reads up to `size` bytes at module-relative address `rva` from the module image file `path`.
// PE files map an RVA through the section headers (bytes inside a section's virtual size but beyond its raw
// data read as zero; the headers map to themselves). ELF64 files follow the WP-72 convention: RVA = address
// minus the load address of the lowest PT_LOAD, so the ELF virtual address is
// `rva + (lowest PT_LOAD p_vaddr rounded down to 4096)`, which is mapped through PT_LOAD (bytes beyond
// p_filesz within p_memsz read as zero). The result may be shorter than `size` where the mapping ends (or the
// file is truncated); NotFound when `rva` itself is not mapped, Unsupported for other file formats,
// CorruptData for malformed headers, IoError when the file cannot be read.
Result<std::vector<uint8_t>> readImageBytes(const std::filesystem::path& path, uint64_t rva, size_t size);

// Thread-safe. readImageBytes on `module.path`, or on `pathOverride` when it is not empty (the module path of
// a trace names the capture machine's file, which may live elsewhere here).
Result<std::vector<uint8_t>> readModuleBytes(const model::Module& module, uint64_t rva, size_t size,
                                             const std::filesystem::path& pathOverride = {});

}  // namespace tachyon::disasm
