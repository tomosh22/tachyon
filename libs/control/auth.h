// Shared-secret authentication for TCP clients (WP-61). The agent reads the token from a file; clients send
// it in the first line of the connection: {"id":N,"cmd":"auth","token":"..."}. Local socket / pipe
// connections never authenticate. The token is a bearer secret on an unencrypted channel: it keeps strangers
// out, it does not protect against someone who can read the network traffic (use an SSH tunnel for that).
#pragma once

#include "core/result.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace tachyon::control {

// The agent refuses tokens shorter than this (`head -c 24 /dev/urandom | base64` gives 32 characters).
constexpr size_t kMinAgentTokenChars = 32;
// Longest token accepted anywhere (the auth line must fit comfortably in a protocol line).
constexpr size_t kMaxTokenChars = 512;

// Thread-safe: reads the file system. Reads the first line of `path` (trailing CR/LF/blanks removed).
// Fails with
//   NotFound         the file does not exist,
//   IoError          it cannot be read,
//   InvalidArgument  the token is empty, has fewer than `minChars` characters, exceeds kMaxTokenChars or
//                    contains control characters or blanks,
//   NoPrivilege      (POSIX) the file is readable or writable by group or others (mode & 077), so the secret
//                    is not secret (`chmod 600` fixes it), or it is owned by neither the effective user nor
//                    root. The checks run on the open descriptor (one open, O_NOFOLLOW): a symbolic link is
//                    refused (InvalidArgument) and nothing is decided by path before the open. Windows has no
//                    mode bits: keep the file in a profile directory.
// Error messages never contain the token.
Result<std::string> loadTokenFile(const std::filesystem::path& path, size_t minChars);

// Thread-safe: reads the environment. $TACHYON_AGENT_TOKEN_FILE or an empty path.
std::filesystem::path tokenFileFromEnvironment();

}  // namespace tachyon::control
