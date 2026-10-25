#include "manifest.h"

#include "sha256.h"

#include <nlohmann/json.hpp>

namespace tachyon::tracegen {

Result<nlohmann::json> manifestEntry(const GenerateReport& report) {
  nlohmann::json entry = nlohmann::json::object();
  for (const auto& p : {report.tachyonPath, report.expectedPath, report.symbolsPath}) {
    TACHYON_TRY_ASSIGN(std::string hash, sha256File(p));
    entry[p.filename().string()] = hash;
  }
  return entry;
}

}  // namespace tachyon::tracegen
