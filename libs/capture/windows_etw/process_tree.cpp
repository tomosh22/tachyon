#include "capture/windows_etw/process_tree.h"

#include "capture/windows_etw/glob_match.h"

#include <algorithm>

namespace tachyon::capture::etw {

std::string executablePathOf(std::string_view cl) {
  size_t i = 0;
  while (i < cl.size() && (cl[i] == ' ' || cl[i] == '\t')) ++i;
  cl.remove_prefix(i);
  if (cl.empty()) return {};
  if (cl.front() == '"') {
    const size_t end = cl.find('"', 1);
    return std::string(cl.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1));
  }
  const size_t end = cl.find_first_of(" \t");
  return std::string(cl.substr(0, end));
}

std::string baseNameOf(std::string_view path) {
  const size_t sep = path.find_last_of("/\\");
  return std::string(sep == std::string_view::npos ? path : path.substr(sep + 1));
}

bool matchesChildFilter(std::string_view filter, std::string_view imageName, std::string_view commandLine,
                        bool ci) {
  if (filter.empty()) return true;
  const std::string exePath = executablePathOf(commandLine);
  if (filter.find_first_of("/\\") != std::string_view::npos)
    return !exePath.empty() && globMatch(filter, exePath, ci);
  if (!imageName.empty() && globMatch(filter, baseNameOf(imageName), ci)) return true;
  return !exePath.empty() && globMatch(filter, baseNameOf(exePath), ci);
}

ProcessTreeTracker::ProcessTreeTracker(std::vector<uint32_t> roots, bool followChildren,
                                       std::string childFilter, bool caseInsensitive)
    : m_follow(followChildren), m_filter(std::move(childFilter)), m_ci(caseInsensitive) {
  for (uint32_t r : roots) {
    if (r == 0 || m_roots.count(r) != 0) continue;
    m_roots[r] = true;
    m_targets.push_back(r);
    m_live[r] = m_intervals.size();
    m_intervals.push_back({r, 0, ~uint64_t{0}});  // accepted from the beginning of the trace
  }
}

bool ProcessTreeTracker::accept(const ProcessStartInfo& info, uint64_t start) {
  if (!m_follow || m_live.count(info.parentPid) == 0) return false;
  if (!matchesChildFilter(m_filter, info.imageName, info.commandLine, m_ci)) {
    ++m_rejected;
    return false;
  }
  if (std::find(m_targets.begin(), m_targets.end(), info.pid) == m_targets.end())
    m_targets.push_back(info.pid);
  m_children.push_back({info.pid, info.parentPid, info.imageName});
  m_live[info.pid] = m_intervals.size();
  m_intervals.push_back({info.pid, start, ~uint64_t{0}});
  return true;
}

bool ProcessTreeTracker::onProcessStart(const ProcessStartInfo& info, uint64_t ts) {
  // A launched root is created after the session, so its own Start event is in the trace: it stays a target.
  if (m_roots.count(info.pid) != 0) return false;
  // A Start for a pid we think is alive means its End was missed (or the pid was reused): close the old one.
  if (auto it = m_live.find(info.pid); it != m_live.end()) {
    m_intervals[it->second].end = ts > 0 ? ts - 1 : 0;
    m_live.erase(it);
  }
  return accept(info, ts);
}

void ProcessTreeTracker::onProcessEnd(uint32_t pid, uint64_t ts) {
  auto it = m_live.find(pid);
  if (it == m_live.end()) return;
  m_intervals[it->second].end = ts;
  m_live.erase(it);
}

bool ProcessTreeTracker::onProcessExisting(const ProcessStartInfo& info) {
  if (m_live.count(info.pid) != 0 || m_roots.count(info.pid) != 0) return false;  // already tracked
  return accept(info, 0);
}

std::vector<etlconvert::TargetInterval> ProcessTreeTracker::intervals() const {
  return m_intervals;
}

bool ProcessTreeTracker::onEvent(const etlconvert::EtwEvent& e) {
  if (e.provider != etlconvert::guids::kProcess) return false;
  if (e.opcode != etlconvert::opcodes::kProcessStart && e.opcode != etlconvert::opcodes::kProcessEnd &&
      e.opcode != etlconvert::opcodes::kProcessDcStart)
    return false;
  const auto p = etlconvert::decodeProcess(e.payload, e.version);
  if (!p) return false;
  if (e.opcode == etlconvert::opcodes::kProcessEnd) {
    onProcessEnd(p->processId, e.timestamp);
    return false;
  }
  const ProcessStartInfo info{p->processId, p->parentId, p->imageFileName, p->commandLine};
  if (e.opcode == etlconvert::opcodes::kProcessDcStart) return onProcessExisting(info);
  return onProcessStart(info, e.timestamp);
}

}  // namespace tachyon::capture::etw
