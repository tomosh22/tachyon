// Child-process following (WP-13): decides which processes belong to a capture's target set.
//
// Rules:
//  - The roots (the launched or attached pid) are targets.
//  - A Process Start event whose parent pid is a LIVE target makes the new process a target when
//    `followChildren` is set and its image name matches `childFilter` (empty filter = every child).
//  - The filter is a glob ('*' and '?' only) matched against the image file name and the executable named by
//    the command line (base names, case-insensitive on Windows). A filter containing a path separator is
//    matched against the command line's executable path instead.
//  - A child that does not match is not a target and is not traversed: its own children are not followed.
//  - Process End removes a pid from the live set, so a reused pid is evaluated afresh.
// Children that existed before the capture started (attach mode) are NOT followed: only Start events count.
#pragma once

#include "etlconvert/etl_events.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
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
  bool onProcessStart(const ProcessStartInfo& info);
  // Not thread-safe. Process End.
  void onProcessEnd(uint32_t pid);
  // Not thread-safe. Decodes Process Start / End events and ignores every other event (including the
  // DCStart / DCEnd rundown). Returns true when a child was accepted.
  bool onEvent(const etlconvert::EtwEvent& event);

  // Not thread-safe. Every pid ever accepted, roots first, then children in acceptance order.
  const std::vector<uint32_t>& targets() const { return m_targets; }
  // Not thread-safe. The accepted children (not the roots) with their parents.
  const std::vector<FollowedChild>& children() const { return m_children; }
  // Not thread-safe. Start events seen with a target parent that the filter rejected.
  uint32_t rejectedChildren() const { return m_rejected; }

 private:
  bool m_follow;
  std::string m_filter;
  bool m_ci;
  std::vector<uint32_t> m_targets;
  std::vector<FollowedChild> m_children;
  std::unordered_set<uint32_t> m_live;
  std::unordered_set<uint32_t> m_roots;
  uint32_t m_rejected = 0;
};

}  // namespace tachyon::capture::etw
