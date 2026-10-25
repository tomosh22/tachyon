// Minimal SHA-256 (FIPS 180-4) used for the fixture manifest.
#pragma once

#include "core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tachyon::tracegen {

class Sha256 {
 public:
  // Not thread-safe.
  Sha256();
  // Not thread-safe.
  void update(const void* data, size_t size);
  // Not thread-safe. Finalizes and returns the lowercase hex digest; the object must not be reused.
  std::string finishHex();

 private:
  void block(const uint8_t* p);
  std::array<uint32_t, 8> m_h;
  std::array<uint8_t, 64> m_buf{};
  uint64_t m_total = 0;
  size_t m_fill = 0;
};

// Thread-safe: pure function. Lowercase hex SHA-256 of a whole file.
Result<std::string> sha256File(const std::filesystem::path& path);

}  // namespace tachyon::tracegen
