// Brute-force reference: re-reads an emitted .tachyon (plus its .symbols.json) and applies the Part 3.8 rules
// naively. It deliberately shares no derivation code with the generator's analytic bookkeeping.
#pragma once

#include "core/result.h"
#include "tally.h"
#include "trace/trace_reader.h"

#include <filesystem>

namespace tachyon::tracegen {

// Thread-safe: pure function of the two files. Memory use is bounded by the number of threads / intervals,
// not by the number of samples (samples are streamed chunk by chunk).
Result<Tally> computeReference(const std::filesystem::path& tachyonPath,
                               const std::filesystem::path& symbolsPath);

}  // namespace tachyon::tracegen
