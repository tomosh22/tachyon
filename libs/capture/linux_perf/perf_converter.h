// Spool to .tachyon conversion for the perf backend (WP-70), the counterpart of etlconvert.
//
// Pipeline (single pass over a k-way merge of the spool's per-ring streams, every record carries a timestamp
// because sample_id_all is on):
//   1. The /proc rundown records (ring kSpoolRingRundown, all stamped with the session start) are applied
//      first: processes, threads, names, the address spaces that existed when the capture began.
//   2. The remaining records are merged by timestamp. FORK / EXIT / COMM maintain the process and thread
//      tables and the target set; MMAP2 maintains one address space per process (exec replaces it, fork
//      shares it copy-on-write); SAMPLE records of kept threads become callstacks; LOST / THROTTLE are
//      tallied.
//   3. At the end the process, thread, module, thread-name and CaptureInfo chunks are written.
//
// Callchains: perf delivers [PERF_CONTEXT_KERNEL, k0, k1, ..., PERF_CONTEXT_USER, u0, u1, ...] leaf first.
// The context markers are stripped; kernel addresses stay absolute frames (module kInvalidIdx: kernel module
// symbolisation needs /proc/kallsyms and is a follow-up); user addresses are resolved against the process's
// address space at the sample time to (module, RVA) with RVA = address - ModuleRec.base, where `base` is the
// load address of the module's lowest PT_LOAD segment (the binding ELF rule; see docs/specs). Frames outside
// every mapping are absolute. The sample is flagged kSampleFlagKernelLeaf when its leaf is in the kernel.
// Frame addresses are the raw instruction pointers and return addresses, as in the ETW converter.
//
// Modules are created on demand, when a sample first lands in a mapping, one ModuleRec per
// (process, file, load bias). Their identity is the GNU build-id (from the MMAP2 record when the kernel
// provided it, else from the file's .note.gnu.build-id).
//
// Hook for WP-71: kRecordSwitch / kRecordSwitchCpuWide and tracepoint samples are parsed but ignored today;
// the dispatch in Converter::onRecord() is where they will produce ContextSwitchRec / ReadyRec.
#pragma once

#include "capture/linux_perf/spool.h"
#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::capture::perf {

struct ConvertOptions {
  // Target processes. Empty: system mode, every non-idle sample is kept.
  std::vector<uint32_t> rootPids;
  bool followChildren = false;
  std::string childFilter;  // glob over the child's command name (comm, 15 characters); empty: all children
  bool keepIdle = false;    // keep samples of the idle tasks (pid 0)
  // Circular flush: samples older than this are dropped (0: keep all). Metadata is always processed in full.
  uint64_t windowStartTs = 0;
  // Overrides the session's end time (flush of a live capture); 0: session.endTs, else the last record.
  uint64_t endTs = 0;
  std::string agentVersion;
  std::string captureOptionsJson;  // embedded under "capture_options" in CaptureInfo
  std::string rootCommandLine;     // command line of the single root (launch); empty: read /proc
  std::filesystem::path procRoot = "/proc";
  // Tests: maps the file name recorded by MMAP2 to the file to read for the module identity (null: as is;
  // a "(deleted)" file is read through procRoot/<pid>/map_files).
  std::function<std::filesystem::path(uint32_t pid, std::string_view recordedPath)> fileMapper;
  std::function<void(float)> progress;
};

struct ConvertStats {
  uint64_t recordsRead = 0;
  uint64_t samplesRead = 0;
  uint64_t samplesWritten = 0;
  uint64_t samplesDroppedNotTarget = 0;
  uint64_t samplesDroppedIdle = 0;
  uint64_t samplesDroppedWindow = 0;
  uint64_t samplesMalformed = 0;
  uint64_t recordsUnsupported = 0;
  uint64_t recordsMalformed = 0;  // malformed bodies and unusable batch tails
  uint64_t eventsLost = 0;        // sum of PERF_RECORD_LOST / LOST_SAMPLES counts
  uint64_t throttleEvents = 0;    // THROTTLE records
  uint64_t throttledNs = 0;       // total time between THROTTLE and the matching UNTHROTTLE
  uint64_t framesTotal = 0;
  uint64_t framesUnmapped = 0;  // user frames outside every file mapping (absolute)
  uint64_t framesKernel = 0;
  uint64_t processesWritten = 0;
  uint64_t threadsWritten = 0;
  uint64_t modulesWritten = 0;
  uint32_t modulesWithBuildId = 0;
  uint32_t modulesNoFile = 0;  // the mapped file could not be read (identity from the kernel or none)
  uint32_t modulesFromKernelId = 0;
  uint64_t startTs = 0, endTs = 0;
  double seconds = 0;
  std::vector<std::string> warnings;
};

// Converts a spool (the files named by `set`) to the .tachyon file `out`. Thread-safe (no shared state).
Result<ConvertStats> convertSpool(const SpoolSet& set, const std::filesystem::path& out,
                                  const ConvertOptions& options);

}  // namespace tachyon::capture::perf
