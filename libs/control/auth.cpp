#include "control/auth.h"

#include "core/platform.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#if !TACHYON_PLATFORM_WINDOWS
#include <sys/stat.h>
#endif

namespace tachyon::control {

Result<std::string> loadTokenFile(const std::filesystem::path& path, size_t minChars) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return makeError(ErrorCode::NotFound, "token file " + path.string() + " does not exist");
#if !TACHYON_PLATFORM_WINDOWS
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0)
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
#endif
  std::ifstream in(path, std::ios::binary);
  if (!in) return makeError(ErrorCode::IoError, "cannot read the token file " + path.string());
  std::string line;
  // A generous cap so a wrong file (a binary, a log) is not slurped; the token is the first line.
  char chunk[kMaxTokenChars + 64];
  in.read(chunk, sizeof(chunk));
  line.assign(chunk, static_cast<size_t>(in.gcount()));
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
  const char* env = std::getenv("TACHYON_AGENT_TOKEN_FILE");
  return env != nullptr ? std::filesystem::path(env) : std::filesystem::path();
}

}  // namespace tachyon::control
