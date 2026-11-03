#include "control/sha256.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace tachyon::control {

namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t rotr(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

Sha256::Sha256() {
  static constexpr uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(m_state, kInit, sizeof(m_state));
  std::memset(m_buffer, 0, sizeof(m_buffer));
}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t{p[i * 4]} << 24) | (uint32_t{p[i * 4 + 1]} << 16) | (uint32_t{p[i * 4 + 2]} << 8) |
           uint32_t{p[i * 4 + 3]};
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
  uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  m_state[0] += a;
  m_state[1] += b;
  m_state[2] += c;
  m_state[3] += d;
  m_state[4] += e;
  m_state[5] += f;
  m_state[6] += g;
  m_state[7] += h;
}

void Sha256::update(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  m_bytes += size;
  if (m_fill > 0) {
    const size_t take = std::min(size, sizeof(m_buffer) - m_fill);
    std::memcpy(m_buffer + m_fill, p, take);
    m_fill += take;
    p += take;
    size -= take;
    if (m_fill < sizeof(m_buffer)) return;
    block(m_buffer);
    m_fill = 0;
  }
  while (size >= 64) {
    block(p);
    p += 64;
    size -= 64;
  }
  if (size > 0) {
    std::memcpy(m_buffer, p, size);
    m_fill = size;
  }
}

std::array<uint8_t, 32> Sha256::finish() {
  const uint64_t bits = m_bytes * 8;
  const uint8_t one = 0x80;
  update(&one, 1);
  const uint8_t zero = 0;
  while (m_fill != 56) update(&zero, 1);
  uint8_t length[8];
  for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(length, 8);
  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(m_state[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(m_state[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(m_state[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(m_state[i]);
  }
  return out;
}

std::string toHex(const uint8_t* bytes, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 15]);
  }
  return out;
}

Result<std::string> sha256OfFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return makeError(ErrorCode::IoError, "cannot open " + path.string());
  Sha256 hash;
  std::vector<char> buffer(1 << 20);
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) hash.update(buffer.data(), static_cast<size_t>(got));
  }
  if (in.bad()) return makeError(ErrorCode::IoError, "read error on " + path.string());
  const auto digest = hash.finish();
  return toHex(digest.data(), digest.size());
}

std::string base64Encode(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const uint32_t v = (uint32_t{p[i]} << 16) | (uint32_t{p[i + 1]} << 8) | p[i + 2];
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back(kB64[v & 63]);
  }
  if (i + 1 == size) {
    const uint32_t v = uint32_t{p[i]} << 16;
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out += "==";
  } else if (i + 2 == size) {
    const uint32_t v = (uint32_t{p[i]} << 16) | (uint32_t{p[i + 1]} << 8);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

Result<std::string> base64Decode(std::string_view text) {
  if (text.size() % 4 != 0)
    return makeError(ErrorCode::InvalidArgument, "base64 length is not a multiple of 4");
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (size_t k = 0; k < 4; ++k) {
      const char c = text[i + k];
      if (c == '=') {
        // Padding only in the last two positions of the last group.
        if (i + 4 != text.size() || k < 2) return makeError(ErrorCode::InvalidArgument, "bad base64 padding");
        v[k] = 0;
        ++pad;
      } else {
        if (pad > 0) return makeError(ErrorCode::InvalidArgument, "bad base64 padding");
        v[k] = value(c);
        if (v[k] < 0) return makeError(ErrorCode::InvalidArgument, "bad base64 character");
      }
    }
    const uint32_t n = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                       (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
    out.push_back(static_cast<char>((n >> 16) & 0xFF));
    if (pad < 2) out.push_back(static_cast<char>((n >> 8) & 0xFF));
    if (pad < 1) out.push_back(static_cast<char>(n & 0xFF));
  }
  return out;
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
  // Always walks max(len) bytes; the length difference is folded into the accumulator.
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  volatile uint32_t diff = static_cast<uint32_t>(a.size() ^ b.size());
  for (size_t i = 0; i < n; ++i) {
    const uint8_t x = i < a.size() ? static_cast<uint8_t>(a[i]) : uint8_t{0};
    const uint8_t y = i < b.size() ? static_cast<uint8_t>(b[i]) : uint8_t{0};
    diff = diff | static_cast<uint32_t>(x ^ y);
  }
  return diff == 0;
}

}  // namespace tachyon::control
