#include "capture/linux_perf/perf_converter.h"

#include "capture/linux_perf/perf_system.h"
#include "capture/windows_etw/glob_match.h"
#include "core/hash.h"
#include "core/log.h"
#include "symbols/dwarf/elf_reader.h"
#include "trace/trace_writer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <set>
#include <unordered_map>

namespace tachyon::capture::perf {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kNoFile = 0xFFFFFFFFu;
constexpr uint32_t kNoModule = 0xFFFFFFFEu;  // the mapping is not backed by a module (absolute frames)
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kSampleFlushCount = 262144;
constexpr const char* kConverterVersion = "1";
constexpr int kMaxAncestors = 64;

uint64_t alignDown(uint64_t v) {
  return v & ~(kPageSize - 1);
}
uint64_t alignUp(uint64_t v) {
  return (v + kPageSize - 1) & ~(kPageSize - 1);
}

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// A name that is not a file to read: special mappings and anonymous memory.
bool isPseudoPath(std::string_view p) {
  return p.empty() || p[0] == '[' || p.rfind("//anon", 0) == 0 || p.rfind("/memfd:", 0) == 0 ||
         p.rfind("/dev/", 0) == 0 || p.rfind("/SYSV", 0) == 0 || p[0] != '/';
}

// --- files and modules ------------------------------------------------------------------------------------

struct FileInfo {
  bool loaded = false;
  bool readable = false;  // the file could be opened (NotFound / IoError otherwise)
  bool isElf = false;
  std::vector<symbols::ElfLoadSegment> segs;
  uint64_t lowestAligned = 0;  // lowest PT_LOAD vaddr, aligned down
  uint64_t imageSize = 0;      // end of the highest PT_LOAD minus lowestAligned
  std::vector<uint8_t> buildId;
};

struct FileEntry {
  std::string path;
  uint32_t devMajor = 0, devMinor = 0;
  uint64_t inode = 0;
  std::vector<uint8_t> kernelBuildId;  // from MMAP2, when the kernel supplied it
  FileInfo info;
};

struct Mapping {
  uint64_t start = 0, end = 0, pgoff = 0;
  uint32_t prot = 0;
  uint32_t file = kNoFile;
  uint64_t mapTs = 0;
  uint32_t module = kNone;  // cache: ModuleIdx, kNoModule, or kNone (not resolved yet)
  uint64_t base = 0;        // cache: the module's base
};

struct AddressSpace {
  uint32_t id = 0;     // identifies this exact address space (a copy gets a new id): stack cache key
  uint32_t epoch = 0;  // bumped by every mapping change: stack cache validity
  std::map<uint64_t, Mapping> maps;
  std::map<std::pair<uint32_t, uint64_t>, uint32_t> moduleByKey;  // (file, bias) -> module
  std::vector<uint32_t> modules;                                  // modules created through this space

  void insert(const Mapping& m) {
    if (m.end <= m.start) return;
    ++epoch;
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

  Mapping* find(uint64_t addr) {
    auto it = maps.upper_bound(addr);
    if (it == maps.begin()) return nullptr;
    --it;
    return addr < it->second.end ? &it->second : nullptr;
  }
};

struct ModuleBuild {
  trace::ModuleRec rec;
  std::string path;
  bool unloaded = false;
};

// --- processes and threads --------------------------------------------------------------------------------

struct Proc {
  uint32_t pid = 0, ppid = 0;
  uint32_t parent = kNone;  // index into procs
  std::string comm;
  uint64_t startTs = 0, endTs = 0;
  bool preexisting = false;
  bool fromRundown = false;  // created by the /proc rundown (a live FORK of the same process is a duplicate)
  bool exited = false;
  bool isRoot = false;
  uint32_t liveThreads = 0;
  std::shared_ptr<AddressSpace> as;
  uint64_t memoEpoch = ~0ull;
  bool memoTarget = false;
  bool used = false;  // has an emitted thread
};

struct Thr {
  uint32_t tid = 0;
  uint32_t proc = kNone;
  uint64_t startTs = 0, endTs = 0;
  bool preexisting = false;
  bool fromRundown = false;
  bool exited = false;
  std::string name;
  uint64_t kept = 0;
};

struct NameEvent {
  uint64_t ts;
  uint32_t thread;
  std::string name;
};

class SampleSink {
 public:
  explicit SampleSink(trace::TraceWriter& w) : m_writer(w) {}

  void push(uint64_t ts, uint32_t tid, CallstackIdx cs, uint16_t cpu, uint16_t flags) {
    m_batch.push(ts, tid, cs, cpu, flags);
    if (m_batch.size() >= kSampleFlushCount) flush(false);
  }

  // Writes the batch sorted by time. While more samples may follow, the newest eighth stays behind so that a
  // slightly out-of-order stream never produces an inversion across chunks.
  void flush(bool final) {
    const size_t n = m_batch.size();
    if (n == 0) return;
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return m_batch.ts[a] < m_batch.ts[b]; });
    const size_t emit = final ? n : n - n / 8;
    trace::SampleBatch out, rest;
    for (size_t k = 0; k < n; ++k) {
      const uint32_t i = order[k];
      trace::SampleBatch& dst = k < emit ? out : rest;
      dst.push(m_batch.ts[i], m_batch.tid[i], m_batch.callstack[i], m_batch.cpu[i], m_batch.flags[i]);
    }
    if (out.size() != 0) m_writer.append(out);
    m_batch = std::move(rest);
  }

 private:
  trace::TraceWriter& m_writer;
  trace::SampleBatch m_batch;
};

class Converter {
 public:
  Converter(const SpoolSet& set, const ConvertOptions& opt, trace::TraceWriter& writer, ConvertStats& stats)
      : m_set(set), m_opt(opt), m_writer(writer), m_stats(stats), m_sink(writer) {
    m_roots.insert(opt.rootPids.begin(), opt.rootPids.end());
  }

