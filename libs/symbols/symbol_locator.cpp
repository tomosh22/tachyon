#include "symbols/symbol_locator.h"

#include "core/log.h"
#include "symbols/symbol_keys.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

namespace tachyon::symbols {

namespace fs = std::filesystem;

namespace {

bool isHttpUrl(const std::string& s) {
  return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

std::string joinUrl(std::string server, const std::string& relative) {
  while (!server.empty() && server.back() == '/') server.pop_back();
  return server + "/" + relative;
}

bool fileExists(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

fs::path normalizedAbsolute(const fs::path& p) {
  std::error_code ec;
  fs::path abs = fs::absolute(p, ec);
  return (ec ? p : abs).lexically_normal();
}

constexpr size_t kMaxPtrBytes = 4096;

// "PATH:<path>" / "MSG:<text>" content of a file.ptr.
struct PtrContent {
  std::optional<fs::path> path;
  std::string message;
};

PtrContent parsePtr(const fs::path& file) {
  PtrContent out;
  // file.ptr bodies are tiny; never read more than kMaxPtrBytes.
  std::ifstream in(file, std::ios::binary);
  std::string text(kMaxPtrBytes, '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(in.gcount()));
  while (!text.empty() &&
         (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\0')) {
    text.pop_back();
  }
  if (text.rfind("PATH:", 0) == 0) {
    out.path = fs::path(text.substr(5));
  } else if (text.rfind("MSG:", 0) == 0) {
    out.message = text.substr(4);
  } else {
    out.message = "unrecognised file.ptr content";
  }
  return out;
}

// Names to look for in a flat directory for `module`.
std::vector<std::string> candidateNames(const Module& module) {
  const std::string image = pathFileName(module.path);
  switch (module.id.kind) {
    case 1: {
      std::string n = pdbFileName(module);
      return n.empty() ? std::vector<std::string>{} : std::vector<std::string>{n};
    }
    case 2:
      return image.empty() ? std::vector<std::string>{} : std::vector<std::string>{image + ".debug", image};
    default: return image.empty() ? std::vector<std::string>{} : std::vector<std::string>{image};
  }
}

// "ab/cdef.debug" relative path of a build-id under a `.build-id` tree.
std::string buildIdRelative(const std::string& hex) {
  if (hex.size() < 3) return {};
  return ".build-id/" + hex.substr(0, 2) + "/" + hex.substr(2) + ".debug";
}

class Gate {
 public:
  explicit Gate(unsigned limit) : m_free(limit == 0 ? 1 : limit) {}
  bool acquire(const std::atomic<bool>* cancel) {
    std::unique_lock lock(m_mutex);
    while (m_free == 0) {
      if (cancel && cancel->load(std::memory_order_relaxed)) return false;
      m_cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    --m_free;
    return true;
  }
  void release() {
    {
      std::lock_guard lock(m_mutex);
      ++m_free;
    }
    m_cv.notify_one();
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  unsigned m_free;
};

}  // namespace

struct SymbolLocator::Impl {
  explicit Impl(SymbolLocatorConfig c) : config(std::move(c)), gate(config.maxConcurrentDownloads) {
#ifndef _WIN32
    if (config.useSystemDebugDirs) config.debugDirs.push_back("/usr/lib/debug");
#endif
  }

  SymbolLocatorConfig config;
  Gate gate;

  std::once_flag httpOnce;
  std::shared_ptr<IHttpClient> httpClient;

  std::mutex flightMutex;
  std::condition_variable flightCv;
  std::set<std::string> inFlight;

  std::mutex pinMutex;
  std::map<std::string, int> pins;

  std::mutex tierMutex;
  std::map<std::string, bool> serverTier;

  std::mutex evictMutex;
  std::atomic<uint64_t> scratchCounter{0};

  IHttpClient& http() {
    std::call_once(httpOnce, [this] {
      httpClient = config.http ? config.http : makeDefaultHttpClient(config.httpOptions);
    });
    return *httpClient;
  }

  fs::path cacheRoot() const {
    if (!config.cacheDir.empty()) return config.cacheDir;
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) tmp = ".";
    return tmp / "tachyon-symcache";
  }

  // --- layout
  // -----------------------------------------------------------------------------------------------

  static bool isTwoTier(const fs::path& root) { return fileExists(root / "index2.txt"); }

  // Directory holding the file for (name, key) in a symbol store rooted at `root`.
  static fs::path storeDir(const fs::path& root, const std::string& name, const std::string& key,
                           bool twoTier) {
    if (twoTier) return root / name.substr(0, 2) / name / key;
    return root / name / key;
  }

  // Looks for a SymStore entry in a store (cache or local server): the file, else its file.ptr.
  // Returns the file path, or nullopt; `reason` collects file.ptr messages.
  std::optional<fs::path> findInStore(const fs::path& root, const SymbolKey& key,
                                      std::vector<std::string>& reasons) const {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    const bool twoTier = isTwoTier(root);
    for (const bool tier : {twoTier, !twoTier}) {
      const fs::path dir = storeDir(root, key.name, key.key, tier);
      const fs::path file = dir / key.name;
      if (fileExists(file)) return file;
      if (fileExists(dir / "file.ptr")) {
        PtrContent ptr = parsePtr(dir / "file.ptr");
        if (ptr.path && fileExists(*ptr.path)) return *ptr.path;
        reasons.push_back(ptr.path
                              ? "file.ptr in " + dir.string() + " points to missing " + ptr.path->string()
                              : "file.ptr in " + dir.string() + ": " + ptr.message);
      }
    }
    return std::nullopt;
  }

  // Where a downloaded file for `key` is stored in the cache.
  fs::path cacheFile(const SymbolKey& key) const {
    const fs::path root = cacheRoot();
    if (key.scheme == SymbolKey::Scheme::DebugInfod) return root / "buildid" / key.key / "debuginfo";
    return storeDir(root, key.name, key.key, isTwoTier(root)) / key.name;
  }

  // --- pinning / eviction
  // -----------------------------------------------------------------------------------

  void pinFile(const fs::path& file) {
    std::lock_guard lock(pinMutex);
    ++pins[normalizedAbsolute(file).string()];
  }

  void unpinFile(const fs::path& file) {
    std::lock_guard lock(pinMutex);
    auto it = pins.find(normalizedAbsolute(file).string());
    if (it != pins.end() && --it->second <= 0) pins.erase(it);
  }

  bool isPinned(const fs::path& file) {
    std::lock_guard lock(pinMutex);
    return pins.count(normalizedAbsolute(file).string()) != 0;
  }

  uint64_t evict() {
    if (config.cacheMaxBytes == 0 || config.cacheDir.empty()) return 0;
    std::lock_guard lock(evictMutex);
    const fs::path root = normalizedAbsolute(config.cacheDir);
    struct Entry {
      fs::path path;
      uint64_t size;
      fs::file_time_type mtime;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_regular_file(e2) || e2) continue;
      const fs::path& p = it->path();
      const std::string name = p.filename().string();
      if (name == "index2.txt" || p.extension() == ".part" || p.extension() == ".tmp") continue;
      const uint64_t size = static_cast<uint64_t>(it->file_size(e2));
      if (e2) continue;
      const fs::file_time_type mtime = it->last_write_time(e2);
      if (e2) continue;
      entries.push_back({p, size, mtime});
      total += size;
    }
    if (total <= config.cacheMaxBytes) return 0;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
    uint64_t freed = 0;
    for (const Entry& e : entries) {
      if (total <= config.cacheMaxBytes) break;
      if (isPinned(e.path)) continue;
      std::error_code rm;
      if (!fs::remove(e.path, rm) || rm) continue;
      total -= e.size;
      freed += e.size;
      // Remove directories left empty, up to (not including) the cache root.
      for (fs::path dir = e.path.parent_path(); dir != root && dir.has_parent_path();
           dir = dir.parent_path()) {
        std::error_code de;
        if (!fs::is_empty(dir, de) || de) break;
        fs::remove(dir, de);
      }
    }
    return freed;
  }

  // --- local probing
  // ----------------------------------------------------------------------------------------

  // Probes one directory the way explicit search paths and next-to-module do.
  std::optional<fs::path> probeDirectory(const fs::path& dir, const Module& module,
                                         const std::optional<SymbolKey>& key,
                                         std::vector<std::string>& reasons) const {
    for (const std::string& name : candidateNames(module)) {
      if (fileExists(dir / name)) return dir / name;
    }
    if (key && key->scheme == SymbolKey::Scheme::SymStore) {
      if (auto hit = findInStore(dir, *key, reasons)) return hit;
    }
    if (key && key->scheme == SymbolKey::Scheme::DebugInfod) {
      const std::string rel = buildIdRelative(key->key);
      if (!rel.empty() && fileExists(dir / rel)) return dir / rel;
    }
    return std::nullopt;
  }

  std::optional<fs::path> searchExplicit(const Module& module, const std::optional<SymbolKey>& key,
                                         std::vector<std::string>& reasons) const {
    for (const fs::path& p : config.searchPaths) {
      std::error_code ec;
      if (fs::is_regular_file(p, ec)) {
        const std::string fileName = lower(p.filename().string());
        for (const std::string& name : candidateNames(module)) {
          if (lower(name) == fileName) return p;
        }
        continue;
      }
      if (auto hit = probeDirectory(p, module, key, reasons)) return hit;
    }
    return std::nullopt;
  }

  std::optional<fs::path> searchNextToModule(const Module& module, const std::optional<SymbolKey>& key,
                                             std::vector<std::string>& reasons) const {
    if (module.path.empty()) return std::nullopt;
    const fs::path image(module.path);
    if (image.has_parent_path()) {
      const fs::path dir = image.parent_path();
      for (const std::string& name : candidateNames(module)) {
        if (fileExists(dir / name)) return dir / name;
      }
      if (module.id.kind == 2) {
        const std::string base = pathFileName(module.path);
        if (fileExists(dir / ".debug" / (base + ".debug"))) return dir / ".debug" / (base + ".debug");
      }
    }
    if (module.id.kind == 2) {
      // The debug info may be in the binary itself.
      if (fileExists(image)) return image;
      if (key) {
        const std::string rel = buildIdRelative(key->key);
        for (const fs::path& dbg : config.debugDirs) {
          if (!rel.empty() && fileExists(dbg / rel)) return dbg / rel;
        }
      }
    }
    (void)reasons;
    return std::nullopt;
  }

  // --- servers
  // ----------------------------------------------------------------------------------------------

  // Blocks while another thread downloads `dest`.
  class FlightGuard {
   public:
    FlightGuard(Impl& impl, std::string dest, const std::atomic<bool>* cancel)
        : m_impl(impl), m_dest(std::move(dest)) {
      std::unique_lock lock(m_impl.flightMutex);
      while (m_impl.inFlight.count(m_dest) != 0) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;
        m_impl.flightCv.wait_for(lock, std::chrono::milliseconds(20));
      }
      m_impl.inFlight.insert(m_dest);
      m_held = true;
    }
    ~FlightGuard() {
      if (!m_held) return;
      {
        std::lock_guard lock(m_impl.flightMutex);
        m_impl.inFlight.erase(m_dest);
      }
      m_impl.flightCv.notify_all();
    }
    FlightGuard(const FlightGuard&) = delete;
    FlightGuard& operator=(const FlightGuard&) = delete;
    bool held() const { return m_held; }

   private:
    Impl& m_impl;
    std::string m_dest;
    bool m_held = false;
  };

  // One bounded HTTP GET (concurrency gate, response size cap; maxBytes 0 = config.maxDownloadBytes).
  Result<HttpStatus> boundedGet(const std::string& url, const fs::path& dest, const std::atomic<bool>* cancel,
                                const HttpProgressFn& progress, uint64_t maxBytes = 0) {
    if (!gate.acquire(cancel)) return makeError(ErrorCode::Cancelled, "cancelled");
    struct Release {
      Gate& g;
      ~Release() { g.release(); }
    } release{gate};
    return http().get(url, dest, cancel, progress, maxBytes != 0 ? maxBytes : config.maxDownloadBytes);
  }

  fs::path scratchFile(const fs::path& dir, const char* tag) {
    return dir / (std::string(tag) + "." + std::to_string(scratchCounter.fetch_add(1)) + ".tmp");
  }

  // Downloads `url` into the cache at `dest` (deduplicated across threads).
  Result<fs::path> download(const std::string& url, const fs::path& dest, const std::atomic<bool>* cancel) {
    FlightGuard guard(*this, normalizedAbsolute(dest).string(), cancel);
    if (!guard.held()) return makeError(ErrorCode::Cancelled, "cancelled");
    if (fileExists(dest)) return dest;  // another thread got it
    TACHYON_LOG_INFO("symbols: downloading {}", redactUrl(url));
    Result<HttpStatus> r = boundedGet(url, dest, cancel, nullptr);
    if (!r.ok()) return r.error();
    return dest;
  }

  // Removes `dir` and its parent when empty (leftovers of a miss), never above the cache root.
  void pruneEmpty(fs::path dir) const {
    const fs::path root = normalizedAbsolute(cacheRoot());
    for (int i = 0; i < 2; ++i) {
      if (normalizedAbsolute(dir) == root || !dir.has_parent_path()) return;
      std::error_code ec;
      if (!fs::is_empty(dir, ec) || ec) return;
      fs::remove(dir, ec);
      dir = dir.parent_path();
    }
  }

  // True when `target` lies under an explicit search path or a local-directory server.
  bool isUnderTrustedRoot(const fs::path& target) const {
    std::vector<fs::path> roots = config.searchPaths;
    for (const std::string& server : config.servers) {
      if (!isHttpUrl(server)) roots.emplace_back(server);
    }
    const fs::path t = normalizedAbsolute(target);
    for (const fs::path& root : roots) {
      const fs::path r = normalizedAbsolute(root);
      auto it = std::mismatch(r.begin(), r.end(), t.begin(), t.end());
      // Every component of the root matched (a trailing empty component from "dir/" is allowed).
      if (it.first == r.end() || (std::next(it.first) == r.end() && it.first->empty())) {
        if (it.second != t.end()) return true;
      }
    }
    return false;
  }

  // Whether `server` publishes a two-tier store (<server>/index2.txt exists). Definitive answers are cached.
  bool serverIsTwoTier(const std::string& server, const std::atomic<bool>* cancel) {
    {
      std::lock_guard lock(tierMutex);
      auto it = serverTier.find(server);
      if (it != serverTier.end()) return it->second;
    }
    const fs::path root = cacheRoot();
    const fs::path scratch = scratchFile(root, "index2");
    Result<HttpStatus> r = boundedGet(joinUrl(server, "index2.txt"), scratch, cancel, nullptr, 1u << 20);
    std::error_code ec;
    fs::remove(scratch, ec);
    pruneEmpty(root);
    if (!r.ok() && r.error().code != ErrorCode::NotFound) return false;  // transient: ask again next time
    std::lock_guard lock(tierMutex);
    serverTier[server] = r.ok();
    return r.ok();
  }

  static std::string urlName(const SymbolKey& key, bool twoTier) {
    const std::string name = urlEncodeComponent(key.name);
    return (twoTier ? urlEncodeComponent(key.name.substr(0, 2)) + "/" : std::string()) + name + "/" + key.key;
  }

  // Probes one layout of one HTTP server: the file, then file.ptr, then the compressed variant.
  Result<std::optional<fs::path>> probeServerLayout(const std::string& server, const SymbolKey& key,
                                                    bool twoTier, std::vector<std::string>& reasons,
                                                    const std::atomic<bool>* cancel, bool& missOnly) {
    const std::string shown = redactUrl(server);
    const fs::path dest = cacheFile(key);
    missOnly = true;
    const std::string dirUrl = joinUrl(server, urlName(key, twoTier));
    const std::string fileUrl = key.scheme == SymbolKey::Scheme::DebugInfod
                                    ? joinUrl(server, key.relativePath())
                                    : dirUrl + "/" + urlEncodeComponent(key.name);
    Result<fs::path> got = download(fileUrl, dest, cancel);
    if (got.ok()) return std::optional<fs::path>(got.value());
    if (got.error().code == ErrorCode::Cancelled) return got.error();
    if (got.error().code != ErrorCode::NotFound) {
      missOnly = false;
      reasons.push_back(shown + ": " + got.error().toString());
      return std::optional<fs::path>();
    }
    if (key.scheme == SymbolKey::Scheme::DebugInfod) {
      reasons.push_back(shown + ": not found");
      pruneEmpty(dest.parent_path());
      return std::optional<fs::path>();
    }

    // file.ptr redirection.
    const fs::path scratch = scratchFile(dest.parent_path(), "file.ptr");
    Result<HttpStatus> ptr = boundedGet(dirUrl + "/file.ptr", scratch, cancel, nullptr, kMaxPtrBytes);
    if (ptr.ok()) {
      PtrContent content = parsePtr(scratch);
      std::error_code ec;
      fs::remove(scratch, ec);
      pruneEmpty(dest.parent_path());
      if (content.path) {
        // The target comes from a remote server: only follow it into locations the user configured.
        bool allowed = isUnderTrustedRoot(*content.path);
#ifdef _WIN32
        const std::string raw = content.path->string();
        if (raw.rfind("\\\\", 0) == 0 || raw.rfind("//", 0) == 0) allowed = false;  // never UNC
#endif
        if (!allowed) {
          reasons.push_back(shown + ": file.ptr PATH from remote server ignored");
          return std::optional<fs::path>();
        }
        if (fileExists(*content.path)) return std::optional<fs::path>(*content.path);
      }
      reasons.push_back(
          shown + ": file.ptr " +
          (content.path ? "points to missing " + content.path->string() : "message: " + content.message));
      return std::optional<fs::path>();
    }
    if (ptr.error().code == ErrorCode::Cancelled) return ptr.error();

    // Compressed (.pd_) variant: detect only.
    std::string compressed = key.name;
    compressed.back() = '_';
    const fs::path probe = scratchFile(dest.parent_path(), "probe");
    Result<HttpStatus> cab =
        boundedGet(dirUrl + "/" + urlEncodeComponent(compressed), probe, cancel, nullptr);
    std::error_code ec;
    fs::remove(probe, ec);
    if (cab.ok()) {
      missOnly = false;
      reasons.push_back(shown + ": compressed CAB not supported (" + compressed + ")");
    } else if (cab.error().code == ErrorCode::Cancelled) {
      return cab.error();
    } else {
      reasons.push_back(shown + ": not found");
    }
    pruneEmpty(dest.parent_path());
    return std::optional<fs::path>();
  }

  // Tries one HTTP server for `key`: flat layout first, then the two-tier layout if the server has
  // index2.txt. On a miss appends the reasons; returns the file on a hit.
  Result<std::optional<fs::path>> tryHttpServer(const std::string& server, const SymbolKey& key,
                                                std::vector<std::string>& reasons,
                                                const std::atomic<bool>* cancel) {
    bool missOnly = true;
    Result<std::optional<fs::path>> r = probeServerLayout(server, key, false, reasons, cancel, missOnly);
    if (!r.ok() || r.value() || key.scheme != SymbolKey::Scheme::SymStore || !missOnly) return r;
    if (!serverIsTwoTier(server, cancel)) return r;
    return probeServerLayout(server, key, true, reasons, cancel, missOnly);
  }

  Result<fs::path> locate(const Module& module, const std::atomic<bool>* cancel) {
    std::vector<std::string> reasons;
    const std::optional<SymbolKey> key = symbolKeyFor(module);
    auto finish = [&](const fs::path& p) -> Result<fs::path> {
      pinFile(p);
      return p;
    };

    if (auto hit = searchExplicit(module, key, reasons)) return finish(*hit);
    if (auto hit = searchNextToModule(module, key, reasons)) return finish(*hit);

    if (!key) {
      return makeError(ErrorCode::NotFound,
                       "no symbol file next to " + module.path + " and the module has no identity");
    }

    // Local cache.
    {
      const fs::path root = cacheRoot();
      std::optional<fs::path> hit;
      if (key->scheme == SymbolKey::Scheme::DebugInfod) {
        const fs::path f = root / "buildid" / key->key / "debuginfo";
        if (fileExists(f)) hit = f;
      } else {
        hit = findInStore(root, *key, reasons);
      }
      if (hit) {
        std::error_code ec;
        fs::last_write_time(*hit, fs::file_time_type::clock::now(), ec);  // LRU touch
        return finish(*hit);
      }
    }

    for (const std::string& server : config.servers) {
      if (cancel && cancel->load(std::memory_order_relaxed))
        return makeError(ErrorCode::Cancelled, "cancelled");
      if (isHttpUrl(server)) {
        Result<std::optional<fs::path>> r = tryHttpServer(server, *key, reasons, cancel);
        if (!r.ok()) return r.error();
        if (r.value()) {
          Result<fs::path> out = finish(*r.value());
          evict();
          return out;
        }
      } else if (key->scheme == SymbolKey::Scheme::SymStore) {
        if (auto hit = findInStore(fs::path(server), *key, reasons)) return finish(*hit);
        reasons.push_back(server + ": not found");
      } else {
        const fs::path f = fs::path(server) / buildIdRelative(key->key);
        if (fileExists(f)) return finish(f);
        reasons.push_back(server + ": not found");
      }
    }

    std::string message = "no symbols for " + module.path + " (" + key->relativePath() + ")";
    for (const std::string& r : reasons) message += "; " + r;
    return makeError(ErrorCode::NotFound, std::move(message));
  }
};

SymbolLocator::SymbolLocator() : m_impl(std::make_unique<Impl>(SymbolLocatorConfig{})) {}

SymbolLocator::SymbolLocator(std::vector<std::string> servers, std::filesystem::path cacheDir) {
  SymbolLocatorConfig config;
  config.servers = std::move(servers);
  config.cacheDir = std::move(cacheDir);
  m_impl = std::make_unique<Impl>(std::move(config));
}

SymbolLocator::SymbolLocator(SymbolLocatorConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

SymbolLocator::~SymbolLocator() = default;
SymbolLocator::SymbolLocator(SymbolLocator&&) noexcept = default;
SymbolLocator& SymbolLocator::operator=(SymbolLocator&&) noexcept = default;

Result<std::filesystem::path> SymbolLocator::locate(const Module& module, const std::atomic<bool>* cancel) {
  return m_impl->locate(module, cancel);
}

void SymbolLocator::pin(const std::filesystem::path& file) {
  m_impl->pinFile(file);
}

void SymbolLocator::unpin(const std::filesystem::path& file) {
  m_impl->unpinFile(file);
}

uint64_t SymbolLocator::evictCache() {
  return m_impl->evict();
}

LocateFn makeDefaultLocate(SymbolLocatorConfig config) {
  auto locator = std::make_shared<SymbolLocator>(std::move(config));
  return [locator](const Module& m) { return locator->locate(m); };
}

CancellableLocateFn makeDefaultCancellableLocate(SymbolLocatorConfig config) {
  auto locator = std::make_shared<SymbolLocator>(std::move(config));
  return [locator](const Module& m, const std::atomic<bool>* cancel) { return locator->locate(m, cancel); };
}

}  // namespace tachyon::symbols
