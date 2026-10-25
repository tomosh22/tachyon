// Trace generation: streams a scenario through TraceWriter and computes the analytic expected results.
#pragma once

#include "core/result.h"
#include "scenario.h"
#include "tally.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace tachyon::tracegen {

struct GenerateOptions {
  bool verify = true;  // run the brute-force reference and require it to agree with the analytic result
};

struct GenerateReport {
  std::filesystem::path tachyonPath, expectedPath, symbolsPath;
  uint64_t tachyonBytes = 0;
  uint64_t samples = 0;
  double generateSeconds = 0;  // scenario -> .tachyon (includes the analytic bookkeeping)
  double verifySeconds = 0;    // brute-force reference + comparison
};

// Thread-safe: pure function of its inputs (writes only below outDir). Generates <outDir>/<name>.tachyon,
// <name>.symbols.json and <name>.expected.json. Fails when the reference and the analytic result disagree.
Result<GenerateReport> generateFixture(const Scenario& scenario, const std::filesystem::path& outDir,
                                       const GenerateOptions& options);

// Thread-safe: pure function. Builds the symbols sidecar document for a scenario.
nlohmann::json buildSymbolsJson(const Scenario& scenario);

// Thread-safe: pure function. Capture-info document embedded in the trace (see README).
std::string buildCaptureInfo(const Scenario& scenario);

// Thread-safe: pure function. Display name of a mapped function ("module!function").
std::string functionDisplayName(const Scenario& scenario, uint32_t module, uint32_t function);

// Thread-safe: pure function. "<unmapped>!0x..." for an absolute address.
std::string unmappedDisplayName(uint64_t address);

// Thread-safe: pure function. Writes a JSON document (UTF-8, '\n' line endings on every platform).
Result<void> writeJsonFile(const std::filesystem::path& path, const nlohmann::json& doc, int indent);

}  // namespace tachyon::tracegen
