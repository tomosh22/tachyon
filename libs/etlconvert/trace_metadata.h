// Process / thread lifetimes and thread names gathered from the Process and Thread kernel events.
#pragma once

#include "etlconvert/etl_events.h"
#include "etlconvert/module_table.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tachyon::etlconvert {

struct ProcessInfo {
  uint32_t pid = 0;
  uint32_t parentPid = 0;
  std::string name;
  std::string commandLine;
  uint64_t start = 0;  // 0 = running before the trace started
  uint64_t end = kOpenEnded;
};

struct ThreadLife {
  uint32_t tid = 0;
  uint32_t pid = 0;
  uint64_t start = 0;  // 0 = running before the trace started
  uint64_t end = kOpenEnded;
  std::string name;  // latest name seen
};

struct ThreadNameEvent {
  uint64_t ts = 0;
  uint32_t tid = 0;
  std::string name;
};

class TraceMetadata {
 public:
  // Not thread-safe. Process Start/End/DCStart/DCEnd. Returns false when the payload is malformed.
  bool onProcess(const EtwEvent& event);
  // Not thread-safe. Thread Start/End/DCStart/DCEnd. Returns false when the payload is malformed.
  bool onThread(const EtwEvent& event);
  // Not thread-safe. Thread SetName (72). Returns false when the payload is malformed.
  bool onSetName(const EtwEvent& event);

  // Thread-safe for const access. The lifetime of `tid` covering `ts` (the only one when unique), or null.
  const ThreadLife* lifeOf(uint32_t tid, uint64_t ts) const;
  // Thread-safe for const access.
  const std::vector<ThreadLife>& threads() const { return m_threads; }
  // Thread-safe for const access.
  const std::unordered_map<uint32_t, ProcessInfo>& processes() const { return m_processes; }
  // Thread-safe for const access.
  const std::vector<ThreadNameEvent>& threadNames() const { return m_names; }

 private:
  std::vector<ThreadLife> m_threads;
  std::unordered_map<uint32_t, std::vector<uint32_t>> m_byTid;  // indices into m_threads
  std::unordered_map<uint32_t, ProcessInfo> m_processes;
  std::vector<ThreadNameEvent> m_names;
};

}  // namespace tachyon::etlconvert
