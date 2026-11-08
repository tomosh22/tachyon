#include "capture/linux_perf/live_source.h"

#include "capture/windows_etw/glob_match.h"
#include "core/log.h"
#include "symbols/dwarf/elf_reader.h"
#include "trace/trace_format.h"

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace tachyon::capture::perf {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kNoFile = 0xFFFFFFFFu;
constexpr uint32_t kBadTid = 0xFFFFFFFFu;  // the kernel's (pid_t)-1: a task that is no longer pid_alive
constexpr int kMaxAncestors = 64;
constexpr size_t kMaxForkSeen = 8192;
constexpr size_t kMaxDeadThreads = 100000;
constexpr uint64_t kKernelBase = 0xFFFF800000000000ull;

uint64_t alignDown(uint64_t v) {
  return v & ~(kPageSize - 1);
}
uint64_t alignUp(uint64_t v) {
  return (v + kPageSize - 1) & ~(kPageSize - 1);
}
bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
bool isPseudoPath(std::string_view p) {
  return p.empty() || p[0] == '[' || p.rfind("//anon", 0) == 0 || p.rfind("/memfd:", 0) == 0 ||
         p.rfind("/dev/", 0) == 0 || p.rfind("/SYSV", 0) == 0 || p[0] != '/';
}

struct FileInfo {
  bool loaded = false;
  bool readable = false;
  bool isElf = false;
  std::vector<symbols::ElfLoadSegment> segs;
  uint64_t lowestAligned = 0;
  uint64_t imageSize = 0;
  std::vector<uint8_t> buildId;
};

struct FileEntry {
  std::string path;
  std::vector<uint8_t> kernelBuildId;
  FileInfo info;
};

struct Mapping {
  uint64_t start = 0, end = 0, pgoff = 0;
  uint32_t prot = 0;
  uint32_t file = kNoFile;
  uint64_t mapTs = 0;
};

struct AddressSpace {
  std::map<uint64_t, Mapping> maps;

  void insert(const Mapping& m) {
    if (m.end <= m.start) return;
    auto it = maps.lower_bound(m.start);
    if (it != maps.begin()) {
      auto prev = std::prev(it);
      if (prev->second.end > m.start) it = prev;
    }
    std::vector<Mapping> keep;
    while (it != maps.end() && it->second.start < m.end) {
      const Mapping& old = it->second;
      if (old.start < m.start) {
        Mapping left = old;
        left.end = m.start;
        keep.push_back(left);
      }
      if (old.end > m.end) {
        Mapping right = old;
        right.pgoff += m.end - old.start;
        right.start = m.end;
        keep.push_back(right);
      }
      it = maps.erase(it);
    }
    for (const Mapping& k : keep) maps.emplace(k.start, k);
    maps.emplace(m.start, m);
  }
};

struct Proc {
  uint32_t pid = 0, ppid = 0;
  std::string comm;
  uint64_t startTs = 0, endTs = 0;
  bool preexisting = false;
  bool fromRundown = false;
  bool exited = false;
  bool isRoot = false;
  bool emitted = false;
  bool modulesReported = false;  // the modules of its address space were reported (a sampled process)
  uint32_t liveThreads = 0;
  std::shared_ptr<AddressSpace> as;
  uint64_t memoEpoch = ~0ull;
  bool memoTarget = false;
  // Modules reported to the sink: (file, bias) -> the reported event, kept to report the unload.
  std::map<std::pair<uint32_t, uint64_t>, LiveModule> modules;
};

struct Thr {
  uint32_t tid = 0, pid = 0;
  uint64_t startTs = 0, endTs = 0;
  bool preexisting = false, fromRundown = false, inferred = false, exited = false, emitted = false;
  std::string name;
};

}  // namespace

struct LiveDecoder::Impl {
  LiveDecoderConfig cfg;
  LiveDecoderStats stats;
  ILiveSink& sink;
  std::set<uint32_t> roots;
  uint64_t startTs = 1;
  uint64_t epoch = 0;
  bool inRundown = false;

  std::unordered_map<uint32_t, Proc> procs;
  std::unordered_map<uint32_t, Thr> threads;  // by tid; exited ones stay until the tid is reused
  std::vector<FileEntry> files;
  std::unordered_map<std::string, uint32_t> fileIndex;
  std::set<std::tuple<uint32_t, uint32_t, uint64_t>> forkSeen;
  std::deque<std::tuple<uint32_t, uint32_t, uint64_t>> forkOrder;

