// Brute-force reference: re-reads an emitted .tachyon (plus its .symbols.json) and applies the Part 3.8 rules
// naively. It deliberately shares no derivation code with the generator's analytic bookkeeping.
#pragma once

#include "core/result.h"
#include "tally.h"
#include "trace/trace_reader.h"

#include <filesystem>

namespace tachyon::tracegen {

struct ModuleInfo {
  uint64_t base = 0, size = 0;
  uint32_t age = 0;
  uint8_t idKind = 0, idLen = 0;
};

// Not thread-safe (uses the reader's file cursor). Decodes the Modules chunks without going through
// std::vector<ModuleRec>: the 78-byte packed record makes StrongId members misaligned (UBSan flags it).
Result<std::vector<ModuleInfo>> readModuleInfos(trace::TraceReader& reader);

// Thread-safe: pure function of the two files. Memory use is bounded by the number of threads / intervals,
// not by the number of samples (samples are streamed chunk by chunk).
Result<Tally> computeReference(const std::filesystem::path& tachyonPath,
                               const std::filesystem::path& symbolsPath);

}  // namespace tachyon::tracegen
