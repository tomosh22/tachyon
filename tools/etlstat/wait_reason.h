// KWAIT_REASON names and the UI wait-category mapping (provisional; WP-11 owns the final mapping).
#pragma once

#include <cstdint>
#include <string_view>

namespace tachyon::etlstat {

enum class WaitCategory : uint8_t { Synchronization, Io, Memory, Preempted, Other };

// Thread-safe: pure function. KWAIT_REASON name ("Executive", "WrQueue", ...) or "Unknown".
std::string_view waitReasonName(int8_t reason);
// Thread-safe: pure function.
WaitCategory waitCategory(int8_t reason);
// Thread-safe: pure function.
std::string_view waitCategoryName(WaitCategory category);

}  // namespace tachyon::etlstat
