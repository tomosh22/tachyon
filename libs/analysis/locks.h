// Lock summary helpers (WP-53). The query itself (lockSummary, LockSummary, LockRow) is declared in query.h.
#pragma once

#include "analysis/query.h"

#include <cstdint>
#include <string_view>

namespace tachyon::analysis {

// Thread-safe: pure function. Parses the data of a lock-wait scope, "0x<hex>" (the "0x" is optional, case
// insensitive, up to 16 digits, surrounding blanks tolerated). Returns false (and leaves `address` alone)
// when there is no valid address.
bool parseLockAddress(std::string_view text, uint64_t& address);

}  // namespace tachyon::analysis
