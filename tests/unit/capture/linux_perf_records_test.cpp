// WP-70: record parsing from byte fixtures. The fixtures are assembled by hand from the layout documented in
// <linux/perf_event.h> (not with RecordBuilder), so the parser is checked against the kernel's format and the
// builder against the parser.
#include "capture/linux_perf/perf_records.h"
#include "capture/linux_perf/ring_buffer.h"

#include <gtest/gtest.h>

#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace tachyon::capture::perf {
namespace {

// A little-endian byte writer for hand-built fixtures.
class Bytes {
 public:
  Bytes& u8(uint8_t v) {
    m_b.push_back(v);
    return *this;
  }
  Bytes& u16(uint16_t v) { return raw(&v, 2); }
  Bytes& u32(uint32_t v) { return raw(&v, 4); }
  Bytes& u64(uint64_t v) { return raw(&v, 8); }
  Bytes& str(const std::string& s, size_t padTo) {  // NUL-padded to padTo bytes
    for (size_t i = 0; i < padTo; ++i) m_b.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
    return *this;
  }
  Bytes& append(const Bytes& other) {
    m_b.insert(m_b.end(), other.m_b.begin(), other.m_b.end());
    return *this;
  }
  Bytes& zeros(size_t n) {
    m_b.insert(m_b.end(), n, 0);
    return *this;
  }
  const std::vector<uint8_t>& get() const { return m_b; }
  size_t size() const { return m_b.size(); }

