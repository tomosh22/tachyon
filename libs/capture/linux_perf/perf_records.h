// Bounds-checked parsing of perf_event_open records (WP-70) and a builder for the same layout.
//
// Records arrive as raw bytes: from a ring buffer (copied out, so a record that wrapped around the end of the
// ring is contiguous again), from the spool file, or from byte fixtures in tests. Nothing here trusts the
// bytes: every field read is checked against the record size, unknown record types are reported as
// Unsupported so the caller skips them by header.size, and a record whose header is inconsistent is
// Malformed. The parser never reads outside the span it is given.
//
// The layout of a record depends on perf_event_attr::sample_type (for SAMPLE records, and for the
// sample_id trailer of every other record when sample_id_all is set): RecordLayout carries both.
#pragma once

#include "capture/linux_perf/perf_abi.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace tachyon::capture::perf {

struct RecordLayout {
  uint64_t sampleType = kBackendSampleType;
  bool sampleIdAll = true;

  // Thread-safe: pure function. True when every sample_type bit is one the parser decodes.
  bool supported() const { return (sampleType & ~kSupportedSampleTypeMask) == 0; }
  // Thread-safe: pure function. Bytes of the sample_id trailer appended to non-SAMPLE records.
  size_t trailerSize() const;
  bool operator==(const RecordLayout&) const = default;
};

enum class ParseStatus : uint8_t {
  Ok,
  Truncated,    // fewer bytes than the header (or the body) needs
  Malformed,    // inconsistent header or body (size < 8, not a multiple of 8, string area negative, ...)
  Unsupported,  // a record type the parser does not decode; skip it by header.size
};

struct RecordHeader {
  uint32_t type = 0;
  uint16_t misc = 0;
  uint16_t size = 0;
};

// Thread-safe: pure function. Reads the 8-byte header at the start of `buf`. Ok only when the whole record
// (header.size bytes) is available; size < 8 or size % 8 != 0 is Malformed, a short buffer is Truncated.
ParseStatus readRecordHeader(std::span<const uint8_t> buf, RecordHeader& out);

// One decoded record. Pointers (`chain`, `name`) alias the input buffer and are valid as long as it is.
// Only the fields of the record's type are set; the rest are zero.
struct ParsedRecord {
  RecordHeader header;

  // sample_id (the trailer of non-SAMPLE records, the body of SAMPLE records) and body fields of
  // FORK / EXIT / THROTTLE.
  uint32_t pid = 0, tid = 0;
  uint64_t time = 0;
  uint32_t cpu = 0;
  uint64_t id = 0, streamId = 0;
  bool hasTime = false;  // time was present (sample_type TIME / body field)

  // FORK / EXIT
  uint32_t ppid = 0, ptid = 0;

  // SAMPLE
  uint64_t ip = 0, addr = 0, period = 0;
  const uint8_t* chain = nullptr;  // chainCount little-endian u64 values, not necessarily aligned
  uint64_t chainCount = 0;

  // MMAP / MMAP2
  uint64_t mapAddr = 0, mapLen = 0, pgoff = 0;
  uint32_t devMajor = 0, devMinor = 0;
  uint64_t inode = 0, inodeGeneration = 0;
  uint8_t buildIdSize = 0;  // > 0 only when the MMAP2 record carried a build-id (kMiscMmapBuildId)
  uint8_t buildId[20] = {};
  uint32_t prot = 0, mapFlags = 0;

  // MMAP / MMAP2: file name; COMM: command name. NUL-terminated inside the record, trimmed here.
  std::string_view name;

  // LOST / LOST_SAMPLES / THROTTLE / UNTHROTTLE
  uint64_t lost = 0;

  // SWITCH_CPU_WIDE
  uint32_t nextPrevPid = 0, nextPrevTid = 0;

  // Thread-safe: pure function. chain[i] for i < chainCount.
  uint64_t chainAt(uint64_t i) const;
  uint32_t type() const { return header.type; }
  uint16_t misc() const { return header.misc; }
  uint16_t cpuMode() const { return header.misc & kMiscCpuModeMask; }
  bool isExecComm() const { return header.type == kRecordComm && (header.misc & kMiscCommExec) != 0; }
};