  Result<void> run();

 private:
  // --- record handlers
  void onRecord(const ParsedRecord& r);
  void onFork(const ParsedRecord& r, bool rundown);
  void onExit(const ParsedRecord& r);
  void onComm(const ParsedRecord& r);
  void onMmap(const ParsedRecord& r);
  void onSample(const ParsedRecord& r);
  CallstackIdx resolveStack(Proc& p, const ParsedRecord& r, uint16_t& flags);
  void onThrottle(const ParsedRecord& r, bool unthrottle);

  // --- tables
  uint32_t ensureProc(uint32_t pid, uint64_t ts);
  uint32_t ensureThread(uint32_t tid, uint32_t proc, uint64_t ts);
  uint32_t createProc(uint32_t pid, uint32_t ppid, uint64_t ts, bool preexisting);
  bool isTarget(uint32_t procIdx);
  bool keep(uint32_t procIdx) { return m_roots.empty() || isTarget(procIdx); }
  std::shared_ptr<AddressSpace> newAddressSpace() {
    auto as = std::make_shared<AddressSpace>();
    as->id = ++m_nextAsId;
    return as;
  }
  void ownAddressSpace(Proc& p);
  void dropAddressSpace(Proc& p, uint64_t ts);
  void markUnloaded(const std::vector<uint32_t>& modules, uint64_t ts);

  // --- symbolisation
  FrameIdx frameFor(Proc& p, uint64_t addr);
  uint32_t fileIndex(const ParsedRecord& r);
  void loadFile(FileEntry& fe, const Proc& p, const Mapping& m);
  uint64_t biasOf(const FileEntry& fe, const Mapping& m) const;
  uint32_t moduleFor(Proc& p, Mapping& m);

  Result<void> readRundown(int phase);
  Result<void> mergeStreams();
  Result<void> finalize();
  void finishTables(uint64_t startTs, uint64_t endTs);
  std::string commandLineOf(const Proc& p) const;

  const SpoolSet& m_set;
  const ConvertOptions& m_opt;
  trace::TraceWriter& m_writer;
  ConvertStats& m_stats;
  SampleSink m_sink;

  std::vector<SpoolFile> m_metaFiles, m_dataFiles;
  RecordLayout m_layout;
  uint64_t m_startTs = 0, m_lastTs = 0;
  uint64_t m_epoch = 0;
  uint32_t m_nextAsId = 0;

  // Samples repeat the same callchains over and over (a hot loop is the same stack thousands of times), so a
  // direct-mapped cache remembers the callstack a (address space, raw chain) resolved to. The key is two
  // independent 64-bit hashes of the raw chain plus the address space identity and its mapping epoch.
  struct StackCacheEntry {
    uint64_t h1 = 0, h2 = 0, ip = 0;
    uint32_t asId = 0, asEpoch = 0, count = 0;
    uint32_t cs = 0;
    uint32_t frames = 0, unmapped = 0, kernel = 0;
    uint16_t flags = 0;
    bool kernelMode = false;
    bool valid = false;
  };
  static constexpr size_t kStackCacheSize = 1u << 16;
  std::vector<StackCacheEntry> m_stackCache = std::vector<StackCacheEntry>(kStackCacheSize);

  std::set<uint32_t> m_roots;
  std::set<uint32_t> m_rootsSeen;
  std::vector<Proc> m_procs;
  std::unordered_map<uint32_t, uint32_t> m_pidToProc;
  std::vector<Thr> m_threads;
  std::unordered_map<uint32_t, uint32_t> m_tidToThread;  // live threads
  std::vector<NameEvent> m_names;

  std::vector<FileEntry> m_files;
  std::unordered_map<std::string, uint32_t> m_fileIndex;
  std::vector<ModuleBuild> m_modules;

  std::vector<FrameIdx> m_stack;
  std::unordered_map<uint32_t, uint64_t> m_openThrottle;  // cpu -> THROTTLE time
  uint64_t m_recordCounter = 0;
  bool m_inRundown = false;
};

// ---------------------------------------------------------------------------------------------------------
// tables

uint32_t Converter::createProc(uint32_t pid, uint32_t ppid, uint64_t ts, bool preexisting) {
  Proc p;
  p.pid = pid;
  p.ppid = ppid;
  p.startTs = preexisting ? m_startTs : ts;
  p.preexisting = preexisting;
  p.as = newAddressSpace();
  if (m_roots.count(pid) != 0 && m_rootsSeen.insert(pid).second) p.isRoot = true;
  m_procs.push_back(std::move(p));
  const uint32_t idx = static_cast<uint32_t>(m_procs.size() - 1);
  m_pidToProc[pid] = idx;
  ++m_epoch;
  return idx;
}

uint32_t Converter::ensureProc(uint32_t pid, uint64_t ts) {
  auto it = m_pidToProc.find(pid);
  if (it != m_pidToProc.end()) return it->second;
  return createProc(pid, 0, ts, true);  // seen without a FORK: it existed before the capture
}

uint32_t Converter::ensureThread(uint32_t tid, uint32_t proc, uint64_t ts) {
  auto it = m_tidToThread.find(tid);
  if (it != m_tidToThread.end() && m_threads[it->second].proc == proc) return it->second;
  Thr t;
  t.tid = tid;
  t.proc = proc;
  t.preexisting = true;
  t.startTs = m_startTs;
  (void)ts;
  m_threads.push_back(std::move(t));
  const uint32_t idx = static_cast<uint32_t>(m_threads.size() - 1);
  m_tidToThread[tid] = idx;
  ++m_procs[proc].liveThreads;
  return idx;
}

