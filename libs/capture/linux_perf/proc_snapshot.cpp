#include "capture/linux_perf/proc_snapshot.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace tachyon::capture::perf {

namespace fs = std::filesystem;

namespace {

bool parseHex(std::string_view s, uint64_t& out) {
  if (s.empty() || s.size() > 16) return false;
  uint64_t v = 0;
  for (char c : s) {
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = static_cast<uint32_t>(c - 'A' + 10);
    else
      return false;
    v = (v << 4) | d;
  }
  out = v;
  return true;
}

bool parseDec(std::string_view s, uint64_t& out) {
  if (s.empty() || s.size() > 20) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const uint64_t next = v * 10 + static_cast<uint64_t>(c - '0');
    if (next < v) return false;
    v = next;
  }
  out = v;
  return true;
}

std::string readSmallFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string trimNewline(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

bool parsePidName(const std::string& name, uint32_t& pid) {
  if (name.empty() || name.size() > 10) return false;
  uint64_t v = 0;
  if (!parseDec(name, v) || v == 0 || v > 0xFFFFFFFFull) return false;
  pid = static_cast<uint32_t>(v);
  return true;
}

}  // namespace

bool parseMapsLine(std::string_view line, MapsLine& out) {
  out = MapsLine{};
  // "start-end perms offset dev inode   path"
  size_t pos = 0;
  auto token = [&](std::string_view& tok) {
    while (pos < line.size() && line[pos] == ' ') ++pos;
    const size_t begin = pos;
    while (pos < line.size() && line[pos] != ' ') ++pos;
    tok = line.substr(begin, pos - begin);
    return !tok.empty();
  };
  std::string_view range, perms, offset, dev, inode;
  if (!token(range) || !token(perms) || !token(offset) || !token(dev) || !token(inode)) return false;
  const size_t dash = range.find('-');
  if (dash == std::string_view::npos || !parseHex(range.substr(0, dash), out.start) ||
      !parseHex(range.substr(dash + 1), out.end) || out.end < out.start)
    return false;
  if (perms.size() != 4) return false;
  out.read = perms[0] == 'r';
  out.write = perms[1] == 'w';
  out.exec = perms[2] == 'x';
  out.shared = perms[3] == 's';
  if (!parseHex(offset, out.offset)) return false;
  const size_t colon = dev.find(':');
  uint64_t maj = 0, min = 0;
  if (colon == std::string_view::npos || !parseHex(dev.substr(0, colon), maj) ||
      !parseHex(dev.substr(colon + 1), min))
    return false;
  out.devMajor = static_cast<uint32_t>(maj);
  out.devMinor = static_cast<uint32_t>(min);
  if (!parseDec(inode, out.inode)) return false;
  while (pos < line.size() && line[pos] == ' ') ++pos;
  out.path = std::string(line.substr(pos));
  while (!out.path.empty() && (out.path.back() == '\n' || out.path.back() == '\r')) out.path.pop_back();
  return true;
}

bool parseProcStat(std::string_view text, ProcStat& out) {
  out = ProcStat{};
  const size_t open = text.find('(');
  const size_t close = text.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open) return false;
  out.comm = std::string(text.substr(open + 1, close - open - 1));
  std::string_view rest = text.substr(close + 1);
  size_t pos = 0;
  auto token = [&](std::string_view& tok) {
    while (pos < rest.size() && (rest[pos] == ' ' || rest[pos] == '\n')) ++pos;
    const size_t begin = pos;
    while (pos < rest.size() && rest[pos] != ' ' && rest[pos] != '\n') ++pos;
    tok = rest.substr(begin, pos - begin);
    return !tok.empty();
  };
  std::string_view state, ppid;
  if (!token(state) || !token(ppid)) return false;
  uint64_t p = 0;
  if (state.size() != 1 || !parseDec(ppid, p) || p > 0xFFFFFFFFull) return false;
  out.state = state[0];
  out.ppid = static_cast<uint32_t>(p);
  return true;
}

