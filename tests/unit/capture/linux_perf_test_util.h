// Shared helpers of the perf backend tests (WP-70): temp directories, generated ELF files, a fake
// perf_event_open layer and a fake /proc.
#pragma once

#include "capture/linux_perf/perf_records.h"
#include "capture/linux_perf/perf_system.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::capture::perf::ptest {

namespace fs = std::filesystem;

class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    m_path = fs::temp_directory_path() /
             ("tachyon-perf-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(m_path);
    fs::create_directories(m_path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(m_path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const fs::path& path() const { return m_path; }

 private:
  fs::path m_path;
};

// --- generated ELF files ----------------------------------------------------------------------------------

struct ElfSegment {
  uint64_t vaddr = 0, offset = 0, filesz = 0, memsz = 0;
  uint32_t flags = 5;  // R + X
};

struct ElfSpec {
  std::vector<uint8_t> buildId;  // empty: no note section
  std::vector<ElfSegment> segments;
  uint16_t type = 3;  // ET_DYN
};

// Writes a minimal ELF64 little-endian file: header, program headers, an optional .note.gnu.build-id and a
// section name table. Enough for symbols::ElfFile to open it and read the build-id and the PT_LOAD headers.
inline void writeElf(const fs::path& path, const ElfSpec& spec) {
  std::vector<uint8_t> f(64, 0);
  auto put16 = [&](size_t at, uint16_t v) { std::memcpy(f.data() + at, &v, 2); };
  auto put32 = [&](size_t at, uint32_t v) { std::memcpy(f.data() + at, &v, 4); };
  auto put64 = [&](size_t at, uint64_t v) { std::memcpy(f.data() + at, &v, 8); };
  f[0] = 0x7f;
  f[1] = 'E';
  f[2] = 'L';
  f[3] = 'F';
  f[4] = 2;  // 64-bit
  f[5] = 1;  // little endian
  f[6] = 1;
  put16(16, spec.type);
  put16(18, 62);  // x86-64
  put32(20, 1);
  put16(52, 64);
  put16(54, 56);
  put16(56, static_cast<uint16_t>(spec.segments.size()));
  put16(58, 64);

  const size_t phAt = f.size();
  f.resize(f.size() + spec.segments.size() * 56, 0);
  for (size_t i = 0; i < spec.segments.size(); ++i) {
    const ElfSegment& s = spec.segments[i];
    const size_t at = phAt + i * 56;
    put32(at, 1);  // PT_LOAD
    put32(at + 4, s.flags);
    put64(at + 8, s.offset);
    put64(at + 16, s.vaddr);
    put64(at + 24, s.vaddr);
    put64(at + 32, s.filesz);
    put64(at + 40, s.memsz);
    put64(at + 48, 0x1000);
  }
  put64(32, spec.segments.empty() ? 0 : 64);  // e_phoff

  // Note section.
  const size_t noteAt = f.size();
  size_t noteSize = 0;
  if (!spec.buildId.empty()) {
    const uint32_t hdr[3] = {4, static_cast<uint32_t>(spec.buildId.size()), 3};
    const auto* h = reinterpret_cast<const uint8_t*>(hdr);
    f.insert(f.end(), h, h + 12);
    f.insert(f.end(), {'G', 'N', 'U', 0});
    f.insert(f.end(), spec.buildId.begin(), spec.buildId.end());
    while (f.size() % 4 != 0) f.push_back(0);
    noteSize = f.size() - noteAt;
  }
  // Section name table.
  const size_t strAt = f.size();
  const std::string names =
      std::string("\0", 1) + ".note.gnu.build-id" + std::string("\0", 1) + ".shstrtab" + std::string("\0", 1);
  f.insert(f.end(), names.begin(), names.end());
  while (f.size() % 8 != 0) f.push_back(0);
  // Section headers: null, note, shstrtab.
  const size_t shAt = f.size();
  f.resize(f.size() + 3 * 64, 0);
  auto shField = [&](size_t index, size_t field, uint64_t v, size_t width) {
    std::memcpy(f.data() + shAt + index * 64 + field, &v, width);
  };
  shField(1, 0, 1, 4);  // name offset of ".note.gnu.build-id"
  shField(1, 4, 7, 4);  // SHT_NOTE
  shField(1, 24, noteAt, 8);
  shField(1, 32, noteSize, 8);
  shField(2, 0, 1 + std::string(".note.gnu.build-id").size() + 1, 4);
  shField(2, 4, 3, 4);  // SHT_STRTAB
  shField(2, 24, strAt, 8);
  shField(2, 32, names.size(), 8);
  put64(40, shAt);
  put16(60, 3);
  put16(62, 2);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
}

// --- fake perf layer --------------------------------------------------------------------------------------

class FakeRing final : public IRing {
 public:
  RingStatus drain(std::vector<uint8_t>& out) override {
    std::lock_guard lock(m_mutex);
    if (m_pending.empty()) return RingStatus::Empty;
    out.insert(out.end(), m_pending.begin(), m_pending.end());
    m_pending.clear();
    return RingStatus::Data;
  }
  int pollFd() const override { return -1; }
  uint64_t overruns() const override { return 0; }
  void push(const std::vector<uint8_t>& bytes) {
    std::lock_guard lock(m_mutex);
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
  }

 private:
  std::mutex m_mutex;
  std::vector<uint8_t> m_pending;
};

// An IRing that reads a FakeRing owned by the test.
class RefRing final : public IRing {
 public:
  explicit RefRing(FakeRing& ring) : m_ring(ring) {}
  RingStatus drain(std::vector<uint8_t>& out) override { return m_ring.drain(out); }
  int pollFd() const override { return -1; }
  uint64_t overruns() const override { return 0; }

 private:
  FakeRing& m_ring;
};

// Hands out the shared FakeRing; the test keeps a pointer through FakePerfSystem::ring().
class FakeRingHandle final : public IRing {
 public:
  explicit FakeRingHandle(std::shared_ptr<FakeRing> ring) : m_ring(std::move(ring)) {}
  RingStatus drain(std::vector<uint8_t>& out) override { return m_ring->drain(out); }
  int pollFd() const override { return -1; }
  uint64_t overruns() const override { return 0; }

 private:
  std::shared_ptr<FakeRing> m_ring;
};

class FakePerfSystem : public IPerfSystem {
 public:
  struct Event {
    int fd = -1;
    EventConfig config;
    int pid = 0, cpu = 0;
    bool enabled = false;
    bool closed = false;
    int outputTarget = -1;  // set_output redirect
    std::shared_ptr<FakeRing> ring;
  };

  // Behaviour knobs (set before use).
  int failSystemWideWith = 0;  // errno for openEvent(pid = -1, ...)
  int failPerThreadWith = 0;   // errno for openEvent(pid >= 0, ...)
  int failMapWith = 0;         // errno for mapRing
  bool kernelBuildId = true;
  std::vector<uint32_t> cpus{0, 1};
  SysctlInfo sys{-1, 100000, 25, 516, true};
  fs::path proc = "/proc";
  // Called for every call, with its description, without the internal lock. For ordering assertions.
  std::function<void(const std::string&)> hook;

  OpenOutcome openEvent(const EventConfig& config, int pid, int cpu) override {
    note("open pid=" + std::to_string(pid) + " cpu=" + std::to_string(cpu));
    OpenOutcome out;
    if (pid < 0 && failSystemWideWith != 0) {
      out.err = failSystemWideWith;
      return out;
    }
    if (pid >= 0 && failPerThreadWith != 0) {
      out.err = failPerThreadWith;
      return out;
    }
    std::lock_guard lock(m_mutex);
    Event e;
    e.fd = m_nextFd++;
    e.config = config;
    e.pid = pid;
    e.cpu = cpu;
    e.ring = std::make_shared<FakeRing>();
    m_events.push_back(e);
    out.fd = e.fd;
    out.kernelBuildId = kernelBuildId;
    return out;
  }
  RingOutcome mapRing(int fd, uint32_t) override {
    RingOutcome out;
    if (failMapWith != 0) {
      out.err = failMapWith;
      return out;
    }
    std::lock_guard lock(m_mutex);
    for (Event& e : m_events)
      if (e.fd == fd) out.ring = std::make_unique<FakeRingHandle>(e.ring);
    return out;
  }
  bool setEnabled(int fd, bool enabled) override {
    note(std::string(enabled ? "enable" : "disable") + " fd=" + std::to_string(fd));
    std::lock_guard lock(m_mutex);
    for (Event& e : m_events)
      if (e.fd == fd) e.enabled = enabled;
    return true;
  }
  bool setOutput(int fd, int targetFd) override {
    note("set_output fd=" + std::to_string(fd) + " target=" + std::to_string(targetFd));
    std::lock_guard lock(m_mutex);
    for (Event& e : m_events)
      if (e.fd == fd) e.outputTarget = targetFd;
    return true;
  }
  void closeEvent(int fd) override {
    note("close fd=" + std::to_string(fd));
    std::lock_guard lock(m_mutex);
    for (Event& e : m_events)
      if (e.fd == fd) e.closed = true;
  }
  std::vector<uint32_t> onlineCpus() override { return cpus; }
  SysctlInfo sysctls() override { return sys; }
  uint64_t nowNs() override {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
  }
  fs::path procRoot() override { return proc; }

  std::vector<Event> events() const {
    std::lock_guard lock(m_mutex);
    return m_events;
  }
  std::vector<std::string> log() const {
    std::lock_guard lock(m_mutex);
    return m_log;
  }
  // The ring of the n-th opened event.
  std::shared_ptr<FakeRing> ring(size_t n) const {
    std::lock_guard lock(m_mutex);
    return n < m_events.size() ? m_events[n].ring : nullptr;
  }

 private:
  void note(const std::string& what) {
    {
      std::lock_guard lock(m_mutex);
      m_log.push_back(what);
    }
    if (hook) hook(what);
  }

  mutable std::mutex m_mutex;
  std::vector<Event> m_events;
  std::vector<std::string> m_log;
  int m_nextFd = 100;
};

// A fake proc filesystem: <root>/<pid>/{stat,comm,maps,task/<tid>/comm,cmdline}.
inline void writeFakeProcess(const fs::path& root, uint32_t pid, uint32_t ppid, const std::string& comm,
                             const std::vector<uint32_t>& tids, const std::vector<std::string>& mapsLines,
                             char state = 'S') {
  const fs::path dir = root / std::to_string(pid);
  fs::create_directories(dir / "task");
  std::ofstream(dir / "stat") << pid << " (" << comm << ") " << state << " " << ppid << " 0 0 0 0 0\n";
  std::ofstream(dir / "comm") << comm << "\n";
  std::ofstream(dir / "cmdline") << comm << std::string("\0", 1) << "--arg" << std::string("\0", 1);
  std::ofstream maps(dir / "maps");
  for (const std::string& line : mapsLines) maps << line << "\n";
  for (uint32_t tid : tids) {
    fs::create_directories(dir / "task" / std::to_string(tid));
    std::ofstream(dir / "task" / std::to_string(tid) / "comm") << comm << "\n";
  }
}

// Waits until `pred` holds or the timeout expires.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

}  // namespace tachyon::capture::perf::ptest
