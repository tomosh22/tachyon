#include "viewer/capture/session_catalog.h"

#include "core/platform.h"
#include "trace/trace_reader.h"
#include "viewer/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <set>

namespace tachyon::viewer {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string lowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool isTraceFile(const fs::path& p) {
  return lowerAscii(pathToUtf8(p.extension())) == ".tachyon";
}

uint64_t jsonUint(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number() || it->get<double>() < 0.0) return 0;
  return it->get<uint64_t>();
}

std::string jsonString(const json& j, const char* key) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

fs::path normalizedDir(const fs::path& dir) {
  fs::path p = dir.lexically_normal();
  if (!p.has_filename() && p.has_parent_path()) p = p.parent_path();
  return p;
}

int64_t epochSeconds(const fs::path& p, std::error_code& ec) {
  const fs::file_time_type ft = fs::last_write_time(p, ec);
  if (ec) return 0;
  const auto sys = std::chrono::system_clock::now() + (ft - fs::file_time_type::clock::now());
  return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

}  // namespace

SessionInfo readSessionInfo(const fs::path& path) {
  SessionInfo info;
  info.path = path;
  std::error_code ec;
  const fs::file_status st = fs::status(path, ec);
  if (ec || !fs::exists(st)) {
    info.state = SessionState::Missing;
    return info;
  }
  if (!fs::is_regular_file(st)) {
    info.state = SessionState::Unreadable;
    info.error = "not a regular file";
    return info;
  }
  info.sizeBytes = fs::file_size(path, ec);
  if (ec) info.sizeBytes = 0;
  ec.clear();
  info.mtime = epochSeconds(path, ec);

  auto reader = trace::TraceReader::open(path);
  if (!reader.ok()) {
    info.state = SessionState::Unreadable;
    info.error = reader.error().message;
    return info;
  }
  info.state = SessionState::Ok;
  const trace::FileHeader& header = reader->header();
  info.partial = reader->isPartial();
  if (header.clock_frequency_hz != 0 && header.end_ts > header.start_ts)
    info.durationS =
        static_cast<double>(header.end_ts - header.start_ts) / static_cast<double>(header.clock_frequency_hz);

  auto text = reader->readCaptureInfo();
  if (!text.ok()) return info;
  const json doc = json::parse(*text, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) return info;
  info.hasCaptureInfo = true;
  info.samplingHz = static_cast<uint32_t>(jsonUint(doc, "sampling_hz"));
  info.eventsLost = jsonUint(doc, "events_lost");
  info.agentVersion = jsonString(doc, "agent_version");
  info.mode = jsonString(doc, "mode");
  if (const auto opts = doc.find("capture_options"); opts != doc.end() && opts->is_object()) {
    if (const std::string m = jsonString(*opts, "mode"); !m.empty()) info.mode = m;
    info.exe = jsonString(*opts, "exe");
    info.pid = static_cast<uint32_t>(jsonUint(*opts, "pid"));
  }
  if (info.pid == 0) {
    if (const auto pids = doc.find("target_pids"); pids != doc.end() && pids->is_array() &&
                                                   pids->size() == 1 && (*pids)[0].is_number() &&
                                                   (*pids)[0].get<double>() > 0.0)
      info.pid = (*pids)[0].get<uint32_t>();
  }
  return info;
}

std::vector<SessionInfo> sortSessions(std::vector<SessionInfo> rows, SessionSort key, bool ascending) {
  auto readable = [](const SessionInfo& r) {
    return r.state == SessionState::Ok || r.state == SessionState::Pending;
  };
  std::stable_sort(rows.begin(), rows.end(), [&](const SessionInfo& a, const SessionInfo& b) {
    switch (key) {
      case SessionSort::Name: {
        const std::string an = lowerAscii(pathToUtf8(a.path.filename()));
        const std::string bn = lowerAscii(pathToUtf8(b.path.filename()));
        return ascending ? an < bn : an > bn;
      }
      case SessionSort::Time:
      case SessionSort::Size: {
        if (readable(a) != readable(b)) return readable(a);  // missing / unreadable rows last
        const int64_t av = key == SessionSort::Time ? a.mtime : static_cast<int64_t>(a.sizeBytes);
        const int64_t bv = key == SessionSort::Time ? b.mtime : static_cast<int64_t>(b.sizeBytes);
        if (av == bv) return false;
        return ascending ? av < bv : av > bv;
      }
    }
    return false;
  });
  return rows;
}