// Thread-safe: pure function. Decodes one whole record (`record.size()` must equal the header's size field;
// use readRecordHeader first, or forEachRecord). On a non-Ok status `out` is unspecified.
ParseStatus parseRecord(std::span<const uint8_t> record, const RecordLayout& layout, ParsedRecord& out);

// Thread-safe: pure function. Walks a buffer of back-to-back records and calls fn(recordBytes) for each
// well-formed one; stops at the first Truncated/Malformed header and reports it. `consumed` is the number of
// bytes before the stop.
template <typename Fn>
ParseStatus forEachRecord(std::span<const uint8_t> buf, size_t& consumed, Fn&& fn) {
  consumed = 0;
  while (consumed < buf.size()) {
    RecordHeader h;
    const ParseStatus st = readRecordHeader(buf.subspan(consumed), h);
    if (st != ParseStatus::Ok) return st;
    fn(buf.subspan(consumed, h.size));
    consumed += h.size;
  }
  return ParseStatus::Ok;
}

// Builds records in the kernel's layout (for synthetic /proc rundown records and for tests). Not thread-safe.
class RecordBuilder {
 public:
  explicit RecordBuilder(RecordLayout layout = {}) : m_layout(layout) {}

  struct Id {
    uint32_t pid = 0, tid = 0;
    uint64_t time = 0;
    uint32_t cpu = 0;
    uint64_t id = 0, streamId = 0;
  };
  struct Sample {
    uint64_t ip = 0, addr = 0, period = 0;
    Id id;
    std::vector<uint64_t> chain;
    uint16_t misc = kMiscUser;
  };
  struct Mmap2 {
    uint32_t pid = 0, tid = 0;
    uint64_t addr = 0, len = 0, pgoff = 0;
    uint32_t devMajor = 0, devMinor = 0;
    uint64_t inode = 0, inodeGeneration = 0;
    std::vector<uint8_t> buildId;                      // non-empty (<= 20 bytes): use the build-id layout
    uint32_t prot = kProtRead | kProtExec, flags = 2;  // MAP_PRIVATE
    std::string_view name;
    uint16_t misc = kMiscUser;
    Id id;
  };

  void sample(const Sample& s);
  void mmap2(const Mmap2& m);
  void mmap(uint32_t pid, uint32_t tid, uint64_t addr, uint64_t len, uint64_t pgoff, std::string_view name,
            const Id& id, uint16_t misc = kMiscUser);
  void comm(uint32_t pid, uint32_t tid, std::string_view name, bool exec, const Id& id);
  void fork(uint32_t pid, uint32_t ppid, uint32_t tid, uint32_t ptid, uint64_t time, const Id& id);
  void exit(uint32_t pid, uint32_t ppid, uint32_t tid, uint32_t ptid, uint64_t time, const Id& id);
  void lost(uint64_t idValue, uint64_t lostCount, const Id& id);
  void lostSamples(uint64_t lostCount, const Id& id);
  void throttle(bool unthrottle, uint64_t time, uint64_t idValue, uint64_t streamId, const Id& id);
  void switchCpuWide(bool out, uint32_t nextPrevPid, uint32_t nextPrevTid, const Id& id);
  // A record of an arbitrary type with `bodyBytes` zero bytes after the header (rounded up to 8), followed
  // by the trailer when `withTrailer`. For "unknown record type" tests.
  void raw(uint32_t type, uint16_t misc, size_t bodyBytes, const Id& id, bool withTrailer = true);

  const std::vector<uint8_t>& bytes() const { return m_bytes; }
  std::vector<uint8_t> take() { return std::move(m_bytes); }
  void clear() { m_bytes.clear(); }
  const RecordLayout& layout() const { return m_layout; }

 private:
  size_t begin(uint32_t type, uint16_t misc);  // returns the record start offset
  void end(size_t start);                      // pads to 8 and patches the size
  void putU16(uint16_t v);
  void putU32(uint32_t v);
  void putU64(uint64_t v);
  void putString(std::string_view s);  // NUL-terminated, padded to 8
  void trailer(const Id& id);

  RecordLayout m_layout;
  std::vector<uint8_t> m_bytes;
};

}  // namespace tachyon::capture::perf