  bool schedTracepoints = false, schedSwitchRecords = false;
  std::optional<SchedSwitchDecoder> switchDecoder;
  std::optional<SchedWakeDecoder> wakeDecoder;
  struct LastSwitchOut {
    uint32_t oldTid = 0, newTid = 0;
    bool valid = false;
  };
  std::unordered_map<uint32_t, LastSwitchOut> lastSwitchOut;
  std::unordered_map<uint32_t, uint32_t> cpuRunning;
  std::vector<uint64_t> ips;

  explicit Impl(LiveDecoderConfig c) : cfg(std::move(c)), sink(*cfg.sink) {
    roots.insert(cfg.rootPids.begin(), cfg.rootPids.end());
    startTs = cfg.startTs != 0 ? cfg.startTs : 1;
    if (cfg.sched.mode == "tracepoint") {
      schedTracepoints = true;
      if (cfg.sched.switches) {
        auto fmt = parseTracepointFormat(cfg.sched.switchFormat);
        if (fmt.ok()) {
          auto dec = SchedSwitchDecoder::create(*fmt);
          if (dec.ok()) switchDecoder.emplace(*dec);
        }
      }
      if (cfg.sched.wakeups) {
        auto fmt = parseTracepointFormat(cfg.sched.wakeFormat);
        if (fmt.ok()) {
          auto dec = SchedWakeDecoder::create(*fmt);
          if (dec.ok()) wakeDecoder.emplace(*dec);
        }
      }
    } else if (cfg.sched.mode == "context_switch") {
      schedSwitchRecords = true;
    }
  }

  // ---- tables ----

  Proc& createProc(uint32_t pid, uint32_t ppid, uint64_t ts, bool preexisting) {
    if (auto old = procs.find(pid); old != procs.end()) {
      retireProc(old->second, ts);
      procs.erase(old);
    }
    Proc p;
    p.pid = pid;
    p.ppid = ppid;
    p.startTs = preexisting ? startTs : ts;
    p.preexisting = preexisting;
    p.as = std::make_shared<AddressSpace>();
    p.isRoot = roots.count(pid) != 0;
    ++epoch;
    return procs.emplace(pid, std::move(p)).first->second;
  }

  Proc& ensureProc(uint32_t pid, uint64_t ts) {
    auto it = procs.find(pid);
    if (it != procs.end()) return it->second;
    return createProc(pid, 0, ts, true);
  }

  // Reports the modules of a process as unloaded (it exited, was replaced by a new image, or its pid is
  // reused).
  void unloadModules(Proc& p, uint64_t ts) {
    if (p.emitted) {
      for (auto& [key, m] : p.modules) {
        m.unloadTs = std::max<uint64_t>(ts, 1);
        sink.onModule(m);
      }
    }
    p.modules.clear();
  }

  void retireProc(Proc& p, uint64_t ts) {
    unloadModules(p, ts);
    if (p.emitted && p.endTs == 0) {
      p.endTs = ts;
      emitProcess(p);
    }
  }

  bool isTarget(Proc& p) {
    if (p.memoEpoch == epoch) return p.memoTarget;
    bool result = false;
    const Proc* cur = &p;
    for (int depth = 0; depth < kMaxAncestors && cur != nullptr; ++depth) {
      if (cur->isRoot) {
        result = true;
        break;
      }
      if (!cfg.followChildren) break;
      if (!cfg.childFilter.empty() && !etw::globMatch(cfg.childFilter, cur->comm, false)) break;
      auto it = procs.find(cur->ppid);
      cur = (cur->ppid == 0 || it == procs.end() || &it->second == cur) ? nullptr : &it->second;
    }
    p.memoEpoch = epoch;
    p.memoTarget = result;
    return result;
  }
  bool keep(Proc& p) { return roots.empty() || isTarget(p); }

  Thr* findThread(uint32_t tid) {
    auto it = threads.find(tid);
    return it == threads.end() ? nullptr : &it->second;
  }
  uint32_t pidOfTid(uint32_t tid) {
    const Thr* t = findThread(tid);
    return t != nullptr ? t->pid : 0;
  }
  bool tidKept(uint32_t tid) {
    if (roots.empty()) return true;
    Thr* t = findThread(tid);
    if (t == nullptr) return false;
    auto it = procs.find(t->pid);
    return it != procs.end() && isTarget(it->second);
  }

  Thr& ensureThread(uint32_t tid, Proc& proc, uint64_t ts) {
    auto it = threads.find(tid);
    if (it != threads.end() && it->second.pid == proc.pid) {
      if (it->second.exited && ts > it->second.endTs) it->second.endTs = ts;  // a late event of a dying task
      return it->second;
    }
    if (threads.size() > kMaxDeadThreads) {
      for (auto d = threads.begin(); d != threads.end();)
        d = d->second.exited ? threads.erase(d) : std::next(d);
    }
    Thr t;
    t.tid = tid;
    t.pid = proc.pid;
    t.preexisting = true;
    t.inferred = true;
    t.startTs = startTs;
    t.name = proc.comm;
    ++proc.liveThreads;
    return threads.insert_or_assign(tid, std::move(t)).first->second;
  }

