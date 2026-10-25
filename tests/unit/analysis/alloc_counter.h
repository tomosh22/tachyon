// Live heap byte counter of the analysis_tests binary (see alloc_counter.cpp).
#pragma once

#include <cstdint>

namespace tachyon::testutil {

// Thread-safe. Sum of the requested sizes of all live allocations made through operator new.
int64_t liveHeapBytes();

}  // namespace tachyon::testutil