bool Converter::isTarget(uint32_t idx) {
  Proc& p = m_procs[idx];
  if (p.memoEpoch == m_epoch) return p.memoTarget;
  bool result = false;
  uint32_t cur = idx;
  for (int depth = 0; depth < kMaxAncestors && cur != kNone; ++depth) {
    const Proc& c = m_procs[cur];
    if (c.isRoot) {
      result = true;
      break;
    }
    if (!m_opt.followChildren) break;
    if (!m_opt.childFilter.empty() && !etw::globMatch(m_opt.childFilter, c.comm, false)) break;
    cur = c.parent;
  }
  p.memoEpoch = m_epoch;
  p.memoTarget = result;
  return result;
}

void Converter::markUnloaded(const std::vector<uint32_t>& modules, uint64_t ts) {
  for (uint32_t m : modules) {
    if (m < m_modules.size() && !m_modules[m].unloaded) {
      m_modules[m].rec.unload_ts = ts;
      m_modules[m].unloaded = true;
    }
  }
}

void Converter::ownAddressSpace(Proc& p) {
  if (p.as.use_count() > 1) {
    p.as = std::make_shared<AddressSpace>(*p.as);
    p.as->id = ++m_nextAsId;
    p.as->epoch = 0;
  }
}

void Converter::dropAddressSpace(Proc& p, uint64_t ts) {
  if (p.as && p.as.use_count() == 1) markUnloaded(p.as->modules, ts);
  p.as = newAddressSpace();
}

// ---------------------------------------------------------------------------------------------------------
// handlers

void Converter::onFork(const ParsedRecord& r, bool rundown) {
  const uint64_t ts = r.time;
  if (r.pid == r.tid) {  // a new process
    auto old = m_pidToProc.find(r.pid);
    if (old != m_pidToProc.end() && !m_procs[old->second].exited && !rundown) {
      Proc& o = m_procs[old->second];
      if (o.fromRundown && o.ppid == r.ppid) {  // the rundown already saw this process: a duplicate
        o.fromRundown = false;
        return;
      }
      o.exited = true;  // its exit was missed; the pid is being reused
      o.endTs = ts;
    }
    const uint32_t parent = [&] {
      auto it = m_pidToProc.find(r.ppid);
      return it == m_pidToProc.end() ? kNone : it->second;
    }();
    const bool pre = rundown || ts <= m_startTs;
    const uint32_t idx = createProc(r.pid, r.ppid, ts, pre);
    Proc& p = m_procs[idx];
    p.parent = parent;
    p.fromRundown = rundown;
    if (parent != kNone && !rundown) {
      p.comm = m_procs[parent].comm;  // until its own exec says otherwise
      p.as = m_procs[parent].as;      // copy-on-write
    }
    // drop a stale live thread entry of the same tid
    m_tidToThread.erase(r.tid);
    Thr t;
    t.tid = r.tid;
    t.proc = idx;
    t.preexisting = pre;
    t.fromRundown = rundown;
    t.startTs = pre ? m_startTs : ts;
    t.name = p.comm;
    m_threads.push_back(std::move(t));
    m_tidToThread[r.tid] = static_cast<uint32_t>(m_threads.size() - 1);
    ++p.liveThreads;
  } else {  // a new thread of an existing process
    const uint32_t proc = ensureProc(r.pid, ts);
    auto stale = m_tidToThread.find(r.tid);
    if (stale != m_tidToThread.end()) {
      Thr& s = m_threads[stale->second];
      if (s.fromRundown && s.proc == proc && !s.exited && !rundown) {  // already known from the rundown
        s.fromRundown = false;
        return;
      }
      if (!s.exited) {
        s.exited = true;
        s.endTs = ts;
        if (m_procs[s.proc].liveThreads > 0) --m_procs[s.proc].liveThreads;
      }
      m_tidToThread.erase(stale);
    }
    Thr t;
    t.tid = r.tid;
    t.proc = proc;
    t.preexisting = rundown || ts <= m_startTs;
    t.fromRundown = rundown;
    t.startTs = t.preexisting ? m_startTs : ts;
    t.name = m_procs[proc].comm;
    m_threads.push_back(std::move(t));
    m_tidToThread[r.tid] = static_cast<uint32_t>(m_threads.size() - 1);
    ++m_procs[proc].liveThreads;
  }
}

void Converter::onExit(const ParsedRecord& r) {
  auto it = m_tidToThread.find(r.tid);
  if (it == m_tidToThread.end()) return;
  Thr& t = m_threads[it->second];
  t.exited = true;
  t.endTs = r.time;
  m_tidToThread.erase(it);
  Proc& p = m_procs[t.proc];
  if (p.liveThreads > 0) --p.liveThreads;
  if (p.liveThreads == 0) {
    p.exited = true;
    p.endTs = r.time;
    if (p.as && p.as.use_count() == 1) markUnloaded(p.as->modules, r.time);
  }
  ++m_epoch;
}

void Converter::onComm(const ParsedRecord& r) {
  const uint32_t proc = ensureProc(r.pid, r.time);
  const uint32_t thr = ensureThread(r.tid, proc, r.time);
  Proc& p = m_procs[proc];
  m_threads[thr].name = std::string(r.name);
  if (!m_inRundown) m_names.push_back({r.time, thr, std::string(r.name)});
  const bool exec = r.isExecComm();
  if (r.tid == r.pid || exec) p.comm = std::string(r.name);
  if (exec && !m_inRundown) dropAddressSpace(p, r.time);  // a new program image: the old mappings are gone
  ++m_epoch;
}

void Converter::onMmap(const ParsedRecord& r) {
  if (r.mapLen == 0) return;
  const bool isMmap2 = r.type() == kRecordMmap2;
  const bool exec = isMmap2 ? (r.prot & kProtExec) != 0 : (r.misc() & kMiscMmapData) == 0;
  if (!exec) return;
  const uint32_t proc = ensureProc(r.pid, r.time);
  Proc& p = m_procs[proc];
  ownAddressSpace(p);
  Mapping m;
  m.start = r.mapAddr;
  m.end = r.mapAddr + r.mapLen;
  if (m.end < m.start) return;
  m.pgoff = r.pgoff;
  m.prot = r.prot;
  m.mapTs = m_inRundown ? m_startTs : r.time;
  m.file = isPseudoPath(r.name) ? kNoFile : fileIndex(r);
  p.as->insert(m);
}

