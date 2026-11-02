// Constants of the perf_event_open record ABI that the parser, the spool and the converter need (WP-70).
//
// They are copied from <linux/perf_event.h> (the ABI is stable) so that the parsing and conversion code
// compiles and is unit-tested on every platform, and so that the record layout is stated once, here. Only
// perf_system.cpp includes the kernel header (for perf_event_attr and the ioctls).
#pragma once

#include <cstddef>
#include <cstdint>

namespace tachyon::capture::perf {

// perf_event_header::type
enum RecordType : uint32_t {
  kRecordMmap = 1,
  kRecordLost = 2,
  kRecordComm = 3,
  kRecordExit = 4,
  kRecordThrottle = 5,
  kRecordUnthrottle = 6,
  kRecordFork = 7,
  kRecordRead = 8,
  kRecordSample = 9,
  kRecordMmap2 = 10,
  kRecordAux = 11,
  kRecordItraceStart = 12,
  kRecordLostSamples = 13,
  kRecordSwitch = 14,
  kRecordSwitchCpuWide = 15,
  kRecordNamespaces = 16,
  kRecordKsymbol = 17,
  kRecordBpfEvent = 18,
  kRecordCgroup = 19,
  kRecordTextPoke = 20,
  kRecordAuxOutputHwId = 21,
};

// perf_event_attr::sample_type bits.
inline constexpr uint64_t kSampleIp = 1ull << 0;
inline constexpr uint64_t kSampleTid = 1ull << 1;
inline constexpr uint64_t kSampleTime = 1ull << 2;
inline constexpr uint64_t kSampleAddr = 1ull << 3;
inline constexpr uint64_t kSampleRead = 1ull << 4;
inline constexpr uint64_t kSampleCallchain = 1ull << 5;
inline constexpr uint64_t kSampleId = 1ull << 6;
inline constexpr uint64_t kSampleCpu = 1ull << 7;
inline constexpr uint64_t kSamplePeriod = 1ull << 8;
inline constexpr uint64_t kSampleStreamId = 1ull << 9;
inline constexpr uint64_t kSampleRaw = 1ull << 10;
inline constexpr uint64_t kSampleIdentifier = 1ull << 16;

// The sample_type the backend requests (and the only one besides small variations the parser accepts for
// SAMPLE records: see kSupportedSampleTypeMask).
inline constexpr uint64_t kBackendSampleType =
    kSampleIp | kSampleTid | kSampleTime | kSampleCallchain | kSampleCpu;
// The sample_type of a capture that also records the sched tracepoints (WP-71): every event of the capture
// uses it, so one layout describes the whole ring (RAW holds the tracepoint payload; a cpu-clock sample
// carries an empty 4-byte RAW area).
inline constexpr uint64_t kSchedSampleType = kBackendSampleType | kSampleRaw;
// Bits whose record layout the parser knows. READ, branch stacks, user regs/stack, ... are not decoded.
inline constexpr uint64_t kSupportedSampleTypeMask =
    kSampleIp | kSampleTid | kSampleTime | kSampleAddr | kSampleCallchain | kSampleId | kSampleCpu |
    kSamplePeriod | kSampleStreamId | kSampleRaw | kSampleIdentifier;

// perf_event_header::misc
inline constexpr uint16_t kMiscCpuModeMask = 7;
inline constexpr uint16_t kMiscCpuModeUnknown = 0;
inline constexpr uint16_t kMiscKernel = 1;
inline constexpr uint16_t kMiscUser = 2;
inline constexpr uint16_t kMiscHypervisor = 3;
inline constexpr uint16_t kMiscGuestKernel = 4;
inline constexpr uint16_t kMiscGuestUser = 5;
inline constexpr uint16_t kMiscMmapData = 1u << 13;   // MMAP/MMAP2: a data mapping
inline constexpr uint16_t kMiscCommExec = 1u << 13;   // COMM: caused by exec
inline constexpr uint16_t kMiscSwitchOut = 1u << 13;  // SWITCH: switch out
inline constexpr uint16_t kMiscMmapBuildId = 1u
                                             << 14;  // MMAP2: the build-id layout is used instead of dev/ino
inline constexpr uint16_t kMiscSwitchOutPreempt = 1u << 14;

// Callchain context markers (values >= kContextMax, as unsigned 64-bit, are markers, not addresses).
inline constexpr uint64_t kContextHypervisor = static_cast<uint64_t>(-32);
inline constexpr uint64_t kContextKernel = static_cast<uint64_t>(-128);
inline constexpr uint64_t kContextUser = static_cast<uint64_t>(-512);
inline constexpr uint64_t kContextGuest = static_cast<uint64_t>(-2048);
inline constexpr uint64_t kContextGuestKernel = static_cast<uint64_t>(-2176);
inline constexpr uint64_t kContextGuestUser = static_cast<uint64_t>(-2560);
inline constexpr uint64_t kContextMax = static_cast<uint64_t>(-4095);

// mmap prot bits as recorded in MMAP2.
inline constexpr uint32_t kProtRead = 1;
inline constexpr uint32_t kProtWrite = 2;
inline constexpr uint32_t kProtExec = 4;

inline constexpr size_t kRecordHeaderSize = 8;
inline constexpr size_t kMaxRecordSize = 65535;  // perf_event_header::size is 16 bits

}  // namespace tachyon::capture::perf
