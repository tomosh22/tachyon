// Sort that observes a cancel token (WP-53b). std::sort of a million elements takes tens of milliseconds
// without a single poll; this sorts chunks of kSortChunk elements and merges them pairwise, polling between
// the steps. Not part of the public API.
#pragma once

#include "analysis/query.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace tachyon::analysis::detail {

inline constexpr size_t kSortChunk = size_t{1} << 16;

// Thread-safe for distinct vectors. Sorts `v` by `less` (stable merges). Returns false if `cancel` fired; `v`
// is then valid but in unspecified order.
template <typename T, typename Less>
bool cancellableSort(std::vector<T>& v, Less less, Cancel cancel) {
  const size_t n = v.size();
  if (n <= kSortChunk) {
    if (cancel.requested()) return false;
    std::sort(v.begin(), v.end(), less);
    return true;
  }
  for (size_t i = 0; i < n; i += kSortChunk) {
    if (cancel.requested()) return false;
    std::sort(v.begin() + static_cast<std::ptrdiff_t>(i),
              v.begin() + static_cast<std::ptrdiff_t>(std::min(n, i + kSortChunk)), less);
  }
  for (size_t width = kSortChunk; width < n; width *= 2) {
    for (size_t i = 0; i + width < n; i += 2 * width) {
      if (cancel.requested()) return false;
      std::inplace_merge(v.begin() + static_cast<std::ptrdiff_t>(i),
                         v.begin() + static_cast<std::ptrdiff_t>(i + width),
                         v.begin() + static_cast<std::ptrdiff_t>(std::min(n, i + 2 * width)), less);
    }
  }
  return !cancel.requested();
}

}  // namespace tachyon::analysis::detail
