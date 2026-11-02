#include "viewer/status/symbol_status.h"

#include "trace/trace_format.h"

#include <cstdio>

namespace tachyon::viewer {

using symbols::SymbolState;

std::vector<bool> referencedModules(const model::TraceModel& model) {
  std::vector<bool> referenced(model.modules.size(), false);
  for (const model::Frame& f : model.frames) {
    if (f.module.valid() && f.module.get() < referenced.size()) referenced[f.module.get()] = true;
  }
  return referenced;
}

SymbolSummary summarizeSymbols(const symbols::SymbolService& service, const std::vector<bool>& referenced) {
  SymbolSummary s;
  const symbols::SymbolProgress p = service.progress();
  s.total = p.total;
  s.done = p.done;
  for (size_t i = 0; i < referenced.size(); ++i) {
    if (!referenced[i]) continue;
    switch (service.moduleState(ModuleIdx(static_cast<uint32_t>(i)))) {
      case SymbolState::Resolved: ++s.resolved; break;
      case SymbolState::NotFound:
      case SymbolState::Failed: ++s.failed; break;
      case SymbolState::Loading: ++s.loading; break;
      case SymbolState::Pending: break;
    }
  }
  return s;
}

std::string moduleFileName(const std::string& path) {
  const size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? path : path.substr(cut + 1);
}

const char* symbolStateName(SymbolState state) {
  switch (state) {
    case SymbolState::Pending: return "Pending";
    case SymbolState::Loading: return "Loading";
    case SymbolState::Resolved: return "Resolved";
    case SymbolState::NotFound: return "NotFound";
    case SymbolState::Failed: return "Failed";
  }
  return "?";
}

std::vector<ModuleSymbolRow> moduleSymbolRows(const model::TraceModel& model,
                                              const symbols::SymbolService& service,
                                              const std::vector<bool>& referenced, bool onlyUnresolved) {
  std::vector<ModuleSymbolRow> rows;
  for (size_t i = 0; i < model.modules.size() && i < referenced.size(); ++i) {
    if (!referenced[i]) continue;
    const ModuleIdx idx(static_cast<uint32_t>(i));
    const SymbolState state = service.moduleState(idx);
    if (onlyUnresolved && state == SymbolState::Resolved) continue;
    ModuleSymbolRow row;
    row.module = idx;
    row.path = model.modules[i].path;
    row.name = moduleFileName(row.path);
    row.state = state;
    if (state == SymbolState::NotFound || state == SymbolState::Failed) row.detail = service.moduleError(idx);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string moduleInfoText(const model::Module& module, const ModuleSymbolRow& row) {
  std::string out =
      "Module: " + row.name + "\nPath: " + module.path + "\nState: " + symbolStateName(row.state);
  if (!row.detail.empty()) out += "\nReason: " + row.detail;
  const model::ModuleId& id = module.id;
  const char* kind = "none";
  switch (id.kind) {
    case trace::kModuleIdPdb: kind = "PDB GUID+age"; break;
    case trace::kModuleIdGnuBuildId: kind = "GNU build-id"; break;
    case trace::kModuleIdPeTimestampSize: kind = "PE timestamp+size"; break;
    default: break;
  }
  out += std::string("\nId kind: ") + kind;
  if (id.len > 0) {
    std::string hex;
    char buf[3];
    for (uint8_t i = 0; i < id.len && i < id.bytes.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%02x", id.bytes[i]);
      hex += buf;
    }
    out += "\nId: " + hex;
    if (id.age != 0) out += "\nAge: " + std::to_string(id.age);
  }
  char base[64];
  std::snprintf(base, sizeof base, "\nBase: 0x%llx  Size: 0x%llx",
                static_cast<unsigned long long>(module.base), static_cast<unsigned long long>(module.size));
  out += base;
  return out;
}

}  // namespace tachyon::viewer