  // ---- reporting ----

  void emitProcess(Proc& p) {
    LiveProcess lp;
    lp.pid = p.pid;
    lp.ppid = p.ppid;
    lp.startTs = p.preexisting ? startTs : p.startTs;
    lp.endTs = p.endTs;
    lp.name = p.comm;
    lp.commandLine = commandLineOf(p);
    sink.onProcess(lp);
  }

  std::string commandLineOf(const Proc& p) const {
    if (p.isRoot && !cfg.rootCommandLine.empty()) return cfg.rootCommandLine;
    if (p.exited) return {};
    std::ifstream in(cfg.procRoot / std::to_string(p.pid) / "cmdline", std::ios::binary);
    if (!in) return {};
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (char& c : raw)
      if (c == '\0') c = ' ';
    while (!raw.empty() && raw.back() == ' ') raw.pop_back();
    return raw;
  }

  void emitThread(Thr& t, Proc& p) {
    LiveThread lt;
    lt.tid = t.tid;
    lt.pid = t.pid;
    lt.startTs = t.preexisting ? startTs : t.startTs;
    lt.endTs = t.exited ? t.endTs : 0;
    lt.name = t.name;
    lt.isTarget = !roots.empty() && isTarget(p);
    sink.onThread(lt);
    t.emitted = true;
  }

  // Makes the sink know the process and the thread before the first event that names them. A process that is
  // sampled (`withModules`) also gets its modules reported; one that only shows up in switches or wakeups
  // does not (in system mode that would be every process of the machine).
  void announce(Proc& p, Thr& t, bool withModules) {
    if (!p.emitted) {
      p.emitted = true;
      emitProcess(p);
    }
    if (withModules && !p.modulesReported) {
      p.modulesReported = true;
      for (const auto& [start, m] : p.as->maps)
        if (m.file != kNoFile) reportModule(p, m);
    }
    if (!t.emitted) emitThread(t, p);
  }

  // Announces the thread `tid` (and its process) when it is known and kept: switches and wakeups name threads
  // that may not have been sampled yet, and their names should reach the sink with the first event.
  void announceTid(uint32_t tid) {
    if (tid == 0) return;
    Thr* t = findThread(tid);
    if (t == nullptr || t->emitted) return;
    auto pit = procs.find(t->pid);
    if (pit == procs.end() || !keep(pit->second)) return;
    announce(pit->second, *t, !roots.empty());
  }

  // ---- files and modules ----

  uint32_t fileFor(const ParsedRecord& r) {
    std::string key(r.name);
    key.push_back('\0');
    key += std::to_string(r.devMajor) + ":" + std::to_string(r.devMinor) + ":" + std::to_string(r.inode);
    auto it = fileIndex.find(key);
    if (it != fileIndex.end()) {
      FileEntry& fe = files[it->second];
      if (fe.kernelBuildId.empty() && r.buildIdSize > 0)
        fe.kernelBuildId.assign(r.buildId, r.buildId + r.buildIdSize);
      return it->second;
    }
    FileEntry fe;
    fe.path = std::string(r.name);
    if (r.buildIdSize > 0) fe.kernelBuildId.assign(r.buildId, r.buildId + r.buildIdSize);
    files.push_back(std::move(fe));
    const auto idx = static_cast<uint32_t>(files.size() - 1);
    fileIndex.emplace(std::move(key), idx);
    return idx;
  }

  void loadFile(FileEntry& fe, uint32_t pid, const Mapping& m) {
    fe.info.loaded = true;
    fs::path path = fe.path;
    if (cfg.fileMapper) path = cfg.fileMapper(pid, fe.path);
    if (endsWith(fe.path, " (deleted)") && !cfg.fileMapper) {
      char range[40];
      std::snprintf(range, sizeof(range), "%lx-%lx", static_cast<unsigned long>(m.start),
                    static_cast<unsigned long>(m.end));
      path = cfg.procRoot / std::to_string(pid) / "map_files" / range;
    }
    auto elf = symbols::ElfFile::open(path);
    if (!elf.ok()) {
      fe.info.readable = elf.error().code != ErrorCode::NotFound && elf.error().code != ErrorCode::IoError;
      return;
    }
    fe.info.readable = true;
    fe.info.isElf = true;
    fe.info.segs = elf->loadSegments();
    if (!fe.info.segs.empty()) {
      uint64_t lowest = fe.info.segs.front().vaddr, highest = 0;
      for (const auto& s : fe.info.segs) {
        lowest = std::min(lowest, s.vaddr);
        highest = std::max(highest, s.vaddr + s.memsz);
      }
      fe.info.lowestAligned = alignDown(lowest);
      fe.info.imageSize = highest > fe.info.lowestAligned ? highest - fe.info.lowestAligned : 0;
    }
    if (auto id = elf->buildId()) fe.info.buildId = std::move(*id);
  }

