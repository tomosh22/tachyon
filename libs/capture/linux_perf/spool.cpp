#include "capture/linux_perf/spool.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace tachyon::capture::perf {

namespace fs = std::filesystem;

std::string SessionInfo::toJson() const {
  nlohmann::json j;
  j["version"] = version;
  j["sample_type"] = layout.sampleType;
  j["sample_id_all"] = layout.sampleIdAll;
  j["start_ts"] = startTs;
  j["end_ts"] = endTs;
  j["sampling_hz"] = samplingHz;
  j["mode"] = mode;
  j["root_pid"] = rootPid;
  j["follow_children"] = followChildren;
  j["child_filter"] = childFilter;
  j["exe"] = exe;
  j["args"] = args;
  j["cpu_count"] = cpuCount;
  j["ring_count"] = ringCount;
  j["plan"] = plan;
  j["kernel_stacks"] = kernelStacks;
  j["exclude_kernel"] = excludeKernel;
  j["kernel_build_id"] = kernelBuildId;
  j["paranoid"] = paranoid;
  j["max_sample_rate"] = maxSampleRate;
  j["cpu_time_max_percent"] = cpuTimeMaxPercent;
  j["circular_mb"] = circularMb;
  j["sched"] = {{"mode", sched.mode},
                {"switches", sched.switches},
                {"wakeups", sched.wakeups},
                {"switch_format", sched.switchFormat},
                {"wake_format", sched.wakeFormat},
                {"wake_event", sched.wakeEvent},
                {"reason", sched.reason}};
  j["kernel_release"] = kernelRelease;
  j["hostname"] = hostname;
  j["agent_version"] = agentVersion;
  j["drained_bytes"] = drainedBytes;
  j["ring_overruns"] = ringOverruns;
  j["corrupt_batches"] = corruptBatches;
  j["warnings"] = warnings;
  j["finished"] = finished;
  return j.dump(2);
}

Result<SessionInfo> SessionInfo::fromJson(const std::string& text) {
  using nlohmann::json;
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object())
    return makeError(ErrorCode::CorruptData, "session.json is not a JSON object");
  SessionInfo s;
  auto num = [&](const char* key, auto& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) out = it->get<std::decay_t<decltype(out)>>();
  };
  auto str = [&](const char* key, std::string& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) out = it->get<std::string>();
  };
  auto flag = [&](const char* key, bool& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) out = it->get<bool>();
  };
  num("version", s.version);
  if (s.version != 1) return makeError(ErrorCode::CorruptData, "unsupported session.json version");
  num("sample_type", s.layout.sampleType);
  flag("sample_id_all", s.layout.sampleIdAll);
  num("start_ts", s.startTs);
  num("end_ts", s.endTs);
  num("sampling_hz", s.samplingHz);
  str("mode", s.mode);
  num("root_pid", s.rootPid);
  flag("follow_children", s.followChildren);
  str("child_filter", s.childFilter);
  str("exe", s.exe);
  if (auto it = j.find("args"); it != j.end() && it->is_array()) {
    for (const auto& a : *it)
      if (a.is_string()) s.args.push_back(a.get<std::string>());
  }
  num("cpu_count", s.cpuCount);
  num("ring_count", s.ringCount);
  str("plan", s.plan);
  flag("kernel_stacks", s.kernelStacks);
  flag("exclude_kernel", s.excludeKernel);
  flag("kernel_build_id", s.kernelBuildId);
  num("paranoid", s.paranoid);
  num("max_sample_rate", s.maxSampleRate);
  num("cpu_time_max_percent", s.cpuTimeMaxPercent);
  num("circular_mb", s.circularMb);
  if (auto it = j.find("sched"); it != j.end() && it->is_object()) {
    const json& o = *it;
    auto sstr = [&](const char* key, std::string& out) {
      auto f = o.find(key);
      if (f != o.end() && f->is_string()) out = f->get<std::string>();
    };
    auto sflag = [&](const char* key, bool& out) {
      auto f = o.find(key);
      if (f != o.end() && f->is_boolean()) out = f->get<bool>();
    };
    sstr("mode", s.sched.mode);
    sflag("switches", s.sched.switches);
    sflag("wakeups", s.sched.wakeups);
    sstr("switch_format", s.sched.switchFormat);
    sstr("wake_format", s.sched.wakeFormat);
    sstr("wake_event", s.sched.wakeEvent);
    sstr("reason", s.sched.reason);
  }
  str("kernel_release", s.kernelRelease);
  str("hostname", s.hostname);
  str("agent_version", s.agentVersion);
  num("drained_bytes", s.drainedBytes);
  num("ring_overruns", s.ringOverruns);
  num("corrupt_batches", s.corruptBatches);
  if (auto it = j.find("warnings"); it != j.end() && it->is_array()) {
    for (const auto& w : *it)
      if (w.is_string()) s.warnings.push_back(w.get<std::string>());
  }
  flag("finished", s.finished);
  return s;
}

