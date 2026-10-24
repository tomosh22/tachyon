// Byte-span helpers for decoding/encoding little-endian packed records safely (no unaligned or aliasing UB).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tachyon::core {

// Thread-safe: pure function.
template <typename T>
std::span<const std::byte> asBytes(const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
}

// Thread-safe: pure function.
template <typename T>
std::span<const std::byte> asBytes(std::span<const T> values) {
  static_assert(std::is_trivially_copyable_v<T>);
  return std::as_bytes(values);
}

// Thread-safe: pure function. Copies a T from bytes at offset; nullopt if out of range.
template <typename T>
std::optional<T> readPod(std::span<const std::byte> bytes, size_t offset = 0) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return std::nullopt;
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

// Thread-safe: pure function. Checked subspan; empty optional if out of range.
inline std::optional<std::span<const std::byte>> subspanChecked(std::span<const std::byte> bytes,
                                                                size_t offset, size_t count) {
  if (offset > bytes.size() || bytes.size() - offset < count) return std::nullopt;
  return bytes.subspan(offset, count);
}

// Thread-safe: pure function. Builds a span over a byte literal/array, for tests and fixtures.
inline std::span<const std::byte> bytesOf(const uint8_t* data, size_t size) {
  return {reinterpret_cast<const std::byte*>(data), size};
}

// Not thread-safe (mutates `out`). Appends the object representation of value.
template <typename T>
void appendPod(std::vector<std::byte>& out, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const size_t at = out.size();
  out.resize(at + sizeof(T));
  std::memcpy(out.data() + at, &value, sizeof(T));
}

// Not thread-safe (mutates `out`).
inline void appendBytes(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

// Sequential little-endian reader over a byte span. Not thread-safe (has a cursor).
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

  template <typename T>
  std::optional<T> read() {
    auto v = readPod<T>(m_bytes, m_offset);
    if (v) m_offset += sizeof(T);
    return v;
  }
  std::optional<std::span<const std::byte>> readBytes(size_t count) {
    auto s = subspanChecked(m_bytes, m_offset, count);
    if (s) m_offset += count;
    return s;
  }
  bool skip(size_t count) { return readBytes(count).has_value(); }
  size_t offset() const { return m_offset; }
  size_t remaining() const { return m_bytes.size() - m_offset; }
  bool atEnd() const { return m_offset == m_bytes.size(); }

 private:
  std::span<const std::byte> m_bytes;
  size_t m_offset = 0;
};

}  // namespace tachyon::core