std::vector<SessionInfo> filterSessions(const std::vector<SessionInfo>& rows, const std::string& text) {
  if (text.empty()) return rows;
  const std::string needle = lowerAscii(text);
  std::vector<SessionInfo> out;
  for (const SessionInfo& r : rows) {
    const std::string hay =
        lowerAscii(pathToUtf8(r.path) + "\n" + r.mode + "\n" + r.exe + "\n" + r.agentVersion);
    if (hay.find(needle) != std::string::npos) out.push_back(r);
  }
  return out;
}

std::vector<fs::path> sessionFilesFor(const fs::path& trace) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (fs::exists(fs::symlink_status(trace, ec))) files.push_back(trace);
  for (const char* suffix : {".etl", ".perfspool", ".inst"}) {
    fs::path sibling = trace;
    sibling += suffix;
    ec.clear();
    if (fs::exists(fs::symlink_status(sibling, ec))) files.push_back(std::move(sibling));
  }
  return files;
}

Result<void> deleteSession(const fs::path& trace, std::vector<fs::path>* removed) {
  if (!isTraceFile(trace))
    return makeError(ErrorCode::InvalidArgument,
                     "refusing to delete a file that is not a .tachyon trace: " + pathToUtf8(trace));
  std::error_code dirEc;
  if (fs::is_directory(fs::symlink_status(trace, dirEc)))
    return makeError(ErrorCode::InvalidArgument,
                     "refusing to delete a directory named like a trace: " + pathToUtf8(trace));
  for (const fs::path& file : sessionFilesFor(trace)) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(file, ec);
    if (fs::is_directory(st)) {
      fs::remove_all(file, ec);  // never follows symlinks
    } else {
      fs::remove(file, ec);
    }
    if (ec) return makeError(ErrorCode::IoError, "cannot delete " + pathToUtf8(file) + ": " + ec.message());
    if (removed) removed->push_back(file);
  }
  return ok();
}

LaunchSpec revealSpec(const fs::path& file) {
  LaunchSpec spec;
#if TACHYON_PLATFORM_WINDOWS
  spec.exe = "explorer.exe";
  spec.args = {"/select," + pathToUtf8(file)};
#else
  spec.exe = "xdg-open";
  spec.args = {pathToUtf8(file.parent_path().empty() ? fs::path(".") : file.parent_path())};
#endif
  spec.description = "the file manager";
  return spec;
}

// ---- SessionCatalog ----

SessionCatalog::SessionCatalog() = default;

SessionCatalog::~SessionCatalog() {
  {
    std::lock_guard lock(m_mutex);
    m_quit = true;
    m_pending.reset();
  }
  m_cv.notify_all();
  if (m_worker.joinable()) m_worker.join();
}

void SessionCatalog::setOnUpdate(std::function<void()> onUpdate) {
  std::lock_guard lock(m_mutex);
  m_onUpdate = std::move(onUpdate);
}

void SessionCatalog::refresh(std::vector<fs::path> recent, fs::path capturesDir) {
  {
    std::lock_guard lock(m_mutex);
    if (m_quit) return;
    m_pending = Request{std::move(recent), std::move(capturesDir)};
    if (!m_worker.joinable()) m_worker = std::thread([this] { workerMain(); });
  }
  m_cv.notify_all();
}

bool SessionCatalog::poll(std::vector<SessionInfo>& rows) {
  std::lock_guard lock(m_mutex);
  if (!m_changed) return false;
  m_changed = false;
  rows = m_rows;
  return true;
}