  static uint64_t biasOf(const FileEntry& fe, const Mapping& m) {
    if (fe.info.isElf) {
      for (const auto& s : fe.info.segs) {
        const uint64_t segOff = alignDown(s.offset);
        const uint64_t segEnd = alignUp(s.offset + s.filesz);
        if (m.pgoff >= segOff && m.pgoff < segEnd) return m.start - (alignDown(s.vaddr) + (m.pgoff - segOff));
      }
      if (!fe.info.segs.empty()) {
        const auto& s = fe.info.segs.front();
        return m.start - (m.pgoff - alignDown(s.offset) + alignDown(s.vaddr));
      }
    }
    return m.start - m.pgoff;
  }

  // Reports the module behind an executable file mapping of `p` (once per (file, bias)).
  void reportModule(Proc& p, const Mapping& m) {
    FileEntry& fe = files[m.file];
    if (!fe.info.loaded) loadFile(fe, p.pid, m);
    if (fe.info.readable && !fe.info.isElf) return;  // a mapped data file, a JIT dump
    const uint64_t bias = biasOf(fe, m);
    const std::pair<uint32_t, uint64_t> key{m.file, bias};
    if (p.modules.count(key) != 0) return;
    LiveModule lm;
    lm.pid = p.pid;
    lm.base = bias + (fe.info.isElf ? fe.info.lowestAligned : 0);
    lm.size =
        fe.info.isElf && fe.info.imageSize != 0 ? fe.info.imageSize : (m.end > lm.base ? m.end - lm.base : 0);
    lm.path = fe.path;
    const std::vector<uint8_t>* id = !fe.kernelBuildId.empty()
                                         ? &fe.kernelBuildId
                                         : (fe.info.buildId.empty() ? nullptr : &fe.info.buildId);
    if (id != nullptr && id->size() <= lm.id.size()) {
      lm.idKind = trace::kModuleIdGnuBuildId;
      lm.idLen = static_cast<uint8_t>(id->size());
      std::memcpy(lm.id.data(), id->data(), id->size());
    }
    lm.loadTs = m.mapTs;
    p.modules.emplace(key, lm);
    ++stats.modules;
    sink.onModule(lm);
  }

  // ---- records ----

  void onFork(const ParsedRecord& r, bool rundown) {
    const uint64_t ts = r.time;
    if (!rundown) {
      const auto key = std::make_tuple(r.pid, r.tid, ts);
      if (!forkSeen.insert(key).second) return;  // the same FORK reaches the stream once per ring
      forkOrder.push_back(key);
      if (forkOrder.size() > kMaxForkSeen) {
        forkSeen.erase(forkOrder.front());
        forkOrder.pop_front();
      }
    }
    constexpr uint64_t kDuplicateWindowNs = 1'000'000;
    if (r.pid == r.tid) {  // a new process
      auto old = procs.find(r.pid);
      if (old != procs.end() && !old->second.exited) {
        Proc& o = old->second;
        if (rundown) return;
        if (o.fromRundown && o.ppid == r.ppid) {
          o.fromRundown = false;
          return;
        }
        if (!o.preexisting && o.ppid == r.ppid && ts >= o.startTs && ts - o.startTs < kDuplicateWindowNs)
          return;
      }
      auto parent = procs.find(r.ppid);
      std::shared_ptr<AddressSpace> parentAs;
      std::string parentComm;
      if (parent != procs.end() && !rundown) {
        parentAs = parent->second.as;
        parentComm = parent->second.comm;
      }
      Proc& p = createProc(r.pid, r.ppid, ts, rundown || ts <= startTs);
      p.fromRundown = rundown;
      if (parentAs) {
        p.comm = parentComm;
        p.as = parentAs;  // copy-on-write
      }
      Thr t;
      t.tid = r.tid;
      t.pid = r.pid;
      t.preexisting = p.preexisting;
      t.fromRundown = rundown;
      t.startTs = p.preexisting ? startTs : ts;
      t.name = p.comm;
      threads.insert_or_assign(r.tid, std::move(t));
      p.liveThreads = 1;
      return;
    }
    Proc& proc = ensureProc(r.pid, ts);
    auto stale = threads.find(r.tid);
    if (stale != threads.end()) {
      Thr& s = stale->second;
      if (s.fromRundown && s.pid == proc.pid && !s.exited && !rundown) {
        s.fromRundown = false;
        return;
      }
      if (rundown && !s.exited && s.pid == proc.pid) return;
      if (!s.exited && !s.inferred && !s.fromRundown && s.pid == proc.pid && !rundown && ts >= s.startTs &&
          ts - s.startTs < kDuplicateWindowNs)
        return;
      if (s.inferred && s.pid == proc.pid && !s.exited && !rundown) {
        s.inferred = false;
        s.preexisting = false;
        s.startTs = ts;
        return;
      }
      if (!s.exited) exitThread(s, ts);
    }
    Thr t;
    t.tid = r.tid;
    t.pid = proc.pid;
    t.preexisting = rundown || ts <= startTs;
    t.fromRundown = rundown;
    t.startTs = t.preexisting ? startTs : ts;
    t.name = proc.comm;
    ++proc.liveThreads;
    threads.insert_or_assign(r.tid, std::move(t));
  }

