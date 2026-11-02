#include "capture/linux_perf/perf_records.h"

#include <algorithm>
#include <cstring>

namespace tachyon::capture::perf {

namespace {

// Sequential little-endian reader over a span; every read is checked and failure is sticky.
class Cursor {
 public:
  explicit Cursor(std::span<const uint8_t> data) : m_data(data) {}

  bool ok() const { return m_ok; }
  size_t offset() const { return m_off; }
  size_t remaining() const { return m_data.size() - m_off; }

  template <typename T>
  T get() {
    T v{};
    if (!m_ok || remaining() < sizeof(T)) {
      m_ok = false;
      return v;
    }
    std::memcpy(&v, m_data.data() + m_off, sizeof(T));
    m_off += sizeof(T);
    return v;
  }
  void skip(size_t n) {
    if (!m_ok || remaining() < n) {
      m_ok = false;
      return;
    }
    m_off += n;
  }
  const uint8_t* here() const { return m_data.data() + m_off; }

 private:
  std::span<const uint8_t> m_data;
  size_t m_off = 0;
  bool m_ok = true;
};

constexpr size_t popcountBits(uint64_t v) {
  size_t n = 0;
  for (; v != 0; v &= v - 1) ++n;
  return n;
}

// Trims a NUL-padded string area to its C string.
std::string_view trimCString(const uint8_t* p, size_t n) {
  const char* s = reinterpret_cast<const char*>(p);
  return std::string_view(s, ::strnlen(s, n));
}

// Reads the sample_id trailer (non-SAMPLE records). `c` must be positioned at the trailer's start.
void readTrailer(Cursor& c, const RecordLayout& layout, ParsedRecord& out) {
  const uint64_t st = layout.sampleType;
  if ((st & kSampleTid) != 0) {
    out.pid = c.get<uint32_t>();
    out.tid = c.get<uint32_t>();
  }
  if ((st & kSampleTime) != 0) {
    out.time = c.get<uint64_t>();
    out.hasTime = true;
  }
  if ((st & kSampleId) != 0) out.id = c.get<uint64_t>();
  if ((st & kSampleStreamId) != 0) out.streamId = c.get<uint64_t>();
  if ((st & kSampleCpu) != 0) {
    out.cpu = c.get<uint32_t>();
    c.skip(4);
  }
  if ((st & kSampleIdentifier) != 0) {
    const uint64_t identifier = c.get<uint64_t>();
    if ((st & kSampleId) == 0) out.id = identifier;
  }
}

ParseStatus parseSample(std::span<const uint8_t> body, const RecordLayout& layout, ParsedRecord& out) {
  Cursor c(body);
  const uint64_t st = layout.sampleType;
  if ((st & kSampleIdentifier) != 0) out.id = c.get<uint64_t>();
  if ((st & kSampleIp) != 0) out.ip = c.get<uint64_t>();
  if ((st & kSampleTid) != 0) {
    out.pid = c.get<uint32_t>();
    out.tid = c.get<uint32_t>();
  }
  if ((st & kSampleTime) != 0) {
    out.time = c.get<uint64_t>();
    out.hasTime = true;
  }
  if ((st & kSampleAddr) != 0) out.addr = c.get<uint64_t>();
  if ((st & kSampleId) != 0) out.id = c.get<uint64_t>();
  if ((st & kSampleStreamId) != 0) out.streamId = c.get<uint64_t>();
  if ((st & kSampleCpu) != 0) {
    out.cpu = c.get<uint32_t>();
    c.skip(4);
  }
  if ((st & kSamplePeriod) != 0) out.period = c.get<uint64_t>();
  if ((st & kSampleCallchain) != 0) {
    const uint64_t nr = c.get<uint64_t>();
    if (!c.ok()) return ParseStatus::Malformed;
    if (nr > c.remaining() / 8) return ParseStatus::Malformed;  // would run past the record
    out.chain = c.here();
    out.chainCount = nr;
    c.skip(static_cast<size_t>(nr) * 8);
  }
  return c.ok() ? ParseStatus::Ok : ParseStatus::Malformed;
}

}  // namespace

size_t RecordLayout::trailerSize() const {
  if (!sampleIdAll) return 0;
  const uint64_t mask =
      kSampleTid | kSampleTime | kSampleId | kSampleStreamId | kSampleCpu | kSampleIdentifier;
  return popcountBits(sampleType & mask) * 8;
}

ParseStatus readRecordHeader(std::span<const uint8_t> buf, RecordHeader& out) {
  if (buf.size() < kRecordHeaderSize) return ParseStatus::Truncated;
  std::memcpy(&out.type, buf.data(), 4);
  std::memcpy(&out.misc, buf.data() + 4, 2);
  std::memcpy(&out.size, buf.data() + 6, 2);
  if (out.size < kRecordHeaderSize || (out.size & 7u) != 0) return ParseStatus::Malformed;
  if (out.size > buf.size()) return ParseStatus::Truncated;
  return ParseStatus::Ok;
}

uint64_t ParsedRecord::chainAt(uint64_t i) const {
  uint64_t v = 0;
  if (chain != nullptr && i < chainCount) std::memcpy(&v, chain + i * 8, 8);
  return v;
}

ParseStatus parseRecord(std::span<const uint8_t> record, const RecordLayout& layout, ParsedRecord& out) {
  out = ParsedRecord{};
  const ParseStatus hs = readRecordHeader(record, out.header);
  if (hs != ParseStatus::Ok) return hs;
  if (out.header.size != record.size()) return ParseStatus::Malformed;
  if (!layout.supported()) return ParseStatus::Unsupported;

  const std::span<const uint8_t> body = record.subspan(kRecordHeaderSize);
  const size_t trailer = layout.trailerSize();

  // Splits the body into the type-specific part and the sample_id trailer; false when the record is too
  // small.
  auto split = [&](std::span<const uint8_t>& head, Cursor& tail) {
    if (body.size() < trailer) return false;
    head = body.first(body.size() - trailer);
    tail = Cursor(body.last(trailer));
    return true;
  };
  std::span<const uint8_t> head;
  Cursor tail(std::span<const uint8_t>{});

  switch (out.header.type) {
    case kRecordSample: return parseSample(body, layout, out);

    case kRecordMmap: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.pid = c.get<uint32_t>();
      out.tid = c.get<uint32_t>();
      out.mapAddr = c.get<uint64_t>();
      out.mapLen = c.get<uint64_t>();
      out.pgoff = c.get<uint64_t>();
      if (!c.ok()) return ParseStatus::Malformed;
      out.name = trimCString(c.here(), c.remaining());
      break;
    }
    case kRecordMmap2: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.pid = c.get<uint32_t>();
      out.tid = c.get<uint32_t>();
      out.mapAddr = c.get<uint64_t>();
      out.mapLen = c.get<uint64_t>();
      out.pgoff = c.get<uint64_t>();
      if ((out.header.misc & kMiscMmapBuildId) != 0) {
        const uint8_t size = c.get<uint8_t>();
        c.skip(3);
        if (size > sizeof(out.buildId)) return ParseStatus::Malformed;
        for (size_t i = 0; i < sizeof(out.buildId); ++i) {
          const uint8_t b = c.get<uint8_t>();
          if (i < size) out.buildId[i] = b;
        }
        out.buildIdSize = size;
      } else {
        out.devMajor = c.get<uint32_t>();
        out.devMinor = c.get<uint32_t>();
        out.inode = c.get<uint64_t>();
        out.inodeGeneration = c.get<uint64_t>();
      }
      out.prot = c.get<uint32_t>();
      out.mapFlags = c.get<uint32_t>();
      if (!c.ok()) return ParseStatus::Malformed;
      out.name = trimCString(c.here(), c.remaining());
      break;
    }
    case kRecordComm: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.pid = c.get<uint32_t>();
      out.tid = c.get<uint32_t>();
      if (!c.ok()) return ParseStatus::Malformed;
      out.name = trimCString(c.here(), c.remaining());
      break;
    }
    case kRecordFork:
    case kRecordExit: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.pid = c.get<uint32_t>();
      out.ppid = c.get<uint32_t>();
      out.tid = c.get<uint32_t>();
      out.ptid = c.get<uint32_t>();
      out.time = c.get<uint64_t>();
      out.hasTime = true;
      if (!c.ok()) return ParseStatus::Malformed;
      break;
    }
    case kRecordLost: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.id = c.get<uint64_t>();
      out.lost = c.get<uint64_t>();
      if (!c.ok()) return ParseStatus::Malformed;
      break;
    }
    case kRecordLostSamples: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.lost = c.get<uint64_t>();
      if (!c.ok()) return ParseStatus::Malformed;
      break;
    }
    case kRecordThrottle:
    case kRecordUnthrottle: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.time = c.get<uint64_t>();
      out.hasTime = true;
      out.id = c.get<uint64_t>();
      out.streamId = c.get<uint64_t>();
      if (!c.ok()) return ParseStatus::Malformed;
      break;
    }
    case kRecordSwitch: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      break;
    }
    case kRecordSwitchCpuWide: {
      if (!split(head, tail)) return ParseStatus::Malformed;
      Cursor c(head);
      out.nextPrevPid = c.get<uint32_t>();
      out.nextPrevTid = c.get<uint32_t>();
      if (!c.ok()) return ParseStatus::Malformed;
      break;
    }
    default: return ParseStatus::Unsupported;
  }

  if (trailer != 0) {
    // The trailer holds the emitting task, the record time and the cpu. Records whose body already carries a
    // pid/tid (MMAP, MMAP2, COMM, FORK, EXIT) keep the body values; THROTTLE/LOST keep their body ids.
    ParsedRecord t;
    readTrailer(tail, layout, t);
    if (!tail.ok()) return ParseStatus::Malformed;
    const uint32_t type = out.type();
    const bool bodyHasTid = type == kRecordMmap || type == kRecordMmap2 || type == kRecordComm ||
                            type == kRecordFork || type == kRecordExit;
    const bool bodyHasId = type == kRecordLost || type == kRecordThrottle || type == kRecordUnthrottle;
    if (!bodyHasTid) {
      out.pid = t.pid;
      out.tid = t.tid;
    }
    if (t.hasTime) {  // ordering uses the time the record was emitted
      out.time = t.time;
      out.hasTime = true;
    }
    out.cpu = t.cpu;
    if (!bodyHasId) {
      out.id = t.id;
      out.streamId = t.streamId;
    }
  }
  return ParseStatus::Ok;
}

