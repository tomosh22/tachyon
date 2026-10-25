#include "inst/ingest.h"

#include "core/log.h"

#include <system_error>
#include <thread>

namespace tachyon::inst {

namespace fs = std::filesystem;

namespace {

// One connection: complete frames go to the spool, an incomplete tail at the end is only counted.
class SpoolSink final : public IStreamSink {
 public:
  explicit SpoolSink(Spool& spool) : m_spool(spool) { m_spool.noteConnection(); }

  void onData(const uint8_t* data, size_t size) override {
    if (!m_assembler.feed(
            data, size, [this](const uint8_t* frame, size_t bytes) { m_spool.appendFrame(frame, bytes); }) &&
        !m_reportedCorrupt) {
      m_reportedCorrupt = true;
      TACHYON_LOG_WARN(
          "inst: corrupt instrumentation stream (implausible frame length); the rest of the connection is "
          "ignored");
    }
  }

  void onClose() override {
    m_spool.noteTruncatedTail(m_assembler.pendingBytes());
    m_spool.noteCorrupt(m_assembler.discardedBytes());
  }

 private:
  Spool& m_spool;
  FrameAssembler m_assembler;
  bool m_reportedCorrupt = false;
};

}  // namespace

Ingest::Ingest(IngestConfig config) : m_config(std::move(config)) {}

Ingest::~Ingest() {
  (void)finish();
}

size_t Ingest::processCount() const {
  std::lock_guard lock(m_mutex);
  return m_processes.size();
}

Result<void> Ingest::attach(uint32_t pid, const std::string& address) {
  std::lock_guard lock(m_mutex);
  if (m_finished) return makeError(ErrorCode::Busy, "instrumentation ingestion has finished");
  if (m_processes.count(pid) != 0) return ok();
  if (!m_dirCreated) {
    std::error_code ec;
    fs::create_directories(m_config.spoolDir, ec);
    if (ec)
      return makeError(ErrorCode::IoError,
                       "cannot create spool directory " + m_config.spoolDir.string() + ": " + ec.message());
    m_dirCreated = true;
  }
  const fs::path file = m_config.spoolDir / ("inst-" + std::to_string(pid) + ".tachyon-inst");
  TACHYON_TRY_ASSIGN(std::unique_ptr<Spool> spool, Spool::create(file, m_config.maxSpoolBytes));
  Spool* raw = spool.get();
  TACHYON_TRY_ASSIGN(std::unique_ptr<RawStreamServer> server,
                     RawStreamServer::listen(address, [raw]() -> std::unique_ptr<IStreamSink> {
                       return std::make_unique<SpoolSink>(*raw);
                     }));
  Process p;
  p.pid = pid;
  p.spool = std::move(spool);
  p.server = std::move(server);
  m_processes.emplace(pid, std::move(p));
  TACHYON_LOG_INFO("inst: listening for pid {} on {} (spool {})", pid, address, file.string());
  return ok();
}

std::vector<SpoolInfo> Ingest::snapshot() {
  std::lock_guard lock(m_mutex);
  if (m_finished) return m_final;
  std::vector<SpoolInfo> infos;
  for (auto& [pid, p] : m_processes) {
    SpoolInfo info;
    info.pid = pid;
    info.path = p.spool->path();
    auto size = p.spool->flush();
    info.flushedBytes = size.ok() ? *size : 0;
    info.stats = p.spool->stats();
    if (info.stats.framesWritten > 0) infos.push_back(std::move(info));
  }
  return infos;
}

std::vector<SpoolInfo> Ingest::finish() {
  std::map<uint32_t, Process> processes;
  {
    std::lock_guard lock(m_mutex);
    if (m_finished) return m_final;
    m_finished = true;
    processes.swap(m_processes);
  }
  // Drain all processes at once: each drain waits for silence.
  std::vector<std::thread> drains;
  for (auto& [pid, p] : processes) {
    RawStreamServer* server = p.server.get();
    drains.emplace_back([server, this] { server->shutdown(m_config.drainIdleMs, m_config.drainMaxMs); });
  }
  for (std::thread& t : drains) t.join();

  std::vector<SpoolInfo> infos;
  for (auto& [pid, p] : processes) {
    p.server.reset();
    SpoolInfo info;
    info.pid = pid;
    info.path = p.spool->path();
    if (auto closed = p.spool->close(); !closed.ok())
      TACHYON_LOG_WARN("inst: closing the spool of pid {}: {}", pid, closed.error().toString());
    info.stats = p.spool->stats();
    info.flushedBytes = info.stats.bytesWritten;
    const fs::path path = info.path;
    p.spool.reset();
    if (info.stats.framesWritten == 0) {
      std::error_code ec;
      fs::remove(path, ec);  // the process said hello but never sent anything
      continue;
    }
    infos.push_back(std::move(info));
  }
  std::lock_guard lock(m_mutex);
  m_final = infos;
  return infos;
}

void Ingest::removeSpoolDir() {
  std::error_code ec;
  fs::remove_all(m_config.spoolDir, ec);
}

void Ingest::discard() {
  (void)finish();
  removeSpoolDir();
}

}  // namespace tachyon::inst