  void exitThread(Thr& t, uint64_t ts) {
    t.exited = true;
    t.endTs = ts;
    auto pit = procs.find(t.pid);
    if (t.emitted && pit != procs.end()) emitThread(t, pit->second);
    if (pit == procs.end()) return;
    Proc& p = pit->second;
    if (p.liveThreads > 0) --p.liveThreads;
    if (p.liveThreads == 0 && !p.exited) {
      p.exited = true;
      p.endTs = ts;
      unloadModules(p, ts);
      if (p.emitted) emitProcess(p);
    }
    ++epoch;
  }

  void onExit(const ParsedRecord& r) {
    Thr* t = findThread(r.tid);
    if (t == nullptr || t->exited) return;
    exitThread(*t, r.time);
  }

  void onComm(const ParsedRecord& r) {
    Proc& p = ensureProc(r.pid, r.time);
    Thr& t = ensureThread(r.tid, p, r.time);
    const std::string name(r.name);
    const bool changed = t.name != name;
    t.name = name;
    const bool exec = r.isExecComm();
    bool procChanged = false;
    if (r.tid == r.pid || exec) {
      procChanged = p.comm != name;
      p.comm = name;
    }
    if (exec && !inRundown) {
      unloadModules(p, r.time);  // a new program image: the old mappings are gone
      p.as = std::make_shared<AddressSpace>();
    }
    ++epoch;
    if (changed && t.emitted) emitThread(t, p);
    if (procChanged && p.emitted) emitProcess(p);
  }

  void onMmap(const ParsedRecord& r) {
    if (r.mapLen == 0) return;
    const bool isMmap2 = r.type() == kRecordMmap2;
    const bool exec = isMmap2 ? (r.prot & kProtExec) != 0 : (r.misc() & kMiscMmapData) == 0;
    if (!exec) return;
    Proc& p = ensureProc(r.pid, r.time);
    if (p.as.use_count() > 1) p.as = std::make_shared<AddressSpace>(*p.as);  // copy on write
    Mapping m;
    m.start = r.mapAddr;
    m.end = r.mapAddr + r.mapLen;
    if (m.end < m.start) return;
    m.pgoff = r.pgoff;
    m.prot = r.prot;
    m.mapTs = inRundown ? startTs : r.time;
    m.file = isPseudoPath(r.name) ? kNoFile : fileFor(r);
    p.as->insert(m);
    if (p.modulesReported && m.file != kNoFile) reportModule(p, m);
  }

  // The addresses of a sample's callchain, leaf first (see perf_converter.cpp resolveStack); `userToo` false
  // keeps only the kernel frames.
  void buildIps(const ParsedRecord& r, bool userToo, uint16_t& flags) {
    ips.clear();
    bool kernelSection = r.cpuMode() == kMiscKernel;
    bool leafKernel = false, first = true;
    uint64_t firstAddr = 0;
    for (uint64_t i = 0; i < r.chainCount; ++i) {
      const uint64_t v = r.chainAt(i);
      if (v >= kContextMax) {
        kernelSection = v == kContextKernel || v == kContextGuestKernel;
        continue;
      }
      if (!kernelSection && !userToo) continue;
      if (first) {
        first = false;
        leafKernel = kernelSection;
        firstAddr = v;
      }
      ips.push_back(v);
    }
    const bool kernelMode = r.cpuMode() == kMiscKernel;
    if (r.ip != 0 && (ips.empty() || firstAddr != r.ip)) {
      const bool kernelIp = kernelMode || r.ip >= kKernelBase;
      if (kernelIp || userToo) {
        ips.insert(ips.begin(), r.ip);
        leafKernel = kernelIp;
      }
    }
    flags = leafKernel ? trace::kSampleFlagKernelLeaf : uint16_t{0};
  }

