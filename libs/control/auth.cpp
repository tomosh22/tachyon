#include "control/auth.h"

#include "core/platform.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if !TACHYON_PLATFORM_WINDOWS
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tachyon::control {

Result<std::string> loadTokenFile(const std::filesystem::path& path, size_t minChars) {
  // Read the first bytes: a generous cap so a wrong file (a binary, a log) is not slurped; the token is the
  // first line.
  char chunk[kMaxTokenChars + 64];
  size_t got = 0;
#if !TACHYON_PLATFORM_WINDOWS
  // One open, every check on the descriptor: nothing is decided by path and then re-opened, so the file
  // that is checked is the file that is read. O_NOFOLLOW refuses a symlink in the last component.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (fd < 0) {
    const int err = errno;
    if (err == ENOENT || err == ENOTDIR)
      return makeError(ErrorCode::NotFound, "token file " + path.string() + " does not exist");
    if (err == ELOOP)
      return makeError(ErrorCode::InvalidArgument,
                       "token file " + path.string() + " is a symbolic link; point to the file itself");
    return makeError(ErrorCode::IoError,
                     "cannot open the token file " + path.string() + ": " + std::strerror(err));
  }
  struct FdCloser {
    int fd;
    ~FdCloser() { ::close(fd); }
  } closer{fd};
  struct stat st {};
  if (::fstat(fd, &st) != 0)
    return makeError(ErrorCode::IoError, "cannot stat the token file " + path.string());
  if (!S_ISREG(st.st_mode))
    return makeError(ErrorCode::InvalidArgument, "token file " + path.string() + " is not a regular file");
  if ((st.st_mode & 077) != 0) {
    char mode[16];
    std::snprintf(mode, sizeof(mode), "%04o", static_cast<unsigned>(st.st_mode & 07777));
    return makeError(ErrorCode::NoPrivilege, "token file " + path.string() +
                                                 " is accessible by group or others (mode " + mode +
                                                 "); run: chmod 600 " + path.string());
  }
  // A secret in someone else's file is not ours: only this user (or root) could have written it.
  if (st.st_uid != ::geteuid() && st.st_uid != 0)
    return makeError(ErrorCode::NoPrivilege,
                     "token file " + path.string() + " is owned by uid " + std::to_string(st.st_uid) +
                         ", not by the current user; run: chown $(id -u) " + path.string());
  while (got < sizeof(chunk)) {
    const ssize_t n = ::read(fd, chunk + got, sizeof(chunk) - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      return makeError(ErrorCode::IoError, "cannot read the token file " + path.string());
    }
    if (n == 0) break;
    got += static_cast<size_t>(n);
  }
#else
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return makeError(ErrorCode::NotFound, "token file " + path.string() + " does not exist");
  std::ifstream in(path, std::ios::binary);
  if (!in) return makeError(ErrorCode::IoError, "cannot read the token file " + path.string());
  in.read(chunk, sizeof(chunk));
  got = static_cast<size_t>(in.gcount());
#endif
  std::string line(chunk, got);
  const size_t nl = line.find_first_of("\r\n");
  if (nl != std::string::npos) line.resize(nl);
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
  // A UTF-8 byte order mark from an editor.
  if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
  if (line.empty()) return makeError(ErrorCode::InvalidArgument, "token file " + path.string() + " is empty");
  if (line.size() > kMaxTokenChars)
    return makeError(ErrorCode::InvalidArgument, "the token in " + path.string() + " is longer than " +
                                                     std::to_string(kMaxTokenChars) + " characters");
  if (line.size() < minChars)
    return makeError(ErrorCode::InvalidArgument, "the token in " + path.string() + " has only " +
                                                     std::to_string(line.size()) + " characters; at least " +
                                                     std::to_string(minChars) + " are required");
  for (const char c : line)
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F)
      return makeError(ErrorCode::InvalidArgument,
                       "the token in " + path.string() + " contains blanks or control characters");
  return line;
}

std::filesystem::path tokenFileFromEnvironment() {
#if TACHYON_PLATFORM_WINDOWS
  char* buffer = nullptr;
  size_t length = 0;
  std::filesystem::path result;
  if (_dupenv_s(&buffer, &length, "TACHYON_AGENT_TOKEN_FILE") == 0 && buffer != nullptr) result = buffer;
  std::free(buffer);
  return result;
#else
  const char* env = std::getenv("TACHYON_AGENT_TOKEN_FILE");
  return env != nullptr ? std::filesystem::path(env) : std::filesystem::path();
#endif
}

}  // namespace tachyon::control
