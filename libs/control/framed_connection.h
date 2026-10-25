// Internal: shared newline framing on top of a raw byte stream. Not part of the public API.
#pragma once

#include "control/transport.h"

#include <mutex>
#include <string>

namespace tachyon::control::detail {

class FramedConnection : public IConnection {
 public:
  Result<bool> readLine(std::string& line, int timeoutMs) override;
  Result<void> writeLine(std::string_view line) override;

 protected:
  // Reads up to n bytes. Returns the byte count; 0 means EOF. Fails with ErrorCode::Timeout on timeout.
  virtual Result<size_t> readSome(char* buffer, size_t n, int timeoutMs) = 0;
  // Writes all n bytes or fails.
  virtual Result<void> writeAll(const char* data, size_t n) = 0;

 private:
  std::string m_buffer;
  size_t m_scanned = 0;
  std::mutex m_writeMutex;
};

}  // namespace tachyon::control::detail