// ---------------------------------------------------------------------------------------------------------
// RecordBuilder

size_t RecordBuilder::begin(uint32_t type, uint16_t misc) {
  const size_t start = m_bytes.size();
  putU32(type);
  putU16(misc);
  putU16(0);
  return start;
}

void RecordBuilder::end(size_t start) {
  while (((m_bytes.size() - start) & 7u) != 0) m_bytes.push_back(0);
  const uint16_t size = static_cast<uint16_t>(m_bytes.size() - start);
  std::memcpy(m_bytes.data() + start + 6, &size, 2);
}

void RecordBuilder::putU16(uint16_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  m_bytes.insert(m_bytes.end(), p, p + 2);
}
void RecordBuilder::putU32(uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  m_bytes.insert(m_bytes.end(), p, p + 4);
}
void RecordBuilder::putU64(uint64_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  m_bytes.insert(m_bytes.end(), p, p + 8);
}

void RecordBuilder::putString(std::string_view s) {
  m_bytes.insert(m_bytes.end(), s.begin(), s.end());
  m_bytes.push_back(0);
  while ((m_bytes.size() & 7u) != 0) m_bytes.push_back(0);  // buffers start 8-aligned relative to records
}

void RecordBuilder::trailer(const Id& id) {
  if (!m_layout.sampleIdAll) return;
  const uint64_t st = m_layout.sampleType;
  if ((st & kSampleTid) != 0) {
    putU32(id.pid);
    putU32(id.tid);
  }
  if ((st & kSampleTime) != 0) putU64(id.time);
  if ((st & kSampleId) != 0) putU64(id.id);
  if ((st & kSampleStreamId) != 0) putU64(id.streamId);
  if ((st & kSampleCpu) != 0) {
    putU32(id.cpu);
    putU32(0);
  }
  if ((st & kSampleIdentifier) != 0) putU64(id.id);
}

