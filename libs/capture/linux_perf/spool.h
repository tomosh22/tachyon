// The spool: what the drain thread writes while a perf capture runs, and what the converter reads at stop /
// flush (WP-70). It plays the role the .etl file plays for the ETW backend.
//
// A spool is a directory:
//   session.json       SessionInfo: how the events were configured (sample_type, clock, sampling rate, plan,
//                      target, host facts), written at start and rewritten at stop with the end time and the
//                      drain counters.
//   meta.bin           every non-SAMPLE record (MMAP2, COMM, FORK, EXIT, LOST, THROTTLE, ...) plus the /proc
//                      rundown records written at start. Never rotated, so a circular capture keeps the
//                      module and thread history of everything that ever ran.
//   data-NNNNNN.bin    SAMPLE records. One segment in a normal capture; in a circular capture the segments
//                      rotate and the oldest are deleted to stay within the size budget.
// Each file is a SpoolFileHeader followed by batches: {SpoolBatchHeader, raw perf records}. A batch holds
// whole records exactly as the kernel wrote them, from ONE ring (so its timestamps are non-decreasing); the
// ring id lets the converter rebuild one ordered stream per ring and merge them. A crash leaves a truncated
// last batch, which the reader clips.
#pragma once

#include "capture/linux_perf/perf_records.h"
#include "core/result.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tachyon::capture::perf {

inline constexpr char kSpoolMagic[8] = {'T', 'P', 'E', 'R', 'F', 'S', 'P', '\0'};
inline constexpr uint32_t kSpoolVersion = 1;
inline constexpr uint32_t kSpoolBatchMagic = 0x31425054;  // "TPB1"
// Ring id of the records synthesised from /proc when the capture starts (they are not read from a ring).
inline constexpr uint32_t kSpoolRingRundown = 0xFFFFFFFFu;
// A batch larger than this is treated as corruption by the reader.
inline constexpr uint64_t kMaxSpoolBatchBytes = 256ull << 20;

enum class SpoolKind : uint32_t { Meta = 1, Data = 2 };

#pragma pack(push, 1)
struct SpoolFileHeader {
  char magic[8];
  uint32_t version;
  uint32_t kind;  // SpoolKind
  uint64_t sampleType;
  uint32_t sampleIdAll;
  uint32_t reserved;
};
struct SpoolBatchHeader {
  uint32_t magic;
  uint32_t ring;
  uint64_t byteLen;
};
#pragma pack(pop)
static_assert(sizeof(SpoolFileHeader) == 32);
static_assert(sizeof(SpoolBatchHeader) == 16);

// How the scheduler events were recorded (WP-71). Serialised inside session.json so that the converter needs
// nothing from the machine it runs on.
struct SchedInfo {
  std::string mode = "none";  // "none" | "tracepoint" | "context_switch"
  bool switches = false;      // context-switch events were requested (and recorded)
  bool wakeups = false;       // ready-thread (wakeup) events were requested (and recorded)
  std::string switchFormat;   // tracefs events/sched/sched_switch/format (tracepoint mode)
  std::string wakeFormat;     // format of the wakeup tracepoint
  std::string wakeEvent;      // "sched_waking" | "sched_wakeup"
  std::string reason;         // why the tracepoint path was not used (context_switch / none)
};

// How the capture was configured and what the drain saw. Serialised as session.json.
struct SessionInfo {
  uint32_t version = 1;
  RecordLayout layout;
  uint64_t startTs = 0, endTs = 0;  // CLOCK_MONOTONIC_RAW nanoseconds
  uint32_t samplingHz = 0;
  std::string mode;  // "launch" | "attach" | "system"
  uint32_t rootPid = 0;
  bool followChildren = false;
  std::string childFilter;
  std::string exe;
  std::vector<std::string> args;
  uint32_t cpuCount = 0;
  uint32_t ringCount = 0;
  std::string plan;  // "system-wide" | "per-thread"
  bool kernelStacks = true;
  bool excludeKernel = false;
  bool kernelBuildId = false;  // MMAP2 records carry the kernel-provided build-id when the file allows it
  int paranoid = 0;
  int64_t maxSampleRate = 0;
  int cpuTimeMaxPercent = 0;
  uint32_t circularMb = 0;
  SchedInfo sched;
  std::string kernelRelease, hostname, agentVersion;
  // Filled at stop.
  uint64_t drainedBytes = 0;
  uint64_t ringOverruns = 0;
  uint64_t corruptBatches = 0;
  std::vector<std::string> warnings;
  bool finished = false;  // the capture was stopped cleanly (endTs valid)

  // Thread-safe: pure function.
  std::string toJson() const;
  // Thread-safe: pure function. CorruptData on malformed JSON or a wrong version.
  static Result<SessionInfo> fromJson(const std::string& text);
};

