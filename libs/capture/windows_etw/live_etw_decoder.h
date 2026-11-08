// LiveEtwDecoder (WP-65w, docs/specs/live.md section 1): turns decoded kernel events (etlconvert::EtwEvent)
// into backend-neutral ILiveSink calls. It is the live twin of etlconvert::ConversionPipeline and shares its
// parts: the payload decoders (etl_events.h), StackMatcher (StackWalk -> parent), ModuleTable (PE identity,
// device paths) and ProcessTreeTracker (child following). What differs is that nothing is written to a file
// and the sink gets RAW identities (pids, tids, addresses, module paths): the sink interns them.
//
// Platform independent and single threaded; fed from fixtures on Linux, from LiveEtwSource on Windows.
//
// What is kept (the same rules as the converter):
//   * system mode (no root pids): every sample / switch / ready of a known thread; threads are not flagged as
//     targets;
//   * process mode: samples of target threads, switches that touch a target thread, readies where either
//     side is a target thread. The target set is the roots plus, with followChildren, children accepted by
//     the ProcessTreeTracker as their Process Start events arrive.
// Modules are reported for processes that are kept (all in system mode, targets in process mode); user-mode
// frames of a kept process are resolved by the sink against them. Kernel images are not reported (kernel
// frames reach the sink as absolute addresses).
//
// Order: events should arrive roughly in timestamp order (LiveEtwSource sorts each batch). A Process, Thread
// or Image event that arrives after the sample it explains is not retroactive: that sample keeps an unknown
// pid or unmapped frames. The opposite order is handled: the metadata of a batch is decoded before its data
// (LiveEtwSource holds data back), so an END (Process End, Image Unload) can overtake the samples that
// precede it; a process stays a target until its End time and its tables and modules are released
// kEndedGraceSeconds later, so those samples are still attributed and resolved.
#pragma once

#include "capture/capture_backend.h"
#include "capture/windows_etw/process_tree.h"
#include "capture/windows_etw/rundown.h"
#include "etlconvert/device_path.h"
#include "etlconvert/etl_events.h"
#include "etlconvert/module_table.h"
#include "etlconvert/stack_matcher.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tachyon::capture::etw {

struct LiveEtwDecoderConfig {
  std::shared_ptr<ILiveSink> sink;
  uint64_t clockFrequencyHz = 10'000'000;  // the session clock (QPC)
  // Target processes. Empty: system mode.
  std::vector<uint32_t> rootPids;
  bool followChildren = false;
  std::string childFilter;
  uint64_t startTs = 0;  // trace clock at capture start: things that existed before it are stamped with it
  uint32_t stackWindowMs = 250;     // how long a parent waits for its StackWalk
  etlconvert::PeLookupFn peLookup;  // empty: read the real file
  etlconvert::DevicePathMapper deviceMapper;
};

// How long after its Process End event a process stays in the tables, and how long an Image Unload is held
// back. The samples, switches and images of a
// process's last moments reach the decoder after the End event when the buffers of the CPUs are delivered in
// another order (or the data is held back, see LiveEtwSource): they are still attributed to it.
inline constexpr uint64_t kEndedGraceSeconds = 5;

struct LiveEtwDecoderStats {
  uint64_t events = 0;
  uint64_t samples = 0;   // handed to the sink
  uint64_t switches = 0;  // handed to the sink
  uint64_t readies = 0;   // handed to the sink
  uint64_t droppedNotTarget = 0;
  uint64_t modulesLoaded = 0;
  uint64_t modulesUnloaded = 0;
  uint64_t stacksMatched = 0;
  uint64_t stacksUnmatched = 0;
  uint64_t parentsWithoutStack = 0;
  uint64_t stackKeyEvents = 0;  // compressed stacks (opcodes 34-37): not decoded
  uint64_t malformed = 0;
  uint64_t maxTs = 0;  // newest event timestamp
};

// Not thread-safe: one thread drives it.
class LiveEtwDecoder final : private etlconvert::ParentListener {
 public:
  explicit LiveEtwDecoder(LiveEtwDecoderConfig config);
  ~LiveEtwDecoder() override;
  LiveEtwDecoder(const LiveEtwDecoder&) = delete;
  LiveEtwDecoder& operator=(const LiveEtwDecoder&) = delete;

