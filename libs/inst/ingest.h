// Agent-side ingestion of one capture: a listener per instrumented process (opened when the process says
// hello) and one spool file per process. Raw frames go straight from the connection into the spool; nothing
// is decoded here.
#pragma once

#include "core/result.h"
#include "inst/raw_stream_server.h"
#include "inst/spool.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tachyon::inst {

struct IngestConfig {
  // Directory for the spool files; created at the first attach().
  std::filesystem::path spoolDir;
  uint64_t maxSpoolBytes = kDefaultMaxSpoolBytes;  // per process
  // At finish(): each connection is read until it was silent for drainIdleMs or drainMaxMs elapsed.
  int drainIdleMs = 200;
  int drainMaxMs = 2000;
};

struct SpoolInfo {
  uint32_t pid = 0;
  std::filesystem::path path;
  SpoolStats stats;
  uint64_t flushedBytes = 0;  // size of the file at a frame boundary (snapshot()) or its final size
};

class Ingest {
 public:
  // Thread-safe.
  explicit Ingest(IngestConfig config);
  // Thread-safe: calls finish().
  ~Ingest();
  Ingest(const Ingest&) = delete;
  Ingest& operator=(const Ingest&) = delete;

  // Thread-safe. Starts listening on `address` for process `pid` and spooling what it sends. Idempotent for a
  // pid that is already attached. Fails after finish().
  Result<void> attach(uint32_t pid, const std::string& address);

  // Thread-safe, safe while streams are live. Writes buffered frames out and returns, per attached process,
  // the spool path and its size at a frame boundary.
  std::vector<SpoolInfo> snapshot();

  // Thread-safe, idempotent. Stops accepting, drains the open connections (see IngestConfig), closes the
  // spools (a spool that overflowed gets its closing Dropped frame) and removes spools that never received a
  // frame. Returns the final state of the remaining spools.
  std::vector<SpoolInfo> finish();

  // Thread-safe. finish() and delete the spool directory.
  void discard();
  // Thread-safe. Deletes the spool directory (after a successful merge).
  void removeSpoolDir();

  // Thread-safe.
  const std::filesystem::path& spoolDir() const { return m_config.spoolDir; }
  // Thread-safe. Processes attached so far.
  size_t processCount() const;

 private:
  struct Process {
    uint32_t pid = 0;
    std::unique_ptr<Spool> spool;
    std::unique_ptr<RawStreamServer> server;
  };

  IngestConfig m_config;
  mutable std::mutex m_mutex;
  std::map<uint32_t, Process> m_processes;
  bool m_finished = false;
  bool m_dirCreated = false;
  std::vector<SpoolInfo> m_final;
};

}  // namespace tachyon::inst
