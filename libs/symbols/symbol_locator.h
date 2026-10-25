// Symbol file location (spec 3.6, WP-21): explicit paths -> next to the module -> local two-tier cache ->
// symbol servers (HTTP symbol servers, debuginfod, or local symbol stores) in order.
#pragma once

#include "core/result.h"
#include "symbols/http_client.h"
#include "symbols/symbol_provider.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::symbols {

using LocateFn = std::function<Result<std::filesystem::path>(const Module&)>;
// Cancellable variant: `cancel` (may be null) flips to true when the owner wants outstanding work to stop;
// the function should then return ErrorCode::Cancelled promptly.
using CancellableLocateFn =
    std::function<Result<std::filesystem::path>(const Module&, const std::atomic<bool>* cancel)>;

struct SymbolLocatorConfig {
  // Searched first. A directory is probed for the symbol file name, the symstore layout
  // (<dir>/<name>/<key>/<name>, also two-tier) and `.build-id/xx/rest.debug`; a file is used when its name
  // matches the wanted symbol file.
  std::vector<std::filesystem::path> searchPaths;
  // "http://" / "https://" symbol servers (symstore layout, or debuginfod for build-ids), or local symbol
  // store directories, consulted in order.
  std::vector<std::string> servers;
  // Local cache root (symstore layout, two-tier when <cacheDir>/index2.txt exists). Empty: a directory under
  // the system temp directory is used for downloads.
  std::filesystem::path cacheDir;
  // Cache size limit enforced after each download, least recently used (mtime) first; 0 = unlimited.
  uint64_t cacheMaxBytes = 0;
  // Upper bound on simultaneous HTTP transfers across all threads using this locator.
  unsigned maxConcurrentDownloads = 4;
  // Largest response body accepted from a server (streamed check; the part file is deleted on overflow).
  uint64_t maxDownloadBytes = 2ull * 1024 * 1024 * 1024;
  // Extra roots probed for `.build-id/xx/rest.debug` (ELF build-ids). Default: /usr/lib/debug on POSIX.
  std::vector<std::filesystem::path> debugDirs;
  bool useSystemDebugDirs = true;
  // HTTP backend; null = makeDefaultHttpClient(httpOptions) created on first use.
  std::shared_ptr<IHttpClient> http;
  HttpOptions httpOptions;
};

class SymbolLocator {
 public:
  // Thread-safe (constructors). Default: next-to-module search only.
  SymbolLocator();
  SymbolLocator(std::vector<std::string> servers, std::filesystem::path cacheDir);
  explicit SymbolLocator(SymbolLocatorConfig config);
  ~SymbolLocator();
  SymbolLocator(SymbolLocator&&) noexcept;
  SymbolLocator& operator=(SymbolLocator&&) noexcept;
  SymbolLocator(const SymbolLocator&) = delete;
  SymbolLocator& operator=(const SymbolLocator&) = delete;

  // Thread-safe. Finds the symbol file for `module` (PDB, ELF debug file or, for PE timestamp ids, the image
  // itself). NotFound carries the reasons ("compressed CAB not supported", file.ptr messages, server errors).
  // The returned path is pinned: it is never evicted from the cache until unpin() or destruction.
  // `cancel` (optional) aborts pending downloads (Cancelled).
  Result<std::filesystem::path> locate(const Module& module, const std::atomic<bool>* cancel = nullptr);

  // Thread-safe. Pinned files are in use by a live provider and are skipped by cache eviction. Counted:
  // every pin needs one unpin.
  void pin(const std::filesystem::path& file);
  void unpin(const std::filesystem::path& file);

  // Thread-safe. Runs the size-based cache eviction now; returns the bytes removed.
  uint64_t evictCache();

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Thread-safe. LocateFn for SymbolService::setLocate backed by a shared SymbolLocator.
LocateFn makeDefaultLocate(SymbolLocatorConfig config);
// Thread-safe. Cancellable LocateFn backed by a shared SymbolLocator (forwards `cancel` to locate()).
CancellableLocateFn makeDefaultCancellableLocate(SymbolLocatorConfig config);

}  // namespace tachyon::symbols