  // Not thread-safe. Decodes one kernel event. `event.payload` need only live for the call.
  void onEvent(const etlconvert::EtwEvent& event);
  // Not thread-safe. The processes, threads and modules that already existed when the session started
  // (stamped with startTs).
  void applyRundown(const RundownSnapshot& snapshot);
  // Not thread-safe. Releases every parent that is still waiting for its stack.
  void flush();

  // Not thread-safe.
  const LiveEtwDecoderStats& stats() const { return m_stats; }

 private:
  struct Proc {
    uint32_t ppid = 0;
    std::string name, commandLine;
    uint64_t startTs = 0, endTs = 0;
    bool emitted = false;
  };
  struct Thr {
    uint32_t pid = 0;
    uint64_t startTs = 0, endTs = 0;
    std::string name;
    bool emitted = false;
  };

  void onParentDone(etlconvert::PendingParent& parent) override;

  bool systemMode() const { return m_roots.empty(); }
  // `pid` is a target at trace time `ts` (0: whenever): from its start until its Process End event.
  bool isTarget(uint32_t pid, uint64_t ts = 0) const {
    const auto it = m_targets.find(pid);
    return it != m_targets.end() && (ts == 0 || ts <= it->second);
  }
  bool keepPid(uint32_t pid, uint64_t ts = 0) const {
    return pid != 0 && (systemMode() || isTarget(pid, ts));
  }
  // The pid a thread belongs to (thread table, then `fallbackPid`), 0 when unknown.
  uint32_t pidOfThread(uint32_t tid, uint32_t fallbackPid) const;
  bool keepThread(uint32_t tid, uint32_t fallbackPid, uint64_t ts = 0) const;

  void onProcessEvent(const etlconvert::EtwEvent& e);
  void onThreadEvent(const etlconvert::EtwEvent& e);
  void onSetName(const etlconvert::EtwEvent& e);
  void onImageEvent(const etlconvert::EtwEvent& e);
  void onSample(const etlconvert::EtwEvent& e);
  void onCSwitch(const etlconvert::EtwEvent& e);
  void onReady(const etlconvert::EtwEvent& e);
  void onStack(const etlconvert::EtwEvent& e);

  void emitProcess(uint32_t pid);
  void ensureProcess(uint32_t pid);
  void emitThread(uint32_t tid);
  void ensureThread(uint32_t tid, uint32_t pid);
  void loadModule(uint32_t pid, uint64_t base, uint64_t size, const std::string& path, uint32_t checksum,
                  uint32_t timeDateStamp, uint64_t loadTs);
  void unloadModule(uint32_t pid, uint64_t base, uint64_t ts);
  void unloadAllModules(uint32_t pid, uint64_t ts);
  // A process that ended is forgotten only some time later (see kEndedGraceSeconds); `all` forgets them now.
  void reapEnded(bool all);
  void reapProcess(uint32_t pid, uint64_t endTs);
  // An Image Unload is reported kUnloadGrace later, for the same reason (the samples that precede it).
  void applyPendingUnloads(uint32_t pid, uint64_t base, bool anyBase);
  void noteReapDue(uint64_t ts);
  void pruneThreads();

  LiveEtwDecoderConfig m_cfg;
  ILiveSink& m_sink;
  LiveEtwDecoderStats m_stats;
  std::vector<uint32_t> m_roots;
  // pid -> trace time of its Process End (kStillRunning while it runs): the pids that are targets.
  std::unordered_map<uint32_t, uint64_t> m_targets;
  // pid -> Process End time, for every process that ended and has not been forgotten yet.
  std::unordered_map<uint32_t, uint64_t> m_ended;
  ProcessTreeTracker m_tracker;
  etlconvert::ModuleTable m_modules;
  std::unique_ptr<etlconvert::StackMatcher> m_matcher;
  std::unordered_map<uint32_t, Proc> m_procs;
  std::unordered_map<uint32_t, Thr> m_threads;
  // pid -> base -> the module as reported (kept to report its unload).
  std::unordered_map<uint32_t, std::unordered_map<uint64_t, LiveModule>> m_loaded;
  struct PendingUnload {
    uint32_t pid = 0;
    uint64_t base = 0, ts = 0;
  };
  std::vector<PendingUnload> m_pendingUnloads;
  uint64_t m_reapDueAt = ~uint64_t{0};  // trace time after which reapEnded() has something to do
  uint64_t m_newestTs = 0;
  std::vector<uint64_t> m_ips;  // scratch
};

}  // namespace tachyon::capture::etw
