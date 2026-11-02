#include "symbols/source_locator.h"

#include <algorithm>
#include <system_error>

namespace tachyon::symbols {

namespace fs = std::filesystem;

namespace {

bool isSep(char c) {
  return c == '/' || c == '\\';
}

bool looksWindows(std::string_view s) {
  if (s.size() >= 2 && s[1] == ':') return true;
  return s.find('\\') != std::string_view::npos;
}

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Splits on both separators, dropping empty components and ".".
std::vector<std::string> splitComponents(std::string_view s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isSep(s[i])) ++i;
    const size_t b = i;
    while (i < s.size() && !isSep(s[i])) ++i;
    if (i > b) {
      std::string c(s.substr(b, i - b));
      if (c != ".") out.push_back(std::move(c));
    }
  }
  return out;
}

bool sameComponent(const std::string& a, const std::string& b, bool ignoreCase) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (ignoreCase ? lower(a[i]) != lower(b[i]) : a[i] != b[i]) return false;
  }
  return true;
}

bool isFile(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

// True when `child` lies inside `root` (both canonicalised as far as they exist).
bool isInside(const fs::path& child, const fs::path& root) {
  std::error_code ec;
  const fs::path c = fs::weakly_canonical(child, ec);
  if (ec) return false;
  const fs::path r = fs::weakly_canonical(root, ec);
  if (ec) return false;
  auto ci = c.begin();
  for (auto ri = r.begin(); ri != r.end(); ++ri, ++ci) {
    if (ri->empty()) continue;  // trailing separator
    if (ci == c.end() || *ci != *ri) return false;
  }
  return true;
}

}  // namespace

SourceLocator::SourceLocator(SourceLocatorConfig config) : m_config(std::move(config)) {}

void SourceLocator::configure(SourceLocatorConfig config) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config = std::move(config);
  m_cache.clear();
  m_lru.clear();
}

SourceLocatorConfig SourceLocator::config() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_config;
}

void SourceLocator::clearCache() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_cache.clear();
  m_lru.clear();
}

size_t SourceLocator::cacheSize() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_cache.size();
}

Result<fs::path> SourceLocator::locate(std::string_view debugPath) const {
  SourceLocatorConfig cfg;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_cache.find(std::string(debugPath));
    if (it != m_cache.end()) {
      m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
      if (it->second.path.empty())
        return makeError(ErrorCode::NotFound, "source file not found: " + std::string(debugPath));
      return it->second.path;
    }
    cfg = m_config;
  }
  Result<fs::path> r = resolve(cfg, debugPath);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key(debugPath);
    const auto it = m_cache.find(key);
    if (it != m_cache.end()) {
      it->second.path = r ? *r : fs::path();
      m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
    } else {
      m_lru.push_front(key);
      m_cache.emplace(key, CacheEntry{r ? *r : fs::path(), m_lru.begin()});
      while (m_cache.size() > kMaxCacheEntries) {
        m_cache.erase(m_lru.back());
        m_lru.pop_back();
      }
    }
  }
  return r;
}

Result<fs::path> SourceLocator::resolve(const SourceLocatorConfig& cfg, std::string_view debugPath) const {
  if (debugPath.empty()) return makeError(ErrorCode::InvalidArgument, "empty source path");
  // 1. as given
  {
    const fs::path direct{std::string(debugPath)};
    if (isFile(direct)) return direct;
  }
  const std::vector<std::string> comps = splitComponents(debugPath);
  // 2. remap rules
  for (const SourceRemapRule& rule : cfg.remaps) {
    const std::vector<std::string> from = splitComponents(rule.from);
    if (from.empty() || from.size() > comps.size() || rule.to.empty()) continue;
    const bool ignoreCase = looksWindows(rule.from) || looksWindows(debugPath);
    bool match = true;
    for (size_t i = 0; i < from.size() && match; ++i) match = sameComponent(from[i], comps[i], ignoreCase);
    if (!match) continue;
    fs::path candidate(rule.to);
    for (size_t i = from.size(); i < comps.size(); ++i) candidate /= comps[i];
    if (!isFile(candidate)) continue;
    if (!isInside(candidate, fs::path(rule.to))) continue;  // ".." or a symlink leading out of the target
    return candidate;
  }
  // 3. search roots by trailing components (".." components never take part)
  std::vector<std::string> clean;
  for (const std::string& c : comps) {
    if (c == "..") {
      clean.clear();  // a path that climbs cannot be matched by its tail; start over after it
      continue;
    }
    // Drive designators such as "C:" are not directories.
    if (clean.empty() && c.size() == 2 && c[1] == ':') continue;
    clean.push_back(c);
  }
  for (size_t n = clean.size(); n >= 1; --n) {
    for (const fs::path& root : cfg.searchRoots) {
      fs::path candidate = root;
      for (size_t i = clean.size() - n; i < clean.size(); ++i) candidate /= clean[i];
      if (isFile(candidate) && isInside(candidate, root)) return candidate;
    }
  }
  return makeError(ErrorCode::NotFound, "source file not found: " + std::string(debugPath));
}

}  // namespace tachyon::symbols