bool SessionCatalog::busy() const {
  std::lock_guard lock(m_mutex);
  return m_pending.has_value() || m_running;
}

void SessionCatalog::waitIdle() {
  std::unique_lock lock(m_mutex);
  m_idleCv.wait(lock, [this] { return !m_pending.has_value() && !m_running; });
}

void SessionCatalog::publish(std::vector<SessionInfo> rows) {
  std::function<void()> notify;
  {
    std::lock_guard lock(m_mutex);
    m_rows = std::move(rows);
    m_changed = true;
    notify = m_onUpdate;
  }
  if (notify) notify();
}

void SessionCatalog::workerMain() {
  for (;;) {
    Request request;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [this] { return m_quit || m_pending.has_value(); });
      if (m_quit) return;
      request = std::move(*m_pending);
      m_pending.reset();
      m_running = true;
    }
    process(request);
    {
      std::lock_guard lock(m_mutex);
      m_running = false;
    }
    m_idleCv.notify_all();
  }
}

void SessionCatalog::process(const Request& request) {
  std::error_code ec;
  std::set<std::string> seen;
  std::vector<SessionInfo> rows;
  std::set<std::string> recentKeys;
  for (const fs::path& p : request.recent) recentKeys.insert(pathToUtf8(p.lexically_normal()));
  const fs::path dirKey = request.dir.empty() ? fs::path() : normalizedDir(request.dir);

  auto add = [&](const fs::path& path, bool inDir) {
    const std::string key = pathToUtf8(path.lexically_normal());
    if (!seen.insert(key).second) return;
    SessionInfo row;
    std::error_code e;
    const fs::file_status st = fs::status(path, e);
    if (e || !fs::exists(st)) {
      row.path = path;
      row.state = SessionState::Missing;
    } else {
      const uint64_t size = fs::is_regular_file(st) ? fs::file_size(path, e) : 0;
      e.clear();
      const int64_t mtime = epochSeconds(path, e);
      auto cached = m_cache.find(key);
      if (cached != m_cache.end() && cached->second.mtime == mtime && cached->second.size == size) {
        row = cached->second.info;
      } else {
        row.path = path;
        row.state = SessionState::Pending;
        row.sizeBytes = size;
        row.mtime = mtime;
      }
    }
    row.recent = recentKeys.count(key) != 0;
    row.inCapturesDir = inDir;
    rows.push_back(std::move(row));
  };

  for (const fs::path& p : request.recent) add(p, false);
  if (!request.dir.empty()) {
    fs::directory_iterator it(request.dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
      std::error_code e;
      if (it->is_regular_file(e) && isTraceFile(it->path())) add(it->path(), true);
    }
  }
  // Rows from the directory scan also flag the recent ones that live there.
  for (SessionInfo& r : rows) {
    if (!r.inCapturesDir && !dirKey.empty()) r.inCapturesDir = normalizedDir(r.path.parent_path()) == dirKey;
  }
  publish(rows);

  size_t sinceUpdate = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].state != SessionState::Pending) continue;
    {
      std::lock_guard lock(m_mutex);
      if (m_pending.has_value() || m_quit)
        return;  // a newer request supersedes; the cache keeps what was read
    }
    SessionInfo read = readSessionInfo(rows[i].path);
    ++m_reads;
    read.recent = rows[i].recent;
    read.inCapturesDir = rows[i].inCapturesDir;
    if (read.state != SessionState::Missing) {
      CacheEntry entry;
      entry.mtime = read.mtime;
      entry.size = read.sizeBytes;
      entry.info = read;
      m_cache[pathToUtf8(rows[i].path.lexically_normal())] = std::move(entry);
    }
    rows[i] = std::move(read);
    if (++sinceUpdate >= 16) {
      sinceUpdate = 0;
      publish(rows);
    }
  }
  if (sinceUpdate > 0) publish(rows);
}

}  // namespace tachyon::viewer
