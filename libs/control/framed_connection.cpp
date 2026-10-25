#include "control/framed_connection.h"

namespace tachyon::control::detail {

Result<bool> FramedConnection::readLine(std::string& line, int timeoutMs) {
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

    char chunk[16384];
    auto n = readSome(chunk, sizeof(chunk), timeoutMs);
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