void RecordBuilder::sample(const Sample& s) {
  const size_t start = begin(kRecordSample, s.misc);
  const uint64_t st = m_layout.sampleType;
  if ((st & kSampleIdentifier) != 0) putU64(s.id.id);
  if ((st & kSampleIp) != 0) putU64(s.ip);
  if ((st & kSampleTid) != 0) {
    putU32(s.id.pid);
    putU32(s.id.tid);
  }
  if ((st & kSampleTime) != 0) putU64(s.id.time);
  if ((st & kSampleAddr) != 0) putU64(s.addr);
  if ((st & kSampleId) != 0) putU64(s.id.id);
  if ((st & kSampleStreamId) != 0) putU64(s.id.streamId);
  if ((st & kSampleCpu) != 0) {
    putU32(s.id.cpu);
    putU32(0);
  }
  if ((st & kSamplePeriod) != 0) putU64(s.period);
  if ((st & kSampleCallchain) != 0) {
    putU64(s.chain.size());
    for (uint64_t ip : s.chain) putU64(ip);
  }
  end(start);
}

void RecordBuilder::mmap2(const Mmap2& m) {
  uint16_t misc = m.misc;
  if (!m.buildId.empty()) misc = static_cast<uint16_t>(misc | kMiscMmapBuildId);
  const size_t start = begin(kRecordMmap2, misc);
  putU32(m.pid);
  putU32(m.tid);
  putU64(m.addr);
  putU64(m.len);
  putU64(m.pgoff);
  if (!m.buildId.empty()) {
    const size_t n = std::min<size_t>(m.buildId.size(), 20);
    m_bytes.push_back(static_cast<uint8_t>(n));
    m_bytes.push_back(0);
    m_bytes.push_back(0);
    m_bytes.push_back(0);
    for (size_t i = 0; i < 20; ++i) m_bytes.push_back(i < n ? m.buildId[i] : 0);
  } else {
    putU32(m.devMajor);
    putU32(m.devMinor);
    putU64(m.inode);
    putU64(m.inodeGeneration);
  }
  putU32(m.prot);
  putU32(m.flags);
  // The string area is relative to the record start; 64 bytes precede it, so padding to 8 works on the
  // absolute offset because every record starts at a multiple of 8.
  putString(m.name);
  trailer(m.id);
  end(start);
}

