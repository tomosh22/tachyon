// Child-process following (WP-13, lifetime-scoped in WP-13b): decides which processes belong to a capture's
// target set, and when.
//
// Rules:
//  - The roots (the launched or attached pid) are targets from the beginning of the trace until their
//    Process End event (so an unrelated process that later reuses the pid is not a target).
//  - A Process Start event whose parent pid is a LIVE target makes the new process a target from that
//    event's timestamp, when `followChildren` is set and its image name matches `childFilter` (empty filter =
//    every child). Its Process End closes the interval.
//  - A Process DCStart (rundown) event of a process whose parent is a live target and whose image matches is
//    accepted from the beginning of the trace: it was already running when the session started. This is what
//    carries children across the segments of a circular capture. Caveat: a process whose recorded parent pid
//    was recycled by a target could be accepted wrongly; Windows does not update ParentId.
//  - The filter is a glob ('*' and '?' only) matched against the image file name and the executable named by
//    the command line (base names, case-insensitive on Windows). A filter containing a path separator is
//    matched against the command line's executable path instead.
//  - A child that does not match is not a target and is not traversed: its own children are not followed.
//  - One tracker is fed the events of every segment of a capture in order, so its accepted set survives
//    flushes.
#pragma once

#include "etlconvert/etl_converter.h"
#include "etlconvert/etl_events.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tachyon::capture::etw {

// Thread-safe: pure function. The executable named by the first token of a command line (a leading quoted
// token may contain spaces). Empty when the command line is empty.
std::string executablePathOf(std::string_view commandLine);

// Thread-safe: pure function. Final path component (after '/' or '\').
std::string baseNameOf(std::string_view path);

// Thread-safe: pure function. See the rules above. An empty filter matches everything.
bool matchesChildFilter(std::string_view filter, std::string_view imageName, std::string_view commandLine,
                        bool caseInsensitive);

struct ProcessStartInfo {
  uint32_t pid = 0;
  uint32_t parentPid = 0;
  std::string imageName;
  std::string commandLine;
};

struct FollowedChild {
  uint32_t pid = 0;
  uint32_t parentPid = 0;
  std::string imageName;
};

class ProcessTreeTracker {
 public:
  // Thread-safe: construction only. `caseInsensitive` is true for Windows semantics.
  ProcessTreeTracker(std::vector<uint32_t> roots, bool followChildren, std::string childFilter,
                     bool caseInsensitive = true);

  // Not thread-safe. Feed Process Start events in trace order. Returns true when the process was accepted.
  bool onProcessStart(const ProcessStartInfo& info, uint64_t ts);
  // Not thread-safe. Process End: closes the pid's open interval at `ts`.
  void onProcessEnd(uint32_t pid, uint64_t ts);
  // Not thread-safe. Process DCStart rundown record (see the rules). Returns true when accepted.
  bool onProcessExisting(const ProcessStartInfo& info);
  // Not thread-safe. Decodes Process Start / End / DCStart events and ignores every other event (including
  // the DCEnd rundown). Returns true when a child was accepted.
  bool onEvent(const etlconvert::EtwEvent& event);

  // Not thread-safe. Every pid ever accepted, roots first, then children in acceptance order.
  const std::vector<uint32_t>& targets() const { return m_targets; }
  // Not thread-safe. The accepted children (not the roots) with their parents.
  const std::vector<FollowedChild>& children() const { return m_children; }
  // Not thread-safe. One interval per accepted process lifetime (a reused pid can have several).
  std::vector<etlconvert::TargetInterval> intervals() const;
  // Not thread-safe. Start events seen with a target parent that the filter rejected.
  uint32_t rejectedChildren() const { return m_rejected; }

 private:
  bool accept(const ProcessStartInfo& info, uint64_t start);

  bool m_follow;
  std::string m_filter;
  bool m_ci;
  std::vector<uint32_t> m_targets;
  std::vector<FollowedChild> m_children;
  std::vector<etlconvert::TargetInterval> m_intervals;
  std::unordered_map<uint32_t, size_t> m_live;  // pid -> index of its open interval
  std::unordered_map<uint32_t, bool> m_roots;
  uint32_t m_rejected = 0;
};

}  // namespace tachyon::capture::etw