uint32_t Converter::fileIndex(const ParsedRecord& r) {
  std::string key(r.name);
  key.push_back('\0');
  key += std::to_string(r.devMajor) + ":" + std::to_string(r.devMinor) + ":" + std::to_string(r.inode);
  auto it = m_fileIndex.find(key);
  if (it != m_fileIndex.end()) {
    FileEntry& fe = m_files[it->second];
    if (fe.kernelBuildId.empty() && r.buildIdSize > 0)
      fe.kernelBuildId.assign(r.buildId, r.buildId + r.buildIdSize);
    return it->second;
  }
  FileEntry fe;
  fe.path = std::string(r.name);
  fe.devMajor = r.devMajor;
  fe.devMinor = r.devMinor;
  fe.inode = r.inode;
  if (r.buildIdSize > 0) fe.kernelBuildId.assign(r.buildId, r.buildId + r.buildIdSize);
  m_files.push_back(std::move(fe));
  const uint32_t idx = static_cast<uint32_t>(m_files.size() - 1);
  m_fileIndex.emplace(std::move(key), idx);
  return idx;
}

void Converter::loadFile(FileEntry& fe, const Proc& p, const Mapping& m) {
  fe.info.loaded = true;
  fs::path path = fe.path;
  if (m_opt.fileMapper) path = m_opt.fileMapper(p.pid, fe.path);
  std::error_code ec;
  if (endsWith(fe.path, " (deleted)") && !m_opt.fileMapper) {
    // The file was replaced or removed: read the mapped inode through the process's map_files.
    char range[40];
    std::snprintf(range, sizeof(range), "%lx-%lx", static_cast<unsigned long>(m.start),
                  static_cast<unsigned long>(m.end));
    path = m_opt.procRoot / std::to_string(p.pid) / "map_files" / range;
  }
  auto elf = symbols::ElfFile::open(path);
  if (!elf.ok()) {
    fe.info.readable = elf.error().code != ErrorCode::NotFound && elf.error().code != ErrorCode::IoError;
    return;  // unreadable (NotFound / IoError) or not an ELF (readable == true, isElf == false)
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

uint64_t Converter::biasOf(const FileEntry& fe, const Mapping& m) const {
  if (fe.info.isElf) {
    for (const auto& s : fe.info.segs) {
      const uint64_t segOff = alignDown(s.offset);
      const uint64_t segEnd = alignUp(s.offset + s.filesz);
      if (m.pgoff >= segOff && m.pgoff < segEnd) {
        const uint64_t vaddrAtStart = alignDown(s.vaddr) + (m.pgoff - segOff);
        return m.start - vaddrAtStart;  // modular arithmetic: the bias of a PIE image is its load address
      }
    }
    if (!fe.info.segs.empty()) {  // pgoff outside every segment: use the first one's offset-to-vaddr relation
      const auto& s = fe.info.segs.front();
      return m.start - (m.pgoff - alignDown(s.offset) + alignDown(s.vaddr));
    }
  }
  return m.start - m.pgoff;
}

uint32_t Converter::moduleFor(Proc& p, Mapping& m) {
  if (m.module != kNone) return m.module;
  FileEntry& fe = m_files[m.file];
  if (!fe.info.loaded) loadFile(fe, p, m);
  if (fe.info.readable && !fe.info.isElf) {
    m.module = kNoModule;  // a readable file that is not an ELF image (a mapped data file, a JIT dump)
    return m.module;
  }
  const uint64_t bias = biasOf(fe, m);
  const std::pair<uint32_t, uint64_t> key{m.file, bias};
  auto it = p.as->moduleByKey.find(key);
  if (it != p.as->moduleByKey.end()) {
    m.module = it->second;
    m.base = m_modules[m.module].rec.base;
    return m.module;
  }
  ModuleBuild mb;
  alignas(8) trace::ModuleRec rec{};
  rec.pid = p.pid;
  rec.base = bias + (fe.info.isElf ? fe.info.lowestAligned : 0);
  rec.size =
      fe.info.isElf && fe.info.imageSize != 0 ? fe.info.imageSize : (m.end > rec.base ? m.end - rec.base : 0);
  mb.path = fe.path;
  // The kernel's build-id describes the inode that was actually mapped; the file on disk may have changed.
  const std::vector<uint8_t>* id =
      !fe.kernelBuildId.empty() ? &fe.kernelBuildId : (fe.info.buildId.empty() ? nullptr : &fe.info.buildId);
  if (id != nullptr && id->size() <= sizeof(rec.id)) {
    rec.id_kind = trace::kModuleIdGnuBuildId;
    rec.id_len = static_cast<uint8_t>(id->size());
    std::memcpy(rec.id, id->data(), id->size());
    ++m_stats.modulesWithBuildId;
    if (!fe.kernelBuildId.empty()) ++m_stats.modulesFromKernelId;
  }
  if (!fe.info.isElf) ++m_stats.modulesNoFile;
  rec.load_ts = m.mapTs;
  rec.unload_ts = 0;  // filled in when the module goes away, else the trace end
  mb.rec = rec;
  m_modules.push_back(std::move(mb));
  const uint32_t idx = static_cast<uint32_t>(m_modules.size() - 1);
  p.as->moduleByKey.emplace(key, idx);
  p.as->modules.push_back(idx);
  m.module = idx;
  m.base = rec.base;
  return idx;
}

FrameIdx Converter::frameFor(Proc& p, uint64_t addr) {
  ++m_stats.framesTotal;
  Mapping* m = p.as ? p.as->find(addr) : nullptr;
  if (m != nullptr && m->file != kNoFile) {
    const uint32_t mod = moduleFor(p, *m);
    if (mod != kNoModule && addr >= m->base) return m_writer.internFrame(ModuleIdx(mod), addr - m->base);
  }
  ++m_stats.framesUnmapped;
  return m_writer.internFrame(ModuleIdx::invalid(), addr);
}

void Converter::onSample(const ParsedRecord& r) {
  ++m_stats.samplesRead;
  if (m_opt.windowStartTs != 0 && r.time < m_opt.windowStartTs) {
    ++m_stats.samplesDroppedWindow;
    return;
  }
  if (r.pid == 0 && !m_opt.keepIdle) {
    ++m_stats.samplesDroppedIdle;
    return;
  }
  const uint32_t procIdx = ensureProc(r.pid, r.time);
  if (!keep(procIdx)) {
    ++m_stats.samplesDroppedNotTarget;
    return;
  }
  Proc& p = m_procs[procIdx];
  const uint32_t thrIdx = ensureThread(r.tid, procIdx, r.time);

  const bool kernelMode = r.cpuMode() == kMiscKernel;
  const size_t chainBytes = static_cast<size_t>(r.chainCount) * 8;
  const uint64_t h1 = core::hashBytes(r.chain, chainBytes, core::hashCombine(r.ip, kernelMode ? 1u : 0u));
  const uint64_t h2 = core::hashBytes(r.chain, chainBytes, 0x9E3779B97F4A7C15ull ^ r.chainCount);
  StackCacheEntry& slot = m_stackCache[h1 & (kStackCacheSize - 1)];
  CallstackIdx cs;
  uint16_t flags = 0;
  if (slot.valid && slot.h1 == h1 && slot.h2 == h2 && slot.asId == p.as->id && slot.asEpoch == p.as->epoch &&
      slot.ip == r.ip && slot.count == r.chainCount && slot.kernelMode == kernelMode) {
    cs = CallstackIdx(slot.cs);
    flags = slot.flags;
    m_stats.framesTotal += slot.frames;
    m_stats.framesUnmapped += slot.unmapped;
    m_stats.framesKernel += slot.kernel;
  } else {
    const uint64_t framesBefore = m_stats.framesTotal, unmappedBefore = m_stats.framesUnmapped,
                   kernelBefore = m_stats.framesKernel;
    cs = resolveStack(p, r, flags);
    slot.h1 = h1;
    slot.h2 = h2;
    slot.ip = r.ip;
    slot.asId = p.as->id;
    slot.asEpoch = p.as->epoch;  // read after the resolution: module creation does not change the mappings
    slot.count = static_cast<uint32_t>(r.chainCount);
    slot.kernelMode = kernelMode;
    slot.cs = cs.get();
    slot.flags = flags;
    slot.frames = static_cast<uint32_t>(m_stats.framesTotal - framesBefore);
    slot.unmapped = static_cast<uint32_t>(m_stats.framesUnmapped - unmappedBefore);
    slot.kernel = static_cast<uint32_t>(m_stats.framesKernel - kernelBefore);
    slot.valid = true;
  }
  m_sink.push(r.time, r.tid, cs, static_cast<uint16_t>(std::min<uint32_t>(r.cpu, 0xFFFF)), flags);
  ++m_threads[thrIdx].kept;
  ++m_stats.samplesWritten;
}

// Resolves a sample's raw callchain to a callstack: strips the context markers, resolves user addresses to
// (module, RVA), leaves kernel and unmapped addresses absolute, and sets the kernel-leaf flag.
CallstackIdx Converter::resolveStack(Proc& p, const ParsedRecord& r, uint16_t& flags) {
  // Callchain: strip the context markers; the section a frame belongs to decides kernel vs user.
  m_stack.clear();
  bool kernelSection = r.cpuMode() == kMiscKernel;  // without markers the cpu mode decides
  bool leafKernel = false;
  bool first = true;
  uint64_t firstAddr = 0;
  for (uint64_t i = 0; i < r.chainCount; ++i) {
    const uint64_t v = r.chainAt(i);
    if (v >= kContextMax) {  // a marker
      kernelSection = v == kContextKernel || v == kContextGuestKernel;
      continue;
    }
    if (first) {
      first = false;
      leafKernel = kernelSection;
      firstAddr = v;
    }
    if (kernelSection) {
      ++m_stats.framesTotal;
      ++m_stats.framesKernel;
      m_stack.push_back(m_writer.internFrame(ModuleIdx::invalid(), v));
    } else {
      m_stack.push_back(frameFor(p, v));
    }
  }
  const bool kernelMode = r.cpuMode() == kMiscKernel;
  if (r.ip != 0 && (m_stack.empty() || firstAddr != r.ip)) {
    // The chain is empty, or its kernel part was excluded (kernel_stacks off) while the sample was taken in
    // the kernel, or it does not start at the sampled ip: the ip becomes the leaf.
    const bool kernelIp = kernelMode || r.ip >= 0xFFFF800000000000ull;
    FrameIdx leaf;
    if (kernelIp) {
      ++m_stats.framesTotal;
      ++m_stats.framesKernel;
      leaf = m_writer.internFrame(ModuleIdx::invalid(), r.ip);
    } else {
      leaf = frameFor(p, r.ip);
    }
    m_stack.insert(m_stack.begin(), leaf);
    leafKernel = kernelIp;
  }
  flags = leafKernel ? trace::kSampleFlagKernelLeaf : uint16_t{0};
  return m_writer.internCallstack(std::span<const FrameIdx>(m_stack));
}

void Converter::onThrottle(const ParsedRecord& r, bool unthrottle) {
  if (!unthrottle) {
    ++m_stats.throttleEvents;
    m_openThrottle[r.cpu] = r.time;
  } else {
    auto it = m_openThrottle.find(r.cpu);
    if (it != m_openThrottle.end()) {
      if (r.time > it->second) m_stats.throttledNs += r.time - it->second;
      m_openThrottle.erase(it);
    }
  }
}

void Converter::onRecord(const ParsedRecord& r) {
  ++m_stats.recordsRead;
  if (r.hasTime) m_lastTs = std::max(m_lastTs, r.time);
  switch (r.type()) {
    case kRecordSample: onSample(r); break;
    case kRecordFork: onFork(r, false); break;
    case kRecordExit: onExit(r); break;
    case kRecordComm: onComm(r); break;
    case kRecordMmap:
    case kRecordMmap2: onMmap(r); break;
    case kRecordLost:
    case kRecordLostSamples: m_stats.eventsLost += r.lost; break;
    case kRecordThrottle: onThrottle(r, false); break;
    case kRecordUnthrottle: onThrottle(r, true); break;
    // WP-71 hook: kRecordSwitch / kRecordSwitchCpuWide will produce ContextSwitchRec / ReadyRec here.
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------------------
// driving

Result<void> Converter::readRundown(int phase) {
  // Phase 0: FORK, 1: COMM, 2: MMAP2. The rundown is a few thousand records, so it is read three times
  // instead of being held in memory.
  for (const SpoolFile& f : m_metaFiles) {
    std::vector<RecordStream::Extent> extents;
    for (const SpoolBatchRef& b : f.batches())
      if (b.ring == kSpoolRingRundown) extents.push_back({&f, b});
    if (extents.empty()) continue;
    RecordStream stream(m_layout, std::move(extents));
    while (stream.advance()) {
      const ParsedRecord& r = stream.current();
      if (phase == 0 && r.type() == kRecordFork)
        onFork(r, true);
      else if (phase == 1 && r.type() == kRecordComm)
        onComm(r);
      else if (phase == 2 && (r.type() == kRecordMmap2 || r.type() == kRecordMmap))
        onMmap(r);
    }
    m_stats.recordsUnsupported += stream.skippedUnsupported();
    m_stats.recordsMalformed += stream.malformedBatches();
  }
  return ok();
}

Result<void> Converter::mergeStreams() {
  // One stream per (class, ring); meta streams first so that FORK / COMM / MMAP2 win ties against samples.
  std::vector<RecordStream> streams;
  auto addStreams = [&](const std::vector<SpoolFile>& files) {
    std::map<uint32_t, std::vector<RecordStream::Extent>> byRing;
    for (const SpoolFile& f : files)
      for (const SpoolBatchRef& b : f.batches())
        if (b.ring != kSpoolRingRundown) byRing[b.ring].push_back({&f, b});
    for (auto& [ring, extents] : byRing) streams.emplace_back(m_layout, std::move(extents));
  };
  addStreams(m_metaFiles);
  addStreams(m_dataFiles);

  struct Head {
    uint64_t ts;
    size_t stream;
    bool operator>(const Head& o) const { return ts != o.ts ? ts > o.ts : stream > o.stream; }
  };
  std::priority_queue<Head, std::vector<Head>, std::greater<Head>> heap;
  for (size_t i = 0; i < streams.size(); ++i)
    if (streams[i].advance()) heap.push({streams[i].current().time, i});

  const uint64_t endHint = m_opt.endTs != 0 ? m_opt.endTs : m_set.info.endTs;
  const uint64_t span = endHint > m_startTs ? endHint - m_startTs : 0;
  while (!heap.empty()) {
    const Head h = heap.top();
    heap.pop();
    RecordStream& s = streams[h.stream];
    onRecord(s.current());
    if (m_opt.progress && (++m_recordCounter & 0xFFFF) == 0 && span != 0)
      m_opt.progress(std::min(0.95f, static_cast<float>(m_lastTs - m_startTs) / static_cast<float>(span)));
    if (s.advance()) heap.push({s.current().time, h.stream});
  }
  for (const RecordStream& s : streams) {
    m_stats.recordsUnsupported += s.skippedUnsupported();
    m_stats.recordsMalformed += s.malformedBatches() + s.readErrors();
  }
  return ok();
}

std::string Converter::commandLineOf(const Proc& p) const {
  if (p.isRoot && !m_opt.rootCommandLine.empty()) return m_opt.rootCommandLine;
  if (p.exited) return {};
  std::ifstream in(m_opt.procRoot / std::to_string(p.pid) / "cmdline", std::ios::binary);
  if (!in) return {};
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  for (char& c : raw)
    if (c == '\0') c = ' ';
  while (!raw.empty() && raw.back() == ' ') raw.pop_back();
  return raw;
}

void Converter::finishTables(uint64_t startTs, uint64_t endTs) {
  // Which threads / processes are written: threads with kept samples, and every known thread of a target
  // process. (System mode without roots: only sampled threads.)
  std::vector<bool> emit(m_threads.size(), false);
  for (size_t i = 0; i < m_threads.size(); ++i) {
    const Thr& t = m_threads[i];
    bool e = t.kept > 0;
    if (!e && !m_roots.empty() && t.proc != kNone) e = isTarget(t.proc);
    emit[i] = e;
    if (e) m_procs[t.proc].used = true;
  }
  // The parents of used processes are not written unless used themselves: ProcessRec.parent_pid is a number.

  std::vector<trace::ThreadRec> threadRecs;
  std::vector<uint32_t> threadOrder;
  for (size_t i = 0; i < m_threads.size(); ++i) {
    if (!emit[i]) continue;
    const Thr& t = m_threads[i];
    trace::ThreadRec rec{};
    rec.tid = t.tid;
    rec.pid = m_procs[t.proc].pid;
    rec.start_ts = t.preexisting ? startTs : t.startTs;
    rec.end_ts = t.exited ? t.endTs : endTs;
    rec.name = m_writer.internString(t.name);
    rec.flags = (!m_roots.empty() && isTarget(t.proc)) ? trace::kThreadFlagTargetProcess : 0u;
    threadRecs.push_back(rec);
    threadOrder.push_back(static_cast<uint32_t>(i));
  }
  std::vector<uint32_t> sortIdx(threadRecs.size());
  for (uint32_t i = 0; i < sortIdx.size(); ++i) sortIdx[i] = i;
  std::stable_sort(sortIdx.begin(), sortIdx.end(), [&](uint32_t a, uint32_t b) {
    return threadRecs[a].tid != threadRecs[b].tid ? threadRecs[a].tid < threadRecs[b].tid
                                                  : threadRecs[a].start_ts < threadRecs[b].start_ts;
  });
  std::vector<trace::ThreadRec> sortedThreads;
  for (uint32_t i : sortIdx) sortedThreads.push_back(threadRecs[i]);

  std::vector<trace::ProcessRec> processRecs;
  std::vector<uint32_t> procOrder;
  for (size_t i = 0; i < m_procs.size(); ++i)
    if (m_procs[i].used) procOrder.push_back(static_cast<uint32_t>(i));
  std::stable_sort(procOrder.begin(), procOrder.end(), [&](uint32_t a, uint32_t b) {
    return m_procs[a].pid != m_procs[b].pid ? m_procs[a].pid < m_procs[b].pid
                                            : m_procs[a].startTs < m_procs[b].startTs;
  });
  for (uint32_t i : procOrder) {
    const Proc& p = m_procs[i];
    trace::ProcessRec rec{};
    rec.pid = p.pid;
    rec.parent_pid = p.ppid;
    rec.start_ts = p.preexisting ? startTs : p.startTs;
    rec.end_ts = p.exited ? p.endTs : endTs;
    rec.name = m_writer.internString(p.comm);
    rec.command_line = m_writer.internString(commandLineOf(p));
    processRecs.push_back(rec);
  }

  std::vector<trace::ThreadNameRec> nameRecs;
  for (const NameEvent& n : m_names) {
    if (!emit[n.thread]) continue;
    nameRecs.push_back(trace::ThreadNameRec{n.ts, m_threads[n.thread].tid, m_writer.internString(n.name)});
  }

  std::vector<trace::ModuleRec> moduleRecs;
  moduleRecs.reserve(m_modules.size());
  for (ModuleBuild& mb : m_modules) {
    alignas(8) trace::ModuleRec rec = mb.rec;
    rec.path = m_writer.internString(mb.path);
    if (rec.load_ts < startTs) rec.load_ts = startTs;
    if (!mb.unloaded) rec.unload_ts = endTs;
    moduleRecs.push_back(rec);
  }

  m_writer.append(std::span<const trace::ProcessRec>(processRecs));
  m_writer.append(std::span<const trace::ThreadRec>(sortedThreads));
  m_writer.append(std::span<const trace::ModuleRec>(moduleRecs));
  m_writer.append(std::span<const trace::ThreadNameRec>(nameRecs));
  m_stats.processesWritten = processRecs.size();
  m_stats.threadsWritten = sortedThreads.size();
  m_stats.modulesWritten = moduleRecs.size();
}

Result<void> Converter::run() {
  m_layout = m_set.info.layout;
  if (!m_layout.supported() || (m_layout.sampleType & kSampleTime) == 0 || !m_layout.sampleIdAll)
    return makeError(ErrorCode::Unsupported,
                     "the spool was recorded with a sample_type the converter cannot order (needs TIME and "
                     "sample_id_all)");
  m_startTs = m_set.info.startTs;

  // Open the files. The vectors are sized first: streams keep pointers to the elements.
  size_t fileCount = m_set.meta.empty() ? 0 : 1;
  m_metaFiles.reserve(fileCount);
  m_dataFiles.reserve(m_set.data.size());
  if (!m_set.meta.empty()) {
    auto f = SpoolFile::open(m_set.meta);
    if (!f.ok()) return f.error();
    if (f->truncated())
      m_stats.warnings.push_back("meta.bin ends in a truncated batch (the capture did not stop cleanly)");
    m_metaFiles.push_back(std::move(*f));
  }
  for (const fs::path& p : m_set.data) {
    auto f = SpoolFile::open(p);
    if (!f.ok()) {
      m_stats.warnings.push_back("skipped unreadable spool segment " + p.filename().string() + ": " +
                                 f.error().message);
      continue;
    }
    if (f->truncated()) m_stats.warnings.push_back(p.filename().string() + " ends in a truncated batch");
    m_dataFiles.push_back(std::move(*f));
  }
  if (m_startTs == 0) m_startTs = 1;  // never 0: 0 means "before the trace" in the module and thread records

  m_inRundown = true;
  for (int phase = 0; phase < 3; ++phase) TACHYON_TRY(readRundown(phase));
  m_inRundown = false;
  for (Proc& p : m_procs) {  // link parents that were created after their children (rundown order is by pid)
    if (p.parent == kNone) {
      auto it = m_pidToProc.find(p.ppid);
      if (it != m_pidToProc.end() && &m_procs[it->second] != &p) p.parent = it->second;
    }
  }
  ++m_epoch;

  TACHYON_TRY(mergeStreams());
  m_sink.flush(true);
  return finalize();
}

Result<void> Converter::finalize() {
  uint64_t endTs = m_opt.endTs != 0 ? m_opt.endTs : (m_set.info.endTs != 0 ? m_set.info.endTs : m_lastTs);
  if (endTs < m_startTs) endTs = m_startTs;
  m_stats.startTs = m_startTs;
  m_stats.endTs = endTs;
  m_stats.seconds = static_cast<double>(endTs - m_startTs) / 1e9;
  // A THROTTLE that never got its UNTHROTTLE lasted until the end.
  for (const auto& [cpu, ts] : m_openThrottle)
    if (endTs > ts) m_stats.throttledNs += endTs - ts;
  m_openThrottle.clear();

  finishTables(m_startTs, endTs);
  m_writer.setTimeRange(m_startTs, endTs);

  const SessionInfo& si = m_set.info;
  SysctlInfo sys;
  sys.paranoid = si.paranoid;
  sys.maxSampleRate = si.maxSampleRate;
  sys.cpuTimeMaxPercent = si.cpuTimeMaxPercent;
  std::vector<std::string> warnings = si.warnings;
  warnings.insert(warnings.end(), m_stats.warnings.begin(), m_stats.warnings.end());
  if (m_stats.eventsLost > 0)
    warnings.push_back(
        std::to_string(m_stats.eventsLost) +
        " samples/records were lost because the ring buffers overflowed (PERF_RECORD_LOST): the "
        "drain thread could not keep up. Lower sampling_hz, or enlarge the rings "
        "(tachyon-agent --perf-ring-pages)");
  if (m_stats.throttleEvents > 0)
    warnings.push_back("sampling was throttled " + std::to_string(m_stats.throttleEvents) + " time(s) for " +
                       std::to_string(m_stats.throttledNs / 1'000'000) +
                       " ms in total: " + throttleAdvice(sys, si.samplingHz));
  if (si.ringOverruns > 0 || m_stats.recordsMalformed > 0)
    warnings.push_back(std::to_string(si.ringOverruns) + " ring overruns and " +
                       std::to_string(m_stats.recordsMalformed) +
                       " unreadable records were detected; the trace may be incomplete");
  if (!si.finished) warnings.push_back("the capture did not stop cleanly; the spool was converted as found");
  if (m_stats.modulesWithBuildId < m_stats.modulesWritten)
    warnings.push_back(std::to_string(m_stats.modulesWritten - m_stats.modulesWithBuildId) +
                       " modules have no build-id (file unreadable or without .note.gnu.build-id)");
  m_stats.warnings = warnings;

  using nlohmann::json;
  json doc;
  doc["source"] = "perf";
  doc["converter"] = {{"name", "tachyon-perfconvert"}, {"version", kConverterVersion}};
  doc["agent_version"] = m_opt.agentVersion.empty() ? si.agentVersion : m_opt.agentVersion;
  doc["sampling_hz"] = si.samplingHz;
  doc["events_lost"] = m_stats.eventsLost;
  doc["clock_frequency_hz"] = 1'000'000'000ull;
  doc["mode"] = m_roots.empty() ? "system" : "process";
  doc["target_pids"] = std::vector<uint32_t>(m_roots.begin(), m_roots.end());
  if (!m_opt.captureOptionsJson.empty()) {
    json parsed = json::parse(m_opt.captureOptionsJson, nullptr, false);
    doc["capture_options"] = parsed.is_discarded() ? json(m_opt.captureOptionsJson) : parsed;
  } else {
    doc["capture_options"] = json::object();
  }
  doc["host"] = {{"platform", "linux"},
                 {"logical_cores", si.cpuCount},
                 {"kernel_release", si.kernelRelease},
                 {"hostname", si.hostname},
                 {"pointer_size", 8}};
  doc["perf"] = {{"plan", si.plan},
                 {"rings", si.ringCount},
                 {"kernel_stacks", si.kernelStacks},
                 {"exclude_kernel", si.excludeKernel},
                 {"kernel_build_id", si.kernelBuildId},
                 {"paranoid", si.paranoid},
                 {"max_sample_rate", si.maxSampleRate},
                 {"cpu_time_max_percent", si.cpuTimeMaxPercent},
                 {"clock", "CLOCK_MONOTONIC_RAW"}};
  const double missed = static_cast<double>(m_stats.throttledNs) / 1e9 * si.samplingHz;
  doc["throttle"] = {{"events", m_stats.throttleEvents},
                     {"throttled_ms", m_stats.throttledNs / 1'000'000},
                     {"estimated_missed_samples", static_cast<uint64_t>(missed)}};
  doc["convert_stats"] = {{"records_read", m_stats.recordsRead},
                          {"samples_read", m_stats.samplesRead},
                          {"samples_written", m_stats.samplesWritten},
                          {"samples_dropped_not_target", m_stats.samplesDroppedNotTarget},
                          {"samples_dropped_idle", m_stats.samplesDroppedIdle},
                          {"samples_dropped_window", m_stats.samplesDroppedWindow},
                          {"records_unsupported", m_stats.recordsUnsupported},
                          {"records_malformed", m_stats.recordsMalformed},
                          {"frames_total", m_stats.framesTotal},
                          {"frames_unmapped", m_stats.framesUnmapped},
                          {"frames_kernel", m_stats.framesKernel},
                          {"modules", m_stats.modulesWritten},
                          {"modules_with_build_id", m_stats.modulesWithBuildId},
                          {"modules_from_kernel_id", m_stats.modulesFromKernelId},
                          {"warnings", warnings}};
  m_writer.appendCaptureInfo(doc.dump());
  TACHYON_TRY(m_writer.finish());
  if (m_opt.progress) m_opt.progress(1.0f);
  return ok();
}

}  // namespace

Result<ConvertStats> convertSpool(const SpoolSet& set, const fs::path& out, const ConvertOptions& options) {
  const auto began = std::chrono::steady_clock::now();
  ConvertStats stats;
  trace::FileHeader header;
  header.clock_frequency_hz = 1'000'000'000ull;
  header.platform = trace::kPlatformLinux;
  TACHYON_TRY_ASSIGN(trace::TraceWriter writer, trace::TraceWriter::open(out, header));

  Converter conv(set, options, writer, stats);
  auto ran = conv.run();
  if (!ran.ok()) return ran.error();
  (void)began;
  return stats;
}

}  // namespace tachyon::capture::perf
