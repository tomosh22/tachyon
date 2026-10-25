// Symbol server / debuginfod key formats for a Module (spec 3.6, WP-21).
#pragma once

#include "model/model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tachyon::symbols {

// Thread-safe: pure function. File name component of a Windows or POSIX path ("C:\x\ntdll.dll" ->
// "ntdll.dll").
std::string pathFileName(std::string_view path);
// Thread-safe: pure function. File name without its last extension ("ntdll.dll" -> "ntdll").
std::string pathStem(std::string_view path);

// Thread-safe: pure function. Symbol server directory key of a PDB: GUID in canonical Data1-Data2-Data3-Data4
// order (converted from the stored on-disk byte order) as 32 upper hex digits, then the age in upper hex with
// no leading zeros ("1B72224D37B8179228200ED8994498B22"). Empty unless id.kind is the PDB kind.
std::string pdbKey(const model::ModuleId& id);
// Thread-safe: pure function. Key of the image itself (id.kind 3): TimeDateStamp as 8 upper hex digits then
// SizeOfImage in upper hex, no leading zeros. Empty unless id.kind is the PE timestamp+size kind.
std::string peImageKey(const model::ModuleId& id);
// Thread-safe: pure function. GNU build-id as lowercase hex (id.kind 2); empty otherwise.
std::string buildIdHex(const model::ModuleId& id);

// Thread-safe: pure function. PDB file name for a module. The model does not carry the RSDS path, so this is
// "<image stem>.pdb" (a shared-model change could supply the real name; see the WP-21 report).
std::string pdbFileName(const model::Module& module);

// One thing to look for on a symbol server or in a symbol store.
struct SymbolKey {
  enum class Scheme : uint8_t { SymStore, DebugInfod };
  Scheme scheme = Scheme::SymStore;
  std::string name;  // file name (SymStore) or "debuginfo" (DebugInfod)
  std::string key;   // GUID+age / timestamp+size (SymStore) or lowercase build-id hex (DebugInfod)
  // Thread-safe: pure function. "<name>/<key>/<name>" or "buildid/<key>/debuginfo" (forward slashes).
  std::string relativePath() const;
};

// Thread-safe: pure function. The key to look up `module` by; nullopt for kind 0 or malformed ids.
std::optional<SymbolKey> symbolKeyFor(const model::Module& module);

}  // namespace tachyon::symbols