fs::path spoolSessionPath(const fs::path& dir) {
  return dir / "session.json";
}
fs::path spoolMetaPath(const fs::path& dir) {
  return dir / "meta.bin";
}
fs::path spoolDataPath(const fs::path& dir, uint32_t segment) {
  char name[32];
  std::snprintf(name, sizeof(name), "data-%06u.bin", segment);
  return dir / name;
}

Result<void> writeSessionInfo(const fs::path& dir, const SessionInfo& info) {
  const fs::path target = spoolSessionPath(dir);
  const fs::path tmp = dir / "session.json.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::IoError, "cannot write " + tmp.string());
    out << info.toJson();
    out.flush();
    if (!out) return makeError(ErrorCode::IoError, "cannot write " + tmp.string());
  }
  std::error_code ec;
  fs::rename(tmp, target, ec);
  if (ec) return makeError(ErrorCode::IoError, "cannot rename " + tmp.string() + ": " + ec.message());
  return ok();
}

// ---------------------------------------------------------------------------------------------------------
// SpoolWriter

Result<SpoolWriter> SpoolWriter::create(const fs::path& path, SpoolKind kind, const RecordLayout& layout) {
  SpoolWriter w;
  w.m_path = path;
  w.m_file = std::fopen(path.c_str(), "wb");
  if (w.m_file == nullptr)
    return makeError(ErrorCode::IoError, "cannot create " + path.string() + ": " + std::strerror(errno));
  std::setvbuf(w.m_file, nullptr, _IOFBF, 1u << 20);
  SpoolFileHeader h{};
  std::memcpy(h.magic, kSpoolMagic, sizeof(h.magic));
  h.version = kSpoolVersion;
  h.kind = static_cast<uint32_t>(kind);
  h.sampleType = layout.sampleType;
  h.sampleIdAll = layout.sampleIdAll ? 1u : 0u;
  if (std::fwrite(&h, sizeof(h), 1, w.m_file) != 1)
    return makeError(ErrorCode::IoError, "cannot write " + path.string());
  w.m_bytes = sizeof(h);
  return w;
}

SpoolWriter::SpoolWriter(SpoolWriter&& o) noexcept
    : m_path(std::move(o.m_path)), m_file(o.m_file), m_bytes(o.m_bytes), m_failed(o.m_failed) {
  o.m_file = nullptr;
}

SpoolWriter& SpoolWriter::operator=(SpoolWriter&& o) noexcept {
  if (this != &o) {
    (void)close();
    m_path = std::move(o.m_path);
    m_file = o.m_file;
    m_bytes = o.m_bytes;
    m_failed = o.m_failed;
    o.m_file = nullptr;
  }
  return *this;
}

SpoolWriter::~SpoolWriter() {
  (void)close();
}

Result<void> SpoolWriter::append(uint32_t ring, std::span<const uint8_t> records) {
  if (m_file == nullptr || m_failed)
    return makeError(ErrorCode::IoError, "spool " + m_path.string() + " is not writable");
  if (records.empty()) return ok();
  SpoolBatchHeader h{kSpoolBatchMagic, ring, records.size()};
  if (std::fwrite(&h, sizeof(h), 1, m_file) != 1 ||
      std::fwrite(records.data(), 1, records.size(), m_file) != records.size()) {
    m_failed = true;
    return makeError(ErrorCode::IoError,
                     "writing " + m_path.string() + " failed: " + std::strerror(errno) + " (disk full?)");
  }
  m_bytes += sizeof(h) + records.size();
  return ok();
}

Result<void> SpoolWriter::flush() {
  if (m_file == nullptr) return ok();
  if (std::fflush(m_file) != 0) {
    m_failed = true;
    return makeError(ErrorCode::IoError, "flushing " + m_path.string() + " failed: " + std::strerror(errno));
  }
  return ok();
}

Result<void> SpoolWriter::close() {
  if (m_file == nullptr) return ok();
  const int rc = std::fclose(m_file);
  m_file = nullptr;
  if (rc != 0 || m_failed) return makeError(ErrorCode::IoError, "closing " + m_path.string() + " failed");
  return ok();
}

// ---------------------------------------------------------------------------------------------------------
// SpoolFile

SpoolFile::SpoolFile(SpoolFile&& o) noexcept
    : m_path(std::move(o.m_path)),
      m_fd(o.m_fd),
      m_kind(o.m_kind),
      m_layout(o.m_layout),
      m_batches(std::move(o.m_batches)),
      m_truncated(o.m_truncated) {
  o.m_fd = -1;
}

SpoolFile& SpoolFile::operator=(SpoolFile&& o) noexcept {
  if (this != &o) {
    if (m_fd >= 0) ::close(m_fd);
    m_path = std::move(o.m_path);
    m_fd = o.m_fd;
    m_kind = o.m_kind;
    m_layout = o.m_layout;
    m_batches = std::move(o.m_batches);
    m_truncated = o.m_truncated;
    o.m_fd = -1;
  }
  return *this;
}

SpoolFile::~SpoolFile() {
  if (m_fd >= 0) ::close(m_fd);
}

