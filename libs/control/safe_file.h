// File helpers for the agent's output and fetch paths (WP-66): create a file without ever following a link
// planted by another local user, and read a file through one open handle whose identity can be compared with
// the file that was produced. POSIX: O_CREAT|O_EXCL|O_NOFOLLOW and fstat. Windows: CREATE_NEW and
// FILE_FLAG_OPEN_REPARSE_POINT with GetFileInformationByHandle (needs Windows verification).
#pragma once

#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tachyon::control {

// Identifies a file on a volume (st_dev/st_ino; volume serial and file index on Windows).
struct FileIdentity {
  uint64_t volume = 0;
  uint64_t index = 0;
  bool operator==(const FileIdentity& o) const { return volume == o.volume && index == o.index; }
  bool operator!=(const FileIdentity& o) const { return !(*this == o); }
  bool operator<(const FileIdentity& o) const {
    return volume != o.volume ? volume < o.volume : index < o.index;
  }
};

// Thread-safe. Creates `path` as a new empty regular file and fails if anything is there already: a file, a
// directory or a symbolic link, dangling or not (O_CREAT|O_EXCL|O_NOFOLLOW; CREATE_NEW). Fails with
//   Busy       something exists at `path` (the caller picks another name),
//   NotFound   the directory is missing,
//   IoError    anything else.
// Returns the identity of the new file. `mode` is the POSIX permission before the umask (ignored on Windows).
Result<FileIdentity> createExclusive(const std::filesystem::path& path, unsigned mode = 0644);

// What is at a path, looked at without following a final symbolic link.
struct PathInfo {
  FileIdentity identity;
  bool regular = false;
  bool symlink = false;
};
// Thread-safe. Fails with NotFound when nothing is there.
Result<PathInfo> inspectPath(const std::filesystem::path& path);

// A regular file opened for reading. Move-only. readAt is positional, so it shares no file offset.
class SafeFile {
 public:
  SafeFile() = default;
  ~SafeFile();
  SafeFile(SafeFile&& other) noexcept;
  SafeFile& operator=(SafeFile&& other) noexcept;
  SafeFile(const SafeFile&) = delete;
  SafeFile& operator=(const SafeFile&) = delete;

  // Thread-safe. Opens `path` for reading without following a final symbolic link. Fails with NotFound when
  // it is missing, InvalidArgument when it is a symlink or not a regular file, IoError otherwise.
  static Result<SafeFile> openRead(const std::filesystem::path& path);

  // Properties of the open handle (fstat), taken when the file was opened. Thread-safe.
  FileIdentity identity() const { return m_identity; }
  uint64_t size() const { return m_size; }
  int64_t mtimeNs() const { return m_mtimeNs; }

  // Thread-safe. Reads up to `size` bytes at `offset`; returns the number read (less than `size` only at the
  // end of the file).
  Result<size_t> readAt(uint64_t offset, void* buffer, size_t size) const;
  // Thread-safe. Lower-case hex SHA-256 of the whole file, read through this handle in 1 MiB blocks.
  Result<std::string> sha256() const;

 private:
  intptr_t m_handle = -1;  // fd, or a HANDLE on Windows
  FileIdentity m_identity;
  uint64_t m_size = 0;
  int64_t m_mtimeNs = 0;
};

}  // namespace tachyon::control