std::vector<uint32_t> listThreads(const fs::path& procRoot, uint32_t pid) {
  std::vector<uint32_t> tids;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(procRoot / std::to_string(pid) / "task", ec)) {
    uint32_t tid = 0;
    if (parsePidName(e.path().filename().string(), tid)) tids.push_back(tid);
  }
  std::sort(tids.begin(), tids.end());
  return tids;
}

bool processAlive(const fs::path& procRoot, uint32_t pid) {
  ProcStat st;
  if (!parseProcStat(readSmallFile(procRoot / std::to_string(pid) / "stat"), st)) return false;
  if (st.state == 'X' || st.state == 'x') return false;
  // A zombie thread-group leader whose other threads still run is a live process.
  if (st.state == 'Z') return listThreads(procRoot, pid).size() > 1;
  return true;
}

RundownResult buildRundown(const RundownOptions& options) {
  RundownResult result;
  RecordBuilder rb(options.layout);
  std::error_code ec;

  // Every process and its parent.
  std::map<uint32_t, uint32_t> parentOf;
  for (const auto& e : fs::directory_iterator(options.procRoot, ec)) {
    uint32_t pid = 0;
    if (!parsePidName(e.path().filename().string(), pid)) continue;
    ProcStat st;
    if (!parseProcStat(readSmallFile(e.path() / "stat"), st)) continue;
    parentOf[pid] = st.ppid;
  }

  std::set<uint32_t> selected;
  if (options.rootPids.empty()) {
    for (const auto& [pid, ppid] : parentOf) selected.insert(pid);
  } else {
    for (uint32_t root : options.rootPids)
      if (parentOf.count(root) != 0) selected.insert(root);
    if (options.includeDescendants) {
      bool grew = true;
      while (grew) {  // parents are not ordered by pid, so iterate to a fixed point
        grew = false;
        for (const auto& [pid, ppid] : parentOf)
          if (selected.count(ppid) != 0 && selected.insert(pid).second) grew = true;
      }
    }
  }

  for (uint32_t pid : selected) {
    const fs::path dir = options.procRoot / std::to_string(pid);
    RecordBuilder::Id id;
    id.pid = pid;
    id.tid = pid;
    id.time = options.ts;
    const uint32_t ppid = parentOf[pid];

    std::vector<uint32_t> tids = listThreads(options.procRoot, pid);
    if (tids.empty()) continue;  // vanished
    result.pids.push_back(pid);
    rb.fork(pid, ppid, pid, ppid, options.ts, id);
    for (uint32_t tid : tids) {
      RecordBuilder::Id tidId = id;
      tidId.tid = tid;
      if (tid != pid) {
        rb.fork(pid, pid, tid, pid, options.ts, tidId);
      }
      std::string comm = trimNewline(readSmallFile(dir / "task" / std::to_string(tid) / "comm"));
      if (comm.empty() && tid == pid) {
        ProcStat st;
        if (parseProcStat(readSmallFile(dir / "stat"), st)) comm = st.comm;
      }
      rb.comm(pid, tid, comm, false, tidId);
      ++result.threads;
    }

    std::ifstream maps(dir / "maps");
    std::string line;
    while (std::getline(maps, line)) {
      MapsLine m;
      if (!parseMapsLine(line, m) || !m.exec || m.path.empty() || m.path[0] != '/' || m.end <= m.start)
        continue;
      RecordBuilder::Mmap2 mm;
      mm.pid = pid;
      mm.tid = pid;
      mm.addr = m.start;
      mm.len = m.end - m.start;
      mm.pgoff = m.offset;
      mm.devMajor = m.devMajor;
      mm.devMinor = m.devMinor;
      mm.inode = m.inode;
      mm.prot = (m.read ? 1u : 0u) | (m.write ? 2u : 0u) | 4u;
      mm.flags = m.shared ? 1u : 2u;
      mm.name = m.path;
      mm.id = id;
      rb.mmap2(mm);
      ++result.mappings;
    }
  }
  result.records = rb.take();
  return result;
}

}  // namespace tachyon::capture::perf
