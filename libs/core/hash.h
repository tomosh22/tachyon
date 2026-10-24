// xxHash wrapper (XXH3) used for interning and cache keys.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace tachyon::core {

// Thread-safe: pure function. XXH3 64-bit hash of a byte range.
uint64_t hashBytes(const void* data, size_t size, uint64_t seed = 0);

// Thread-safe: pure function.
inline uint64_t hashString(std::string_view s, uint64_t seed = 0) {
  return hashBytes(s.data(), s.size(), seed);
}

// Thread-safe: pure function. Hash of a trivially copyable value's object representation.
template <typename T>
uint64_t hashPod(const T& value, uint64_t seed = 0) {
  static_assert(std::is_trivially_copyable_v<T>);
  return hashBytes(&value, sizeof(T), seed);
}

// Thread-safe: pure function. Fast, well-mixed hash of a single 64-bit key (for open-addressing tables).
constexpr uint64_t hashU64(uint64_t x) {
  // xxHash/murmur-style avalanche finalizer.
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

// Thread-safe: pure function. Hash of a pair of 32-bit keys, e.g. (parent, frame) in the callstack interner.
constexpr uint64_t hashU32Pair(uint32_t a, uint32_t b) {
  return hashU64((static_cast<uint64_t>(a) << 32) | b);
}

// Thread-safe: pure function. Order-dependent combination of two hashes.
constexpr uint64_t hashCombine(uint64_t seed, uint64_t value) {
  return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
}

}  // namespace tachyon::core
