#include "control/framed_connection.h"

#include <chrono>

namespace tachyon::control::detail {

Result<bool> FramedConnection::readLine(std::string& line, int timeoutMs) {
  // The timeout bounds the whole line, not each read: a peer dribbling bytes must not hold the reader
  // forever.
  using Clock = std::chrono::steady_clock;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
  for (;;) {
    const size_t nl = m_buffer.find('\n', m_scanned);
    if (nl != std::string::npos) {
      if (nl > kMaxLineBytes) return makeError(ErrorCode::InvalidArgument, "line exceeds 1 MiB limit");
      size_t end = nl;
      if (end > 0 && m_buffer[end - 1] == '\r') --end;
      line.assign(m_buffer, 0, end);
      m_buffer.erase(0, nl + 1);
      m_scanned = 0;
      return true;
    }
    m_scanned = m_buffer.size();
    if (m_buffer.size() > kMaxLineBytes)
      return makeError(ErrorCode::InvalidArgument, "line exceeds 1 MiB limit");

    int waitMs = -1;
    if (timeoutMs >= 0) {
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0 && timeoutMs > 0) return makeError(ErrorCode::Timeout, "read timed out");
      waitMs = left <= 0 ? 0 : static_cast<int>(left);
    }
    char chunk[16384];
    auto n = readSome(chunk, sizeof(chunk), waitMs);
    if (!n.ok()) return std::move(n).error();
    if (*n == 0) return false;  // EOF; a trailing partial line is discarded
    m_buffer.append(chunk, *n);
  }
}

Result<void> FramedConnection::writeLine(std::string_view line) {
  std::string framed;
  framed.reserve(line.size() + 1);
  framed.append(line);
  framed.push_back('\n');
  std::lock_guard lock(m_writeMutex);
  return writeAll(framed.data(), framed.size());
}

}  // namespace tachyon::control::detail
