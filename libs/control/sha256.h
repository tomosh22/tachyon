// SHA-256 and base64 for the `fetch` command (WP-61). Small self-contained implementations: the approved
// dependency list has no crypto library, and a transfer checksum is all that is needed.
#pragma once

#include "core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace tachyon::control {

// Incremental SHA-256. Not thread-safe (one instance per thread).
class Sha256 {
 public:
  Sha256();
  // Appends bytes.
  void update(const void* data, size_t size);
  // Finishes and returns the 32-byte digest. The object must not be used afterwards.
  std::array<uint8_t, 32> finish();

 private:
  void block(const uint8_t* p);
  uint32_t m_state[8];
  uint8_t m_buffer[64];
  uint64_t m_bytes = 0;
  size_t m_fill = 0;
};

// Thread-safe: pure function. Lower-case hex of `bytes`.
std::string toHex(const uint8_t* bytes, size_t size);

// Thread-safe: reads the file in 1 MiB blocks (bounded memory). Lower-case hex SHA-256 of the whole file.
Result<std::string> sha256OfFile(const std::filesystem::path& path);

// Thread-safe: pure function. Standard base64 with padding.
std::string base64Encode(const void* data, size_t size);
// Thread-safe: pure function. Standard base64; fails with InvalidArgument on bad characters or length.
Result<std::string> base64Decode(std::string_view text);

// Thread-safe: pure function. Compares in time that depends only on the lengths, not on where the inputs
// differ. For secrets.
bool constantTimeEquals(std::string_view a, std::string_view b);

}  // namespace tachyon::control