 private:
  Bytes& raw(const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    m_b.insert(m_b.end(), c, c + n);
    return *this;
  }
  std::vector<uint8_t> m_b;
};

// perf_event_header with the size patched in at the end. The record is kept alive for the whole test run:
// ParsedRecord::name and ::chain alias it, and callers pass the result straight into parseRecord().
const std::vector<uint8_t>& withHeader(uint32_t type, uint16_t misc, const Bytes& body) {
  static std::deque<std::vector<uint8_t>> keep;
  Bytes out;
  out.u32(type).u16(misc).u16(static_cast<uint16_t>(8 + body.size()));
  std::vector<uint8_t> v = out.get();
  v.insert(v.end(), body.get().begin(), body.get().end());
  keep.push_back(std::move(v));
  return keep.back();
}

const RecordLayout kLayout{kBackendSampleType, true};  // IP|TID|TIME|CALLCHAIN|CPU, sample_id_all

// sample_id trailer for kLayout: TID, TIME, CPU.
Bytes trailer(uint32_t pid, uint32_t tid, uint64_t time, uint32_t cpu) {
  Bytes t;
  t.u32(pid).u32(tid).u64(time).u32(cpu).u32(0);
  return t;
}

TEST(PerfRecords, SampleWithCallchainAndContextMarkers) {
  Bytes body;
  body.u64(0xffffffff81000010ull);  // ip
  body.u32(4242).u32(4243);         // pid, tid
  body.u64(123456789);              // time
  body.u32(3).u32(0);               // cpu, res
  body.u64(6);                      // nr
  body.u64(kContextKernel).u64(0xffffffff81000010ull).u64(0xffffffff81000020ull);
  body.u64(kContextUser).u64(0x400100).u64(0x400200);
  const std::vector<uint8_t>& rec = withHeader(kRecordSample, kMiscKernel, body);
  ASSERT_EQ(rec.size(), 8u + 8 + 8 + 8 + 8 + 8 + 48);

  ParsedRecord r;
  ASSERT_EQ(parseRecord(rec, kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.type(), kRecordSample);
  EXPECT_EQ(r.cpuMode(), kMiscKernel);
  EXPECT_EQ(r.ip, 0xffffffff81000010ull);
  EXPECT_EQ(r.pid, 4242u);
  EXPECT_EQ(r.tid, 4243u);
  EXPECT_EQ(r.time, 123456789u);
  EXPECT_TRUE(r.hasTime);
  EXPECT_EQ(r.cpu, 3u);
  ASSERT_EQ(r.chainCount, 6u);
  EXPECT_EQ(r.chainAt(0), kContextKernel);
  EXPECT_EQ(r.chainAt(1), 0xffffffff81000010ull);
  EXPECT_EQ(r.chainAt(3), kContextUser);
  EXPECT_EQ(r.chainAt(5), 0x400200u);
  EXPECT_EQ(r.chainAt(6), 0u) << "out-of-range index is 0, not a read past the record";
}

TEST(PerfRecords, ContextMarkersAreAboveKContextMax) {
  for (uint64_t marker : {kContextHypervisor, kContextKernel, kContextUser, kContextGuest,
                          kContextGuestKernel, kContextGuestUser})
    EXPECT_GE(marker, kContextMax);
  EXPECT_LT(uint64_t{0x00007fffffffffffull}, kContextMax);
  EXPECT_LT(uint64_t{0xffffffff81000000ull}, kContextMax) << "kernel text addresses are not markers";
}

TEST(PerfRecords, SampleWithEmptyCallchain) {
  Bytes body;
  body.u64(0x401000).u32(1).u32(1).u64(5).u32(0).u32(0).u64(0);
  ParsedRecord r;
  ASSERT_EQ(parseRecord(withHeader(kRecordSample, kMiscUser, body), kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.chainCount, 0u);
  EXPECT_EQ(r.ip, 0x401000u);
}

TEST(PerfRecords, Mmap2WithoutBuildId) {
  Bytes body;
  body.u32(100).u32(101);                               // pid tid
  body.u64(0x7f0000001000ull).u64(0x2000).u64(0x1000);  // addr len pgoff
  body.u32(8).u32(1).u64(777).u64(5);                   // maj min ino ino_generation
  body.u32(kProtRead | kProtExec).u32(2);               // prot flags
  body.str("/usr/lib/libfoo.so.1", 24);
  Bytes all = body;
  all.append(trailer(100, 101, 999, 2));
  ParsedRecord r;
  ASSERT_EQ(parseRecord(withHeader(kRecordMmap2, kMiscUser, all), kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.pid, 100u);
  EXPECT_EQ(r.tid, 101u);
  EXPECT_EQ(r.mapAddr, 0x7f0000001000ull);
  EXPECT_EQ(r.mapLen, 0x2000u);
  EXPECT_EQ(r.pgoff, 0x1000u);
  EXPECT_EQ(r.devMajor, 8u);
  EXPECT_EQ(r.devMinor, 1u);
  EXPECT_EQ(r.inode, 777u);
  EXPECT_EQ(r.inodeGeneration, 5u);
  EXPECT_EQ(r.buildIdSize, 0u);
  EXPECT_EQ(r.prot, kProtRead | kProtExec);
  EXPECT_EQ(r.mapFlags, 2u);
  EXPECT_EQ(r.name, "/usr/lib/libfoo.so.1")
      << "the NUL padding and the sample_id trailer are not part of the name";
  EXPECT_EQ(r.time, 999u);
  EXPECT_EQ(r.cpu, 2u);
}

TEST(PerfRecords, Mmap2WithBuildId) {
  for (uint8_t idSize : {uint8_t{20}, uint8_t{16}, uint8_t{8}, uint8_t{1}}) {
    Bytes body;
    body.u32(7).u32(7).u64(0x400000).u64(0x1000).u64(0);
    body.u8(idSize).u8(0).u16(0);
    for (uint8_t i = 0; i < 20; ++i) body.u8(i < idSize ? static_cast<uint8_t>(0xA0 + i) : 0);
    body.u32(kProtRead | kProtExec).u32(2);
    body.str("/bin/app", 16);
    Bytes all = body;
    all.append(trailer(7, 7, 50, 0));
    ParsedRecord r;
    ASSERT_EQ(parseRecord(withHeader(kRecordMmap2, static_cast<uint16_t>(kMiscUser | kMiscMmapBuildId), all),
                          kLayout, r),
              ParseStatus::Ok)
        << int(idSize);
    EXPECT_EQ(r.buildIdSize, idSize);
    for (uint8_t i = 0; i < idSize; ++i) EXPECT_EQ(r.buildId[i], 0xA0 + i);
    EXPECT_EQ(r.devMajor, 0u) << "the build-id layout has no dev / ino";
    EXPECT_EQ(r.name, "/bin/app");
  }
}

TEST(PerfRecords, Mmap2BuildIdSizeAboveTwentyIsMalformed) {
  Bytes body;
  body.u32(7).u32(7).u64(0x400000).u64(0x1000).u64(0);
  body.u8(21).u8(0).u16(0).zeros(20);
  body.u32(5).u32(2);
  body.str("/bin/app", 8);
  Bytes all = body;
  all.append(trailer(7, 7, 50, 0));
  ParsedRecord r;
  EXPECT_EQ(parseRecord(withHeader(kRecordMmap2, static_cast<uint16_t>(kMiscUser | kMiscMmapBuildId), all),
                        kLayout, r),
            ParseStatus::Malformed);
}

TEST(PerfRecords, CommWithAndWithoutExec) {
  for (bool exec : {false, true}) {
    Bytes body;
    body.u32(55).u32(56).str("worker-thread", 16);
    Bytes all = body;
    all.append(trailer(55, 56, 4000, 1));
    ParsedRecord r;
    ASSERT_EQ(parseRecord(withHeader(kRecordComm, exec ? kMiscCommExec : uint16_t{0}, all), kLayout, r),
              ParseStatus::Ok);
    EXPECT_EQ(r.pid, 55u);
    EXPECT_EQ(r.tid, 56u);
    EXPECT_EQ(r.name, "worker-thread");
    EXPECT_EQ(r.isExecComm(), exec);
    EXPECT_EQ(r.time, 4000u);
  }
}

TEST(PerfRecords, ForkAndExit) {
  for (uint32_t type : {kRecordFork, kRecordExit}) {
    Bytes body;
    body.u32(200).u32(100).u32(201).u32(101).u64(31337);  // pid ppid tid ptid time
    Bytes all = body;
    all.append(trailer(100, 101, 31337, 0));
    ParsedRecord r;
    ASSERT_EQ(parseRecord(withHeader(type, 0, all), kLayout, r), ParseStatus::Ok);
    EXPECT_EQ(r.pid, 200u) << "the body pid, not the emitting task of the trailer";
    EXPECT_EQ(r.ppid, 100u);
    EXPECT_EQ(r.tid, 201u);
    EXPECT_EQ(r.ptid, 101u);
    EXPECT_EQ(r.time, 31337u);
  }
}

TEST(PerfRecords, LostAndLostSamples) {
  Bytes lost;
  lost.u64(9).u64(1234);
  lost.append(trailer(0, 0, 77, 5));
  ParsedRecord r;
  ASSERT_EQ(parseRecord(withHeader(kRecordLost, 0, lost), kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.lost, 1234u);
  EXPECT_EQ(r.id, 9u);
  EXPECT_EQ(r.cpu, 5u);

  Bytes lostSamples;
  lostSamples.u64(55);
  lostSamples.append(trailer(0, 0, 78, 5));
  ASSERT_EQ(parseRecord(withHeader(kRecordLostSamples, 0, lostSamples), kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.lost, 55u);
}

TEST(PerfRecords, ThrottleAndUnthrottle) {
  for (uint32_t type : {kRecordThrottle, kRecordUnthrottle}) {
    Bytes body;
    body.u64(5000).u64(11).u64(12);
    body.append(trailer(0, 0, 5000, 2));
    ParsedRecord r;
    ASSERT_EQ(parseRecord(withHeader(type, 0, body), kLayout, r), ParseStatus::Ok);
    EXPECT_EQ(r.type(), type);
    EXPECT_EQ(r.time, 5000u);
    EXPECT_EQ(r.id, 11u);
    EXPECT_EQ(r.streamId, 12u);
    EXPECT_EQ(r.cpu, 2u);
  }
}

TEST(PerfRecords, SwitchCpuWideIsParsedForLaterUse) {
  Bytes body;
  body.u32(9).u32(10);
  body.append(trailer(3, 4, 600, 1));
  ParsedRecord r;
  ASSERT_EQ(parseRecord(withHeader(kRecordSwitchCpuWide, kMiscSwitchOut, body), kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.nextPrevPid, 9u);
  EXPECT_EQ(r.nextPrevTid, 10u);
  EXPECT_EQ(r.tid, 4u);
}

TEST(PerfRecords, UnknownTypesAreUnsupportedAndSkippedBySize) {
  RecordBuilder b;
  b.raw(kRecordKsymbol, 0, 24, {});
  b.raw(9999, 0, 40, {});
  RecordBuilder::Id id;
  id.pid = 5;
  id.tid = 5;
  id.time = 10;
  b.comm(5, 5, "after", false, id);
  std::vector<std::string> comms;
  int unsupported = 0;
  size_t consumed = 0;
  EXPECT_EQ(forEachRecord(std::span<const uint8_t>(b.bytes()), consumed,
                          [&](std::span<const uint8_t> rec) {
                            ParsedRecord r;
                            const ParseStatus st = parseRecord(rec, kLayout, r);
                            if (st == ParseStatus::Unsupported)
                              ++unsupported;
                            else if (st == ParseStatus::Ok)
                              comms.emplace_back(r.name);
                          }),
            ParseStatus::Ok);
  EXPECT_EQ(unsupported, 2);
  EXPECT_EQ(comms, std::vector<std::string>{"after"});
  EXPECT_EQ(consumed, b.bytes().size());
}

TEST(PerfRecords, TruncatedAndMalformedHeaders) {
  RecordHeader h;
  EXPECT_EQ(readRecordHeader(std::span<const uint8_t>(), h), ParseStatus::Truncated);
  std::vector<uint8_t> shortBuf(7, 0);
  EXPECT_EQ(readRecordHeader(shortBuf, h), ParseStatus::Truncated);

  Bytes sizeFour;
  sizeFour.u32(kRecordComm).u16(0).u16(4).zeros(8);
  EXPECT_EQ(readRecordHeader(sizeFour.get(), h), ParseStatus::Malformed) << "size below the header";
  Bytes sizeOdd;
  sizeOdd.u32(kRecordComm).u16(0).u16(20).zeros(16);
  EXPECT_EQ(readRecordHeader(sizeOdd.get(), h), ParseStatus::Malformed) << "not a multiple of 8";
  Bytes sizeBig;
  sizeBig.u32(kRecordComm).u16(0).u16(64).zeros(16);
  EXPECT_EQ(readRecordHeader(sizeBig.get(), h), ParseStatus::Truncated) << "the record runs past the buffer";
}

TEST(PerfRecords, BodiesTooSmallForTheirTypeAreMalformed) {
  ParsedRecord r;
  // FORK needs 24 bytes of body plus the trailer.
  Bytes smallFork;
  smallFork.u32(1).u32(2);
  EXPECT_EQ(parseRecord(withHeader(kRecordFork, 0, smallFork), kLayout, r), ParseStatus::Malformed);
  // SAMPLE with nr larger than the record.
  Bytes bigChain;
  bigChain.u64(1).u32(1).u32(1).u64(1).u32(0).u32(0).u64(1000).u64(0x1234);
  EXPECT_EQ(parseRecord(withHeader(kRecordSample, kMiscUser, bigChain), kLayout, r), ParseStatus::Malformed);
  // nr that would overflow nr * 8.
  Bytes overflow;
  overflow.u64(1).u32(1).u32(1).u64(1).u32(0).u32(0).u64(0x2000000000000000ull);
  EXPECT_EQ(parseRecord(withHeader(kRecordSample, kMiscUser, overflow), kLayout, r), ParseStatus::Malformed);
  // A record smaller than its own trailer.
  Bytes tiny;
  tiny.u32(1);
  EXPECT_EQ(parseRecord(withHeader(kRecordComm, 0, tiny), kLayout, r), ParseStatus::Malformed);
  // A sample cut right after the ip.
  Bytes cut;
  cut.u64(0x1000);
  EXPECT_EQ(parseRecord(withHeader(kRecordSample, kMiscUser, cut), kLayout, r), ParseStatus::Malformed);
}

TEST(PerfRecords, HeaderSizeMustMatchTheBuffer) {
  RecordBuilder b;
  RecordBuilder::Id id;
  b.comm(1, 1, "x", false, id);
  std::vector<uint8_t> bytes = b.bytes();
  bytes.resize(bytes.size() + 8);  // a longer buffer than the record
  ParsedRecord r;
  EXPECT_EQ(parseRecord(bytes, kLayout, r), ParseStatus::Malformed);
}

TEST(PerfRecords, NameWithoutNulUsesWholeAreaAndStaysInBounds) {
  Bytes body;
  body.u32(5).u32(5).str("0123456789abcdef", 16);  // 16 characters, no NUL
  Bytes all = body;
  all.append(trailer(5, 5, 1, 0));
  ParsedRecord r;
  ASSERT_EQ(parseRecord(withHeader(kRecordComm, 0, all), kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.name, "0123456789abcdef");
}

TEST(PerfRecords, LayoutWithoutSampleIdAll) {
  const RecordLayout noTrailer{kBackendSampleType, false};
  EXPECT_EQ(noTrailer.trailerSize(), 0u);
  Bytes body;
  body.u32(8).u32(9).str("plain", 8);
  ParsedRecord r;
  const std::vector<uint8_t> rec = withHeader(kRecordComm, 0, body);  // r.name aliases it
  ASSERT_EQ(parseRecord(rec, noTrailer, r), ParseStatus::Ok);
  EXPECT_EQ(r.name, "plain");
  EXPECT_FALSE(r.hasTime);
}

TEST(PerfRecords, LayoutSizes) {
  EXPECT_EQ((RecordLayout{kBackendSampleType, true}.trailerSize()), 24u);  // TID + TIME + CPU
  EXPECT_EQ((RecordLayout{kSampleTid | kSampleTime, true}.trailerSize()), 16u);
  EXPECT_EQ((RecordLayout{
                kSampleTid | kSampleTime | kSampleId | kSampleStreamId | kSampleCpu | kSampleIdentifier, true}
                 .trailerSize()),
            48u);
  EXPECT_TRUE(RecordLayout{}.supported());
  EXPECT_FALSE((RecordLayout{kBackendSampleType | kSampleRead, true}.supported()));
}

TEST(PerfRecords, UnsupportedSampleTypeIsReportedNotMisparsed) {
  RecordBuilder b;
  RecordBuilder::Id id;
  b.comm(1, 1, "x", false, id);
  ParsedRecord r;
  EXPECT_EQ(parseRecord(b.bytes(), RecordLayout{kBackendSampleType | kSampleRead, true}, r),
            ParseStatus::Unsupported);
}

TEST(PerfRecords, BuilderRoundTripsAcrossLayouts) {
  const uint64_t types[] = {kBackendSampleType, kSampleTid | kSampleTime,
                            kBackendSampleType | kSampleId | kSampleStreamId | kSamplePeriod | kSampleAddr,
                            kBackendSampleType | kSampleIdentifier};
  for (uint64_t st : types) {
    const RecordLayout layout{st, true};
    RecordBuilder b(layout);
    RecordBuilder::Sample s;
    s.ip = 0x1111;
    s.addr = 0x2222;
    s.period = 77;
    s.id = {10, 11, 123456, 4, 99, 98};
    s.chain = {kContextUser, 0x1111, 0x3333};
    b.sample(s);
    RecordBuilder::Mmap2 m;
    m.pid = 10;
    m.tid = 11;
    m.addr = 0x10000;
    m.len = 0x1000;
    m.name = "/lib/x.so";
    m.buildId = {1, 2, 3, 4};
    m.id = s.id;
    b.mmap2(m);
    b.comm(10, 11, "comm", true, s.id);
    b.fork(10, 1, 11, 1, 5, s.id);
    b.exit(10, 1, 11, 1, 6, s.id);
    b.lost(1, 50, s.id);
    b.throttle(false, 7, 1, 2, s.id);

    size_t consumed = 0;
    std::vector<ParsedRecord> out;
    ASSERT_EQ(forEachRecord(std::span<const uint8_t>(b.bytes()), consumed,
                            [&](std::span<const uint8_t> rec) {
                              ParsedRecord r;
                              ASSERT_EQ(parseRecord(rec, layout, r), ParseStatus::Ok);
                              out.push_back(r);
                            }),
              ParseStatus::Ok);
    ASSERT_EQ(out.size(), 7u);
    if ((st & kSampleIp) != 0) EXPECT_EQ(out[0].ip, 0x1111u);
    if ((st & kSampleCallchain) != 0) {
      EXPECT_EQ(out[0].chainCount, 3u);
      EXPECT_EQ(out[0].chainAt(2), 0x3333u);
    } else {
      EXPECT_EQ(out[0].chainCount, 0u);
    }
    EXPECT_EQ(out[0].pid, 10u);
    EXPECT_EQ(out[0].tid, 11u);
    if ((st & kSampleTime) != 0) EXPECT_EQ(out[0].time, 123456u);
    if ((st & kSamplePeriod) != 0) EXPECT_EQ(out[0].period, 77u);
    if ((st & kSampleAddr) != 0) EXPECT_EQ(out[0].addr, 0x2222u);
    EXPECT_EQ(out[1].name, "/lib/x.so");
    EXPECT_EQ(out[1].buildIdSize, 4u);
    EXPECT_EQ(out[2].name, "comm");
    EXPECT_TRUE(out[2].isExecComm());
    EXPECT_EQ(out[5].lost, 50u);
  }
}

// --- ring buffer ------------------------------------------------------------------------------------------

TEST(RingBuffer, CopiesAcrossTheWrapPoint) {
  // A 64-byte data area; two records written so that the second one straddles the end.
  RecordBuilder::Id id;
  id.pid = 1;
  id.tid = 1;
  id.time = 42;
  // Records of 8 header + 8 pid/tid + 8 name + 24 trailer = 48 bytes: the second one straddles the end of a
  // 64-byte ring.
  RecordBuilder a, b;
  a.comm(1, 1, "aaaa", false, id);
  b.comm(2, 2, "bbbb", false, id);
  ASSERT_EQ(a.bytes().size(), 48u);

  std::vector<uint8_t> area(64, 0xEE);
  uint64_t head = 0, tail = 0;
  RingConsumer consumer(area.data(), area.size(), &head, &tail);

  // Producer writes a at [0,48); consumer drains it.
  std::copy(a.bytes().begin(), a.bytes().end(), area.begin());
  head = 48;
  std::vector<uint8_t> out;
  ASSERT_EQ(consumer.drain(out), RingStatus::Data);
  EXPECT_EQ(out, a.bytes());
  EXPECT_EQ(tail, 48u);

  // Producer writes b at logical [48, 96): 16 bytes before the wrap, 32 after.
  for (size_t i = 0; i < 48; ++i) area[(48 + i) % 64] = b.bytes()[i];
  head = 96;
  out.clear();
  ASSERT_EQ(consumer.drain(out), RingStatus::Data);
  ASSERT_EQ(out.size(), 48u);
  EXPECT_EQ(out, b.bytes()) << "a record that wrapped around the end of the ring is contiguous again";
  ParsedRecord r;
  ASSERT_EQ(parseRecord(out, kLayout, r), ParseStatus::Ok);
  EXPECT_EQ(r.name, "bbbb");
  EXPECT_EQ(r.pid, 2u);
  EXPECT_EQ(consumer.drain(out), RingStatus::Empty);
}

TEST(RingBuffer, CountersKeepGrowingPastTheDataSize) {
  std::vector<uint8_t> area(32);
  for (size_t i = 0; i < area.size(); ++i) area[i] = static_cast<uint8_t>(i);
  std::vector<uint8_t> out;
  EXPECT_EQ(copyRingRange(area.data(), 32, 1000, 1010, out), RingStatus::Data);
  ASSERT_EQ(out.size(), 10u);
  EXPECT_EQ(out[0], 1000 % 32);
  EXPECT_EQ(out[9], (1009 % 32));
  out.clear();
  EXPECT_EQ(copyRingRange(area.data(), 32, 1000, 1000, out), RingStatus::Empty);
}

TEST(RingBuffer, OverrunAndNonsenseCountersAreRejected) {
  std::vector<uint8_t> area(32, 1);
  std::vector<uint8_t> out;
  EXPECT_EQ(copyRingRange(area.data(), 32, 0, 33, out), RingStatus::Overrun)
      << "more unread bytes than the ring holds";
  EXPECT_EQ(copyRingRange(area.data(), 32, 50, 40, out), RingStatus::Overrun) << "head behind tail";
  EXPECT_EQ(copyRingRange(area.data(), 0, 0, 8, out), RingStatus::Overrun);
  EXPECT_TRUE(out.empty());

  uint64_t head = 200, tail = 10;
  RingConsumer consumer(area.data(), 32, &head, &tail);
  EXPECT_EQ(consumer.drain(out), RingStatus::Overrun);
  EXPECT_EQ(tail, 200u) << "the consumer resynchronises at head";
  EXPECT_EQ(consumer.overruns(), 1u);
}

// --- robustness -------------------------------------------------------------------------------------------

TEST(PerfRecords, MutatedAndTruncatedStreamsNeverEscapeTheBuffer) {
  RecordBuilder::Id id;
  id.pid = 10;
  id.tid = 11;
  id.time = 123;
  RecordBuilder b;
  RecordBuilder::Sample s;
  s.ip = 0x401000;
  s.id = id;
  s.chain = {kContextKernel, 0xffffffff81000000ull, kContextUser, 0x401000, 0x401100};
  b.sample(s);
  RecordBuilder::Mmap2 m;
  m.pid = 10;
  m.tid = 11;
  m.addr = 0x400000;
  m.len = 0x1000;
  m.name = "/bin/prog";
  m.id = id;
  b.mmap2(m);
  b.comm(10, 11, "prog", true, id);
  b.fork(12, 10, 12, 11, 5, id);
  b.lost(1, 2, id);
  b.throttle(false, 5, 1, 1, id);
  const std::vector<uint8_t> good = b.bytes();

  std::mt19937 rng(12345);
  for (int iter = 0; iter < 20000; ++iter) {
    std::vector<uint8_t> buf = good;
    const int mutations = 1 + static_cast<int>(rng() % 6);
    for (int k = 0; k < mutations; ++k) buf[rng() % buf.size()] = static_cast<uint8_t>(rng());
    if (rng() % 3 == 0) buf.resize(rng() % (buf.size() + 1));
    // Under ASAN an out-of-bounds read of `buf` (heap) aborts the test.
    size_t consumed = 0;
    forEachRecord(std::span<const uint8_t>(buf), consumed, [&](std::span<const uint8_t> rec) {
      ParsedRecord r;
      if (parseRecord(rec, kLayout, r) == ParseStatus::Ok) {
        uint64_t sum = r.name.size();
        for (uint64_t i = 0; i < r.chainCount; ++i) sum += r.chainAt(i) & 1u;
        EXPECT_LE(r.chainCount * 8, rec.size());
        EXPECT_LE(sum, rec.size() + r.chainCount);
      }
    });
    EXPECT_LE(consumed, buf.size());
  }
}

}  // namespace
}  // namespace tachyon::capture::perf