namespace {

bool preadAll(int fd, void* dst, size_t n, uint64_t offset) {
  auto* p = static_cast<uint8_t*>(dst);
  while (n > 0) {
    const ssize_t got = ::pread(fd, p, n, static_cast<off_t>(offset));
    if (got < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (got == 0) return false;
    p += got;
    n -= static_cast<size_t>(got);
    offset += static_cast<uint64_t>(got);
  }
  return true;
}

}  // namespace

Result<SpoolFile> SpoolFile::open(const fs::path& path) {
  SpoolFile f;
  f.m_path = path;
  f.m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (f.m_fd < 0) {
    return makeError(errno == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError,
                     "cannot open " + path.string() + ": " + std::strerror(errno));
  }
  struct stat st {};
  if (::fstat(f.m_fd, &st) != 0) return makeError(ErrorCode::IoError, "cannot stat " + path.string());
  const uint64_t size = static_cast<uint64_t>(st.st_size);
  SpoolFileHeader h{};
  if (size < sizeof(h) || !preadAll(f.m_fd, &h, sizeof(h), 0))
    return makeError(ErrorCode::CorruptData, path.string() + ": truncated spool header");
  if (std::memcmp(h.magic, kSpoolMagic, sizeof(h.magic)) != 0 || h.version != kSpoolVersion ||
      (h.kind != static_cast<uint32_t>(SpoolKind::Meta) && h.kind != static_cast<uint32_t>(SpoolKind::Data)))
    return makeError(ErrorCode::CorruptData, path.string() + ": not a perf spool file");
  f.m_kind = static_cast<SpoolKind>(h.kind);
  f.m_layout.sampleType = h.sampleType;
  f.m_layout.sampleIdAll = h.sampleIdAll != 0;

  uint64_t off = sizeof(h);
  while (off < size) {
    SpoolBatchHeader bh{};
    if (size - off < sizeof(bh) || !preadAll(f.m_fd, &bh, sizeof(bh), off) || bh.magic != kSpoolBatchMagic ||
        bh.byteLen > kMaxSpoolBatchBytes) {
      f.m_truncated = true;
      break;
    }
    off += sizeof(bh);
    uint64_t len = bh.byteLen;
    if (len > size - off) {  // the last batch was cut short: keep what is there
      len = size - off;
      f.m_truncated = true;
    }
    f.m_batches.push_back({bh.ring, off, len});
    off += len;
  }
  return f;
}

Result<void> SpoolFile::read(const SpoolBatchRef& batch, std::vector<uint8_t>& out) const {
  out.resize(static_cast<size_t>(batch.length));
  if (!preadAll(m_fd, out.data(), out.size(), batch.offset))
    return makeError(ErrorCode::IoError, "reading " + m_path.string() + " failed");
  return ok();
}

// ---------------------------------------------------------------------------------------------------------
// SpoolSet

Result<SpoolSet> SpoolSet::openDirectory(const fs::path& dir) {
  SpoolSet set;
  set.dir = dir;
  std::ifstream in(spoolSessionPath(dir), std::ios::binary);
  if (!in) return makeError(ErrorCode::NotFound, "no session.json in " + dir.string());
  std::stringstream ss;
  ss << in.rdbuf();
  TACHYON_TRY_ASSIGN(set.info, SessionInfo::fromJson(ss.str()));
  std::error_code ec;
  if (fs::exists(spoolMetaPath(dir), ec)) set.meta = spoolMetaPath(dir);
  std::vector<fs::path> data;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("data-", 0) == 0 && name.size() > 9 && name.substr(name.size() - 4) == ".bin")
      data.push_back(entry.path());
  }
  std::sort(data.begin(), data.end());
  set.data = std::move(data);
  return set;
}

// ---------------------------------------------------------------------------------------------------------
// RecordStream

bool RecordStream::loadNextBatch() {
  while (m_nextExtent < m_extents.size()) {
    const Extent& e = m_extents[m_nextExtent++];
    if (!e.file->read(e.batch, m_buf).ok()) {
      ++m_readErrors;
      continue;
    }
    m_pos = 0;
    if (!m_buf.empty()) return true;
  }
  return false;
}

bool RecordStream::advance() {
  for (;;) {
    if (m_pos >= m_buf.size()) {
      if (!loadNextBatch()) return false;
    }
    const std::span<const uint8_t> rest(m_buf.data() + m_pos, m_buf.size() - m_pos);
    RecordHeader h;
    const ParseStatus hs = readRecordHeader(rest, h);
    if (hs != ParseStatus::Ok) {  // truncated or malformed: nothing after this point in the batch is usable
      ++m_malformed;
      m_pos = m_buf.size();
      continue;
    }
    const ParseStatus ps = parseRecord(rest.first(h.size), m_layout, m_record);
    m_pos += h.size;
    if (ps == ParseStatus::Ok) return true;
    if (ps == ParseStatus::Unsupported) {
      ++m_skippedUnsupported;
      continue;
    }
    ++m_malformed;  // a malformed body: skip just this record, the header was sane
  }
}

}  // namespace tachyon::capture::perf