  void onSample(const ParsedRecord& r) {
    if (r.rawSize > 4) {
      onSchedSample(r);
      return;
    }
    if (r.pid == 0) return;  // idle
    if (r.pid == kBadTid || r.tid == kBadTid) {
      ++stats.malformed;
      return;
    }
    Proc& p = ensureProc(r.pid, r.time);
    if (!keep(p)) {
      ++stats.droppedNotTarget;
      return;
    }
    Thr& t = ensureThread(r.tid, p, r.time);
    announce(p, t, true);
    uint16_t flags = 0;
    buildIps(r, true, flags);
    sink.onSample(r.time, r.pid, r.tid, std::min<uint32_t>(r.cpu, 0xFFFF), flags, ips);
    ++stats.samples;
  }

  void onSchedSample(const ParsedRecord& r) {
    const std::span<const uint8_t> raw(r.raw, r.rawSize);
    const uint32_t type = tracepointCommonType(raw);
    if (switchDecoder && type == switchDecoder->id()) {
      SchedSwitchEvent ev;
      if (!switchDecoder->decode(raw, ev)) {
        ++stats.malformed;
        return;
      }
      onSchedSwitch(r, ev);
    } else if (wakeDecoder && type == wakeDecoder->id()) {
      SchedWakeEvent ev;
      if (!wakeDecoder->decode(raw, ev)) {
        ++stats.malformed;
        return;
      }
      onSchedWake(r, ev);
    } else {
      ++stats.unsupported;
    }
  }

  void onSchedSwitch(const ParsedRecord& r, const SchedSwitchEvent& ev) {
    if (ev.prevPid == ev.nextPid) return;
    const bool sampleNamed = r.pid != kBadTid && r.tid != kBadTid;
    Proc* stackProc = nullptr;
    uint32_t stackPid = 0;
    if (sampleNamed && r.pid != 0) {
      Proc& p = ensureProc(r.pid, r.time);
      if (keep(p)) {
        stackProc = &p;
        announce(p, ensureThread(r.tid, p, r.time), !roots.empty());
      }
    } else if (!sampleNamed && ev.prevPid != 0) {
      Thr* t = findThread(ev.prevPid);
      auto pit = t != nullptr ? procs.find(t->pid) : procs.end();
      if (pit != procs.end() && keep(pit->second)) stackProc = &pit->second;
    }
    if (stackProc != nullptr) stackPid = stackProc->pid;
    const bool sampleIsPrev = sampleNamed ? (ev.prevPid == r.tid && r.pid != 0) : ev.prevPid != 0;
    const bool oldKept =
        sampleIsPrev ? (stackProc != nullptr || (!sampleNamed && tidKept(ev.prevPid))) : tidKept(ev.prevPid);
    const bool newKept = tidKept(ev.nextPid);
    if (!oldKept && !newKept) {
      ++stats.droppedNotTarget;
      return;
    }
    if (oldKept) announceTid(ev.prevPid);
    if (newKept) announceTid(ev.nextPid);
    LiveContextSwitch cs;
    cs.ts = r.time;
    cs.cpu = r.cpu;
    cs.oldTid = ev.prevPid;
    cs.oldPid = stackPid != 0 ? stackPid : pidOfTid(ev.prevPid);
    cs.newTid = ev.nextPid;
    cs.newPid = pidOfTid(ev.nextPid);
    const SwitchStateEncoding enc = encodeSwitchState(ev.prevState);
    cs.oldState = enc.oldState;
    cs.oldWaitReason = enc.oldWaitReason;
    cs.newPriority = static_cast<uint8_t>(std::clamp<int32_t>(ev.nextPrio, 0, 255));
    if (oldKept && ev.prevPid != 0 && stackProc != nullptr) {
      uint16_t flags = 0;
      buildIps(r, true, flags);
      cs.oldStack = ips;
      if (sampleNamed && r.tid != ev.prevPid) cs.flags |= trace::kSwitchFlagStackFromOtherThread;
    }
    sink.onContextSwitch(cs);
    ++stats.switches;
  }