void RecordBuilder::mmap(uint32_t pid, uint32_t tid, uint64_t addr, uint64_t len, uint64_t pgoff,
                         std::string_view name, const Id& id, uint16_t misc) {
  const size_t start = begin(kRecordMmap, misc);
  putU32(pid);
  putU32(tid);
  putU64(addr);
  putU64(len);
  putU64(pgoff);
  putString(name);
  trailer(id);
  end(start);
}

void RecordBuilder::comm(uint32_t pid, uint32_t tid, std::string_view name, bool exec, const Id& id) {
  const size_t start = begin(kRecordComm, exec ? kMiscCommExec : uint16_t{0});
  putU32(pid);
  putU32(tid);
  putString(name);
  trailer(id);
  end(start);
}

void RecordBuilder::fork(uint32_t pid, uint32_t ppid, uint32_t tid, uint32_t ptid, uint64_t time,
                         const Id& id) {
  const size_t start = begin(kRecordFork, 0);
  putU32(pid);
  putU32(ppid);
  putU32(tid);
  putU32(ptid);
  putU64(time);
  trailer(id);
  end(start);
}

void RecordBuilder::exit(uint32_t pid, uint32_t ppid, uint32_t tid, uint32_t ptid, uint64_t time,
                         const Id& id) {
  const size_t start = begin(kRecordExit, 0);
  putU32(pid);
  putU32(ppid);
  putU32(tid);
  putU32(ptid);
  putU64(time);
  trailer(id);
  end(start);
}

void RecordBuilder::lost(uint64_t idValue, uint64_t lostCount, const Id& id) {
  const size_t start = begin(kRecordLost, 0);
  putU64(idValue);
  putU64(lostCount);
  trailer(id);
  end(start);
}

void RecordBuilder::lostSamples(uint64_t lostCount, const Id& id) {
  const size_t start = begin(kRecordLostSamples, 0);
  putU64(lostCount);
  trailer(id);
  end(start);
}

void RecordBuilder::throttle(bool unthrottle, uint64_t time, uint64_t idValue, uint64_t streamId,
                             const Id& id) {
  const size_t start = begin(unthrottle ? kRecordUnthrottle : kRecordThrottle, 0);
  putU64(time);
  putU64(idValue);
  putU64(streamId);
  trailer(id);
  end(start);
}

void RecordBuilder::switchCpuWide(bool out, uint32_t nextPrevPid, uint32_t nextPrevTid, const Id& id) {
  const size_t start = begin(kRecordSwitchCpuWide, out ? kMiscSwitchOut : uint16_t{0});
  putU32(nextPrevPid);
  putU32(nextPrevTid);
  trailer(id);
  end(start);
}

void RecordBuilder::raw(uint32_t type, uint16_t misc, size_t bodyBytes, const Id& id, bool withTrailer) {
  const size_t start = begin(type, misc);
  m_bytes.insert(m_bytes.end(), bodyBytes, uint8_t{0});
  while ((m_bytes.size() & 7u) != 0) m_bytes.push_back(0);
  if (withTrailer) trailer(id);
  end(start);
}

}  // namespace tachyon::capture::perf
