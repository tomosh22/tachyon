// SHA-256 manifest of generated fixtures (tests/fixtures/tracegen/manifest.json).
#pragma once

#include "core/result.h"
#include "generator.h"

#include <nlohmann/json_fwd.hpp>

namespace tachyon::tracegen {

// Thread-safe: pure function. {"<name>.tachyon": sha256, "<name>.expected.json": sha256, ...} for one
// scenario.
Result<nlohmann::json> manifestEntry(const GenerateReport& report);

}  // namespace tachyon::tracegen