  void onSchedWake(const ParsedRecord& r, const SchedWakeEvent& ev) {
    if (!ev.success || ev.pid == r.tid) return;
    const bool wakerNamed = r.pid != kBadTid && r.tid != kBadTid;
    if (!tidKept(ev.pid)) {
      ++stats.droppedNotTarget;
      return;
    }
    Proc* wakerProc = nullptr;
    if (wakerNamed && r.pid != 0) {
      Proc& p = ensureProc(r.pid, r.time);
      if (keep(p)) {
        wakerProc = &p;
        announce(p, ensureThread(r.tid, p, r.time), !roots.empty());
      }
    }
    announceTid(ev.pid);
    LiveReady rd;
    rd.ts = r.time;
    rd.cpu = r.cpu;
    rd.readiedTid = ev.pid;
    rd.readiedPid = pidOfTid(ev.pid);
    rd.readyingTid = wakerNamed ? r.tid : 0;
    rd.readyingPid = wakerProc != nullptr ? wakerProc->pid : 0;
    uint16_t flags = 0;
    buildIps(r, wakerProc != nullptr, flags);
    rd.readyingStack = ips;
    sink.onReady(rd);
    ++stats.readies;
  }

  // PERF_RECORD_SWITCH_CPU_WIDE / PERF_RECORD_SWITCH fallback; see perf_converter.cpp onSwitchRecord.
  void onSwitchRecord(const ParsedRecord& r) {
    if (!schedSwitchRecords) return;
    const bool out = (r.misc() & kMiscSwitchOut) != 0;
    uint32_t oldTid = 0, newTid = 0;
    if (r.type() == kRecordSwitchCpuWide) {
      LastSwitchOut& last = lastSwitchOut[r.cpu];
      if (out) {
        oldTid = r.tid;
        if (oldTid == kBadTid) {
          auto running = cpuRunning.find(r.cpu);
          if (running == cpuRunning.end()) return;
          oldTid = running->second;
        }
        newTid = r.nextPrevTid;
        last = {oldTid, newTid, true};
      } else {
        oldTid = r.nextPrevTid;
        newTid = r.tid;
        const bool repeats = last.valid && last.oldTid == oldTid && last.newTid == newTid;
        last.valid = false;
        if (repeats) return;
      }
    } else if (out) {
      oldTid = r.tid;
    } else {
      newTid = r.tid;
    }
    if (oldTid == kBadTid || newTid == kBadTid) return;
    if (r.type() == kRecordSwitchCpuWide && newTid != 0)
      cpuRunning[r.cpu] = newTid;
    else if (r.type() == kRecordSwitchCpuWide)
      cpuRunning.erase(r.cpu);
    if (oldTid == newTid) return;
    if (r.pid != 0 && r.pid != kBadTid && r.tid != kBadTid) {
      Proc& p = ensureProc(r.pid, r.time);
      if (keep(p)) announce(p, ensureThread(r.tid, p, r.time), !roots.empty());
    }
    const bool oldKept = oldTid != 0 && tidKept(oldTid);
    const bool newKept = newTid != 0 && tidKept(newTid);
    if (!oldKept && !newKept) {
      ++stats.droppedNotTarget;
      return;
    }
    if (oldKept) announceTid(oldTid);
    if (newKept) announceTid(newTid);
    LiveContextSwitch cs;
    cs.ts = r.time;
    cs.cpu = r.cpu;
    cs.oldTid = oldTid;
    cs.oldPid = pidOfTid(oldTid);
    cs.newTid = newTid;
    cs.newPid = pidOfTid(newTid);
    cs.oldState = out && (r.misc() & kMiscSwitchOutPreempt) != 0 ? uint8_t{1} : uint8_t{5};
    sink.onContextSwitch(cs);
    ++stats.switches;
  }

  void onRecord(const ParsedRecord& r) {
    ++stats.records;
    if (r.hasTime) stats.maxTs = std::max(stats.maxTs, r.time);
    switch (r.type()) {
      case kRecordSample: onSample(r); break;
      case kRecordFork: onFork(r, inRundown); break;
      case kRecordExit: onExit(r); break;
      case kRecordComm: onComm(r); break;
      case kRecordMmap:
      case kRecordMmap2: onMmap(r); break;
      case kRecordSwitch:
      case kRecordSwitchCpuWide: onSwitchRecord(r); break;
      default: break;  // LOST / THROTTLE and the rest do not matter for the live view
    }
  }
};

LiveDecoder::LiveDecoder(LiveDecoderConfig config) : m_impl(std::make_unique<Impl>(std::move(config))) {}
LiveDecoder::~LiveDecoder() = default;

const LiveDecoderStats& LiveDecoder::stats() const {
  return m_impl->stats;
}

