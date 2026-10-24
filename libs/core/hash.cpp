#include "core/hash.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace tachyon::core {

uint64_t hashBytes(const void* data, size_t size, uint64_t seed) {
  return XXH3_64bits_withSeed(data, size, seed);
}

}  // namespace tachyon::core
