// DIA error mapping and PDB identity conversion (WP-22). Platform-neutral so it is unit-tested on Linux.
#pragma once

#include "core/result.h"
#include "model/model.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace tachyon::symbols {

// HRESULTs of the E_PDB_* family (cvconst.h / DIA SDK) and the Win32 ones DIA forwards.
namespace dia_hr {
inline constexpr uint32_t kEPdbUsage = 0x806D0001u;
inline constexpr uint32_t kEPdbOutOfMemory = 0x806D0002u;
inline constexpr uint32_t kEPdbFileSystem = 0x806D0003u;
inline constexpr uint32_t kEPdbNotFound = 0x806D0004u;
inline constexpr uint32_t kEPdbInvalidSig = 0x806D0005u;
inline constexpr uint32_t kEPdbInvalidAge = 0x806D0006u;
inline constexpr uint32_t kEPdbNotImplemented = 0x806D0009u;
inline constexpr uint32_t kEPdbV1Pdb = 0x806D000Au;
inline constexpr uint32_t kEPdbFormat = 0x806D000Bu;
inline constexpr uint32_t kEPdbLimit = 0x806D000Cu;
inline constexpr uint32_t kEPdbCorrupt = 0x806D000Du;
inline constexpr uint32_t kEPdbTi16 = 0x806D000Eu;
inline constexpr uint32_t kEPdbAccessDenied = 0x806D000Fu;
inline constexpr uint32_t kEPdbInvalidExecutable = 0x806D0011u;
inline constexpr uint32_t kEPdbDbgNotFound = 0x806D0012u;
inline constexpr uint32_t kEPdbNoDebugInfo = 0x806D0013u;
inline constexpr uint32_t kEPdbInvalidExeTimestamp = 0x806D0014u;
inline constexpr uint32_t kEPdbDebugInfoNotInPdb = 0x806D0016u;
inline constexpr uint32_t kEAccessDenied = 0x80070005u;
inline constexpr uint32_t kEOutOfMemory = 0x8007000Eu;
inline constexpr uint32_t kEInvalidArg = 0x80070057u;
inline constexpr uint32_t kFileNotFound = 0x80070002u;  // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
inline constexpr uint32_t kPathNotFound = 0x80070003u;  // HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
inline constexpr uint32_t kClassNotRegistered = 0x80040154u;
}  // namespace dia_hr

// Thread-safe: pure function. Maps the HRESULT of loadAndValidateDataFromPdb / openSession to an Error:
// missing file, GUID / age / timestamp mismatch and "no debug info" -> NotFound (message says which); damaged
// or unsupported-format PDBs -> CorruptData; access / filesystem failures -> IoError; out of memory ->
// OutOfMemory; bad arguments -> InvalidArgument; msdia140 not usable -> Unsupported; anything else ->
// Internal. The message always carries the file and the hex HRESULT.
Error mapDiaLoadError(uint32_t hresult, const std::filesystem::path& pdbFile);

// A PDB identity (GUID in on-disk byte order, as in ModuleId) ready for loadAndValidateDataFromPdb.
struct DiaPdbIdentity {
  std::array<uint8_t, 16> guid{};
  uint32_t age = 0;
};

// Thread-safe: pure function. The identity to validate against, or nullopt when the module has no (usable)
// PDB id (kind other than trace::kModuleIdPdb, or a length other than 16).
std::optional<DiaPdbIdentity> diaIdentityFor(const model::ModuleId& id);

}  // namespace tachyon::symbols
