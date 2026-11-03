#include "control/safe_file.h"

#include "control/sha256.h"
#include "core/platform.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tachyon::control {

#if TACHYON_PLATFORM_WINDOWS

namespace {

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

Error winError(ErrorCode code, const std::string& what, DWORD err) {
  return makeError(code, what + " (Windows error " + std::to_string(err) + ")");
}

struct HandleInfo {
  FileIdentity identity;
  uint64_t size = 0;
  int64_t mtimeNs = 0;
  bool directory = false;
  bool reparse = false;
};

Result<HandleInfo> queryHandle(HANDLE h) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileInformationByHandle(h, &info) == 0)
    return winError(ErrorCode::IoError, "cannot query the file", GetLastError());
  HandleInfo out;
  out.identity.volume = info.dwVolumeSerialNumber;
  out.identity.index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  out.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  const uint64_t ft =
      (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
  out.mtimeNs = static_cast<int64_t>(ft) * 100;
  out.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  out.reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  return out;
}

}  // namespace

Result<FileIdentity> createExclusive(const std::filesystem::path& path, unsigned) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, kShareAll, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
      return makeError(ErrorCode::Busy, path.string() + " already exists");
    if (err == ERROR_PATH_NOT_FOUND)
      return makeError(ErrorCode::NotFound, "no such directory for " + path.string());
    return winError(ErrorCode::IoError, "cannot create " + path.string(), err);
  }
  auto info = queryHandle(h);
  CloseHandle(h);
  if (!info.ok()) return info.error();
  return info->identity;
}

Result<PathInfo> inspectPath(const std::filesystem::path& path) {
  HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
      return makeError(ErrorCode::NotFound, path.string() + " does not exist");
    return winError(ErrorCode::IoError, "cannot inspect " + path.string(), err);
  }
  auto info = queryHandle(h);
  CloseHandle(h);
  if (!info.ok()) return info.error();
  PathInfo out;
  out.identity = info->identity;
  out.symlink = info->reparse;
  out.regular = !info->directory && !info->reparse;
  return out;
}

SafeFile::~SafeFile() {
  if (m_handle != -1) CloseHandle(reinterpret_cast<HANDLE>(m_handle));
}

Result<SafeFile> SafeFile::openRead(const std::filesystem::path& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
      return makeError(ErrorCode::NotFound, path.string() + " does not exist");
    return winError(ErrorCode::IoError, "cannot open " + path.string(), err);
  }
  SafeFile f;
  f.m_handle = reinterpret_cast<intptr_t>(h);  // closed by the destructor from here on
  auto info = queryHandle(h);
  if (!info.ok()) return info.error();
  if (info->directory || info->reparse)
    return makeError(ErrorCode::InvalidArgument, path.string() + " is not a regular file");
  f.m_identity = info->identity;
  f.m_size = info->size;
  f.m_mtimeNs = info->mtimeNs;
  return f;
}

Result<size_t> SafeFile::readAt(uint64_t offset, void* buffer, size_t size) const {
  size_t total = 0;
  auto* out = static_cast<char*>(buffer);
  while (total < size) {
    OVERLAPPED ov{};
    const uint64_t at = offset + total;
    ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32);
    const DWORD want = static_cast<DWORD>(std::min<size_t>(size - total, size_t{1} << 30));
    DWORD got = 0;
    if (ReadFile(reinterpret_cast<HANDLE>(m_handle), out + total, want, &got, &ov) == 0) {
      const DWORD err = GetLastError();
      if (err == ERROR_HANDLE_EOF) break;
      return winError(ErrorCode::IoError, "read error", err);
    }
    if (got == 0) break;
    total += got;
  }
  return total;
}

#else  // POSIX

namespace {

FileIdentity identityOf(const struct stat& st) {
  return FileIdentity{static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino)};
}

int64_t mtimeOf(const struct stat& st) {
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000ll + st.st_mtim.tv_nsec;
}

}  // namespace

