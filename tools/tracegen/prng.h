// Deterministic PRNG and hashing helpers. std::*_distribution is implementation-defined, so tracegen maps raw
// 64-bit words to ranges itself (integer arithmetic only) to stay byte-identical across platforms.
#pragma once

#include <cstdint>
#include <string_view>

namespace tachyon::tracegen {

// Thread-safe: pure function. Advances `state` and returns the next splitmix64 output.
constexpr uint64_t splitmix64(uint64_t& state) {
  state += 0x9e3779b97f4a7c15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// Thread-safe: pure function. FNV-1a 64-bit hash of a string.
constexpr uint64_t fnv1a(std::string_view s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= 0x100000001b3ull;
  }
  return h;
}

// Thread-safe: pure function. Derives an independent stream seed from the scenario seed, a tag and an id.
constexpr uint64_t streamSeed(uint64_t seed, std::string_view tag, uint64_t id = 0) {
  uint64_t s = seed ^ fnv1a(tag) ^ (id * 0xd6e8feb86659fd93ull);
  return splitmix64(s);
}

// xoshiro256** seeded through splitmix64. Not thread-safe (one instance per stream).
class Prng {
 public:
  explicit Prng(uint64_t seed) {
    uint64_t s = seed;
    for (uint64_t& w : m_s) w = splitmix64(s);
  }

  // Not thread-safe.
  uint64_t next() {
    const uint64_t result = rotl(m_s[1] * 5, 7) * 9;
    const uint64_t t = m_s[1] << 17;
    m_s[2] ^= m_s[0];
    m_s[3] ^= m_s[1];
    m_s[1] ^= m_s[2];
    m_s[0] ^= m_s[3];
    m_s[2] ^= t;
    m_s[3] = rotl(m_s[3], 45);
    return result;
  }

  // Not thread-safe. Value in [0, n); 0 when n == 0. Modulo bias is irrelevant at these magnitudes.
  uint64_t below(uint64_t n) { return n == 0 ? 0 : next() % n; }

  // Not thread-safe. Value in [lo, hi] (inclusive); lo when hi <= lo.
  uint64_t range(uint64_t lo, uint64_t hi) { return hi <= lo ? lo : lo + below(hi - lo + 1); }

 private:
  static constexpr uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t m_s[4];
};

}  // namespace tachyon::tracegen
