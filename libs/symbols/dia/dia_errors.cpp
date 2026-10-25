#include "symbols/dia/dia_errors.h"

#include "trace/trace_format.h"

#include <cstdio>
#include <string>

namespace tachyon::symbols {

Error mapDiaLoadError(uint32_t hr, const std::filesystem::path& pdbFile) {
  using namespace dia_hr;
  ErrorCode code = ErrorCode::Internal;
  const char* what = "DIA error";
  switch (hr) {
    case kEPdbNotFound:
    case kFileNotFound:
    case kPathNotFound:
    case kEPdbDbgNotFound:
      code = ErrorCode::NotFound;
      what = "PDB file not found";
      break;
    case kEPdbInvalidSig:
      code = ErrorCode::NotFound;
      what = "PDB does not match the module (GUID mismatch)";
      break;
    case kEPdbInvalidAge:
      code = ErrorCode::NotFound;
      what = "PDB does not match the module (age mismatch)";
      break;
    case kEPdbInvalidExeTimestamp:
    case kEPdbInvalidExecutable:
      code = ErrorCode::NotFound;
      what = "PDB does not match the module (timestamp mismatch)";
      break;
    case kEPdbNoDebugInfo:
    case kEPdbDebugInfoNotInPdb:
      code = ErrorCode::NotFound;
      what = "no debug information";
      break;
    case kEPdbFormat:
    case kEPdbCorrupt:
    case kEPdbV1Pdb:
    case kEPdbTi16:
      code = ErrorCode::CorruptData;
      what = "unreadable or unsupported PDB format";
      break;
    case kEPdbLimit:
    case kEPdbNotImplemented:
      code = ErrorCode::Unsupported;
      what = "PDB feature not supported by DIA";
      break;
    case kEPdbFileSystem:
    case kEPdbAccessDenied:
    case kEAccessDenied:
      code = ErrorCode::IoError;
      what = "cannot read PDB";
      break;
    case kEPdbOutOfMemory:
    case kEOutOfMemory:
      code = ErrorCode::OutOfMemory;
      what = "out of memory while loading PDB";
      break;
    case kEPdbUsage:
    case kEInvalidArg:
      code = ErrorCode::InvalidArgument;
      what = "invalid argument";
      break;
    case kClassNotRegistered:
      code = ErrorCode::Unsupported;
      what = "msdia140.dll is not usable";
      break;
    default: break;
  }
  char hex[16];
  std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(hr));
  return makeError(code, std::string(what) + " [" + pdbFile.string() + ", HRESULT " + hex + "]");
}

std::optional<DiaPdbIdentity> diaIdentityFor(const model::ModuleId& id) {
  if (id.kind != trace::kModuleIdPdb || id.len != 16) return std::nullopt;
  DiaPdbIdentity out;
  for (size_t i = 0; i < 16; ++i) out.guid[i] = id.bytes[i];
  out.age = id.age;
  return out;
}

}  // namespace tachyon::symbols