// Not thread-safe (the drain thread owns one writer per file). I/O errors are sticky and reported by append /
// close.
class SpoolWriter {
 public:
  // Thread-safe: creates `path` and writes the file header.
  static Result<SpoolWriter> create(const std::filesystem::path& path, SpoolKind kind,
                                    const RecordLayout& layout);
  SpoolWriter(SpoolWriter&&) noexcept;
  SpoolWriter& operator=(SpoolWriter&&) noexcept;
  SpoolWriter(const SpoolWriter&) = delete;
  SpoolWriter& operator=(const SpoolWriter&) = delete;
  ~SpoolWriter();

  // Not thread-safe. Appends one batch of whole records read from ring `ring`.
  Result<void> append(uint32_t ring, std::span<const uint8_t> records);
  // Not thread-safe. Pushes buffered bytes to the OS.
  Result<void> flush();
  // Not thread-safe. Flushes and closes; idempotent.
  Result<void> close();
  // Not thread-safe. File size so far, header included.
  uint64_t bytesWritten() const { return m_bytes; }
  const std::filesystem::path& path() const { return m_path; }

 private:
  SpoolWriter() = default;
  std::filesystem::path m_path;
  std::FILE* m_file = nullptr;
  uint64_t m_bytes = 0;
  bool m_failed = false;
};

struct SpoolBatchRef {
  uint32_t ring = 0;
  uint64_t offset = 0;  // of the payload
  uint64_t length = 0;
};

// Read access to one spool file. Thread-safe for const members after open (pread).
class SpoolFile {
 public:
  // Thread-safe. Validates the header and indexes the batches. A truncated or corrupt tail is tolerated: the
  // batches before it (and the readable part of a truncated one) are kept and truncated() is set.
  static Result<SpoolFile> open(const std::filesystem::path& path);
  SpoolFile(SpoolFile&&) noexcept;
  SpoolFile& operator=(SpoolFile&&) noexcept;
  SpoolFile(const SpoolFile&) = delete;
  SpoolFile& operator=(const SpoolFile&) = delete;
  ~SpoolFile();

  SpoolKind kind() const { return m_kind; }
  const RecordLayout& layout() const { return m_layout; }
  const std::vector<SpoolBatchRef>& batches() const { return m_batches; }
  bool truncated() const { return m_truncated; }
  const std::filesystem::path& path() const { return m_path; }
  // Thread-safe. Reads a batch payload into `out` (replacing its contents).
  Result<void> read(const SpoolBatchRef& batch, std::vector<uint8_t>& out) const;

 private:
  SpoolFile() = default;
  std::filesystem::path m_path;
  int m_fd = -1;
  SpoolKind m_kind = SpoolKind::Data;
  RecordLayout m_layout;
  std::vector<SpoolBatchRef> m_batches;
  bool m_truncated = false;
};

// The files of a spool directory.
struct SpoolSet {
  std::filesystem::path dir;
  SessionInfo info;
  std::filesystem::path meta;
  std::vector<std::filesystem::path> data;  // in segment order

  // Thread-safe. Reads session.json and lists meta.bin / data-*.bin. NotFound when the directory has no
  // session.json; a missing meta.bin is allowed (an empty capture).
  static Result<SpoolSet> openDirectory(const std::filesystem::path& dir);
};

// Thread-safe: pure functions. File names inside a spool directory.
std::filesystem::path spoolSessionPath(const std::filesystem::path& dir);
std::filesystem::path spoolMetaPath(const std::filesystem::path& dir);
std::filesystem::path spoolDataPath(const std::filesystem::path& dir, uint32_t segment);

// Thread-safe. Writes session.json atomically (temp file + rename).
Result<void> writeSessionInfo(const std::filesystem::path& dir, const SessionInfo& info);

// One time-ordered stream of parsed records: all batches of one ring (of one class) in file order. Records
// that are not decodable (Unsupported types) are skipped and counted; a malformed record ends the batch.
// Not thread-safe.
class RecordStream {
 public:
  struct Extent {
    const SpoolFile* file = nullptr;
    SpoolBatchRef batch;
  };
  RecordStream(RecordLayout layout, std::vector<Extent> extents)
      : m_layout(layout), m_extents(std::move(extents)) {}

  // Moves to the next record; false at the end. The record (and its aliased pointers) stays valid until the
  // next call.
  bool advance();
  const ParsedRecord& current() const { return m_record; }

  uint64_t skippedUnsupported() const { return m_skippedUnsupported; }
  uint64_t malformedBatches() const { return m_malformed; }
  uint64_t readErrors() const { return m_readErrors; }

 private:
  bool loadNextBatch();

  RecordLayout m_layout;
  std::vector<Extent> m_extents;
  size_t m_nextExtent = 0;
  std::vector<uint8_t> m_buf;
  size_t m_pos = 0;
  ParsedRecord m_record;
  uint64_t m_skippedUnsupported = 0, m_malformed = 0, m_readErrors = 0;
};

}  // namespace tachyon::capture::perf