Result<FileIdentity> createExclusive(const std::filesystem::path& path, unsigned mode) {
  int fd;
  do {
    fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, static_cast<mode_t>(mode));
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    const int err = errno;
    // EEXIST also covers a dangling symlink: O_CREAT|O_EXCL never follows one.
    if (err == EEXIST) return makeError(ErrorCode::Busy, path.string() + " already exists");
    if (err == ENOENT || err == ENOTDIR)
      return makeError(ErrorCode::NotFound, "no such directory for " + path.string());
    return makeError(ErrorCode::IoError, "cannot create " + path.string() + ": " + std::strerror(err));
  }
  struct stat st {};
  const int rc = ::fstat(fd, &st);
  ::close(fd);
  if (rc != 0) return makeError(ErrorCode::IoError, "cannot stat " + path.string());
  return identityOf(st);
}

Result<PathInfo> inspectPath(const std::filesystem::path& path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      return makeError(ErrorCode::NotFound, path.string() + " does not exist");
    return makeError(ErrorCode::IoError, "cannot inspect " + path.string() + ": " + std::strerror(errno));
  }
  PathInfo info;
  info.identity = identityOf(st);
  info.regular = S_ISREG(st.st_mode);
  info.symlink = S_ISLNK(st.st_mode);
  return info;
}

SafeFile::~SafeFile() {
  if (m_handle != -1) ::close(static_cast<int>(m_handle));
}

Result<SafeFile> SafeFile::openRead(const std::filesystem::path& path) {
  int fd;
  do {
    fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    const int err = errno;
    if (err == ENOENT || err == ENOTDIR)
      return makeError(ErrorCode::NotFound, path.string() + " does not exist");
    if (err == ELOOP) return makeError(ErrorCode::InvalidArgument, path.string() + " is a symbolic link");
    return makeError(ErrorCode::IoError, "cannot open " + path.string() + ": " + std::strerror(err));
  }
  SafeFile f;
  f.m_handle = fd;  // closed by the destructor from here on
  struct stat st {};
  if (::fstat(fd, &st) != 0) return makeError(ErrorCode::IoError, "cannot stat " + path.string());
  if (!S_ISREG(st.st_mode))
    return makeError(ErrorCode::InvalidArgument, path.string() + " is not a regular file");
  f.m_identity = identityOf(st);
  f.m_size = static_cast<uint64_t>(st.st_size);
  f.m_mtimeNs = mtimeOf(st);
  return f;
}

Result<size_t> SafeFile::readAt(uint64_t offset, void* buffer, size_t size) const {
  size_t total = 0;
  auto* out = static_cast<char*>(buffer);
  while (total < size) {
    const ssize_t n =
        ::pread(static_cast<int>(m_handle), out + total, size - total, static_cast<off_t>(offset + total));
    if (n < 0) {
      if (errno == EINTR) continue;
      return makeError(ErrorCode::IoError, std::string("read error: ") + std::strerror(errno));
    }
    if (n == 0) break;
    total += static_cast<size_t>(n);
  }
  return total;
}

#endif

SafeFile::SafeFile(SafeFile&& other) noexcept
    : m_handle(other.m_handle),
      m_identity(other.m_identity),
      m_size(other.m_size),
      m_mtimeNs(other.m_mtimeNs) {
  other.m_handle = -1;
}

SafeFile& SafeFile::operator=(SafeFile&& other) noexcept {
  if (this != &other) {
    SafeFile release(std::move(*this));  // closes the current handle on scope exit
    m_handle = other.m_handle;
    m_identity = other.m_identity;
    m_size = other.m_size;
    m_mtimeNs = other.m_mtimeNs;
    other.m_handle = -1;
  }
  return *this;
}

Result<std::string> SafeFile::sha256() const {
  Sha256 hash;
  std::vector<char> buffer(1 << 20);
  uint64_t offset = 0;
  for (;;) {
    auto got = readAt(offset, buffer.data(), buffer.size());
    if (!got.ok()) return got.error();
    if (*got == 0) break;
    hash.update(buffer.data(), *got);
    offset += *got;
  }
  const auto digest = hash.finish();
  return toHex(digest.data(), digest.size());
}

}  // namespace tachyon::control
