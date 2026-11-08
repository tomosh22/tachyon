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
// pid or unmapped frames.
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
  bool isTarget(uint32_t pid) const { return m_targets.count(pid) != 0; }
  bool keepPid(uint32_t pid) const { return pid != 0 && (systemMode() || isTarget(pid)); }
  // The pid a thread belongs to (thread table, then `fallbackPid`), 0 when unknown.
  uint32_t pidOfThread(uint32_t tid, uint32_t fallbackPid) const;
  bool keepThread(uint32_t tid, uint32_t fallbackPid) const;

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
  void pruneThreads();

  LiveEtwDecoderConfig m_cfg;
  ILiveSink& m_sink;
  LiveEtwDecoderStats m_stats;
  std::vector<uint32_t> m_roots;
  std::unordered_set<uint32_t> m_targets;  // pids that are targets right now
  ProcessTreeTracker m_tracker;
  etlconvert::ModuleTable m_modules;
  std::unique_ptr<etlconvert::StackMatcher> m_matcher;
  std::unordered_map<uint32_t, Proc> m_procs;
  std::unordered_map<uint32_t, Thr> m_threads;
  // pid -> base -> the module as reported (kept to report its unload).
  std::unordered_map<uint32_t, std::unordered_map<uint64_t, LiveModule>> m_loaded;
  uint64_t m_newestTs = 0;
  std::vector<uint64_t> m_ips;  // scratch
};

}  // namespace tachyon::capture::etw
