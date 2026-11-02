// Source file locator (WP-34): maps the file path found in debug info (often a build machine path such as
// "C:\build\engine\src\a.cpp") to a file that exists on this machine. Lives in libs/symbols because the paths
// come from symbol files and the viewer's settings feed it; it depends on nothing but the standard library.
//
// Order of attempts (the first existing regular file wins):
//   1. the path as given;
//   2. the remap rules in order: a rule matches when its `from` prefix covers whole leading path components
//      of the debug path (separators '/' and '\' are interchangeable; the comparison is ASCII
//      case-insensitive when either side looks like a Windows path, i.e. has a drive letter or a backslash);
//      the unmatched rest is appended to `to`. A rule whose result does not exist, or that would leave `to`
//      (".." components or symlinks pointing out of it), is skipped and the next rule is tried;
//   3. the search roots: for the last n components of the debug path, n = all .. 1 (longest first), the file
//      `root/<those components>` for every root in order. No directory is ever walked.
// Results, including failures, are cached per debug path.
#pragma once

#include "core/result.h"

#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tachyon::symbols {

struct SourceRemapRule {
  std::string from;  // prefix of the debug path, e.g. "C:\build\engine"
  std::string to;    // replacement, e.g. "/home/me/engine"
};

struct SourceLocatorConfig {
  std::vector<SourceRemapRule> remaps;
  std::vector<std::filesystem::path> searchRoots;
};

class SourceLocator {
 public:
  static constexpr size_t kMaxCacheEntries = 4096;

  // Thread-safe (constructor).
  explicit SourceLocator(SourceLocatorConfig config = {});

  // Thread-safe. The local file for `debugPath`, or NotFound. Cached; call clearCache() after the files on
  // disk or the configuration changed.
  Result<std::filesystem::path> locate(std::string_view debugPath) const;

  // Thread-safe. Replaces the configuration and drops the cache.
  void configure(SourceLocatorConfig config);
  // Thread-safe.
  SourceLocatorConfig config() const;
  // Thread-safe.
  void clearCache();
  // Thread-safe. Number of cached results (at most kMaxCacheEntries; least recently used are evicted).
  size_t cacheSize() const;

 private:
  Result<std::filesystem::path> resolve(const SourceLocatorConfig& cfg, std::string_view debugPath) const;

  mutable std::mutex m_mutex;
  SourceLocatorConfig m_config;
  // LRU of kMaxCacheEntries: front = most recently used. debug path -> located path (empty = not found)
  struct CacheEntry {
    std::filesystem::path path;
    std::list<std::string>::iterator lru;
  };
  mutable std::list<std::string> m_lru;
  mutable std::unordered_map<std::string, CacheEntry> m_cache;
};

}  // namespace tachyon::symbols