void LiveDecoder::decode(std::span<const uint8_t> records, bool rundown) {
  Impl& m = *m_impl;
  std::vector<ParsedRecord> parsed;
  size_t consumed = 0;
  const ParseStatus walk = forEachRecord(records, consumed, [&](std::span<const uint8_t> rec) {
    ParsedRecord pr;
    switch (parseRecord(rec, m.cfg.layout, pr)) {
      case ParseStatus::Ok: parsed.push_back(pr); break;
      case ParseStatus::Unsupported: ++m.stats.unsupported; break;
      default: ++m.stats.malformed; break;
    }
  });
  if (walk != ParseStatus::Ok) ++m.stats.malformed;
  // Rings are drained independently, so a chunk's records interleave with another's by a few milliseconds:
  // merge by time, the metadata records (FORK / COMM / MMAP) ahead of samples with the same timestamp.
  auto heavy = [](const ParsedRecord& r) {
    return r.type() == kRecordSample || r.type() == kRecordSwitch || r.type() == kRecordSwitchCpuWide;
  };
  if (!rundown) {
    std::stable_sort(parsed.begin(), parsed.end(), [&](const ParsedRecord& a, const ParsedRecord& b) {
      return a.time != b.time ? a.time < b.time : (heavy(a) ? 1 : 0) < (heavy(b) ? 1 : 0);
    });
  }
  m.inRundown = rundown;
  for (const ParsedRecord& r : parsed) m.onRecord(r);
  m.inRundown = false;
  if (rundown) ++m.epoch;
}

// ---------------------------------------------------------------------------------------------------------
// LiveSource

LiveSource::LiveSource(LiveSourceConfig config) : m_config(std::move(config)) {
  if (!m_config.nowNs) {
    m_config.nowNs = [] {
      timespec ts{};
      clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
      return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
    };
  }
  m_decoder = std::make_unique<LiveDecoder>(m_config.decoder);
}

LiveSource::~LiveSource() {
  finish();
}

void LiveSource::start() {
  m_thread = std::thread([this] { run(); });
}

void LiveSource::submit(std::span<const uint8_t> meta, std::span<const uint8_t> data, bool rundown) {
  std::lock_guard lock(m_mutex);
  if (m_stop) return;
  ++m_stats.chunksSubmitted;
  if (!meta.empty()) {
    if (m_queuedBytes + meta.size() > m_config.queueBytes + m_config.metaQueueBytes) {
      ++m_stats.metaLost;
    } else {
      m_queue.push_back(Chunk{std::vector<uint8_t>(meta.begin(), meta.end()), rundown});
      m_queuedBytes += meta.size();
    }
  }
  if (!data.empty()) {
    if (m_queuedBytes + data.size() > m_config.queueBytes) {
      size_t consumed = 0, records = 0;
      (void)forEachRecord(data, consumed, [&](std::span<const uint8_t>) { ++records; });
      m_stats.droppedRecords += records;
      m_droppedPending += records;
    } else {
      m_queue.push_back(Chunk{std::vector<uint8_t>(data.begin(), data.end()), false});
      m_queuedBytes += data.size();
    }
  }
  m_stats.maxQueuedBytes = std::max(m_stats.maxQueuedBytes, m_queuedBytes);
  m_cv.notify_one();
}

void LiveSource::finish() {
  {
    std::lock_guard lock(m_mutex);
    if (m_stop && !m_thread.joinable()) return;
    m_stop = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable()) m_thread.join();
}

LiveSourceStats LiveSource::stats() const {
  std::lock_guard lock(m_mutex);
  LiveSourceStats s = m_stats;
  s.queuedBytes = m_queuedBytes;
  s.decoder = m_decoder->stats();  // racy read of counters: diagnostics only
  return s;
}

void LiveSource::run() {
  ILiveSink& sink = *m_config.decoder.sink;
  for (;;) {
    std::deque<Chunk> work;
    uint64_t dropped = 0;
    bool stopping = false;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait_for(lock, m_config.period, [this] { return m_stop; });
      work.swap(m_queue);
      m_queuedBytes = 0;
      dropped = std::exchange(m_droppedPending, 0);
      stopping = m_stop;
    }
    if (!work.empty()) {
      const auto began = std::chrono::steady_clock::now();
      // Rundown chunks first, then the rest in arrival order (decode() merges one chunk by time; chunks of
      // one wake-up are concatenated so that rings interleave correctly).
      std::vector<uint8_t> merged;
      for (Chunk& c : work) {
        if (c.rundown) m_decoder->decode(c.bytes, true);
      }
      for (Chunk& c : work)
        if (!c.rundown) merged.insert(merged.end(), c.bytes.begin(), c.bytes.end());
      if (!merged.empty()) m_decoder->decode(merged, false);
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
      std::lock_guard lock(m_mutex);
      m_stats.decodeSeconds += secs;
      ++m_stats.batches;
    }
    if (dropped != 0) sink.onDropped(dropped);
    sink.onTimeAdvance(m_config.nowNs());
    if (stopping) {
      std::lock_guard lock(m_mutex);
      if (m_queue.empty()) return;
    }
  }
}

}  // namespace tachyon::capture::perf
