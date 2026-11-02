// WP-70: spool to .tachyon conversion from synthetic record streams: modules and the ELF base rule, callchain
// handling, process and thread tables, target selection, lost / throttle accounting, ordering.
#include "capture/linux_perf/perf_converter.h"
#include "linux_perf_test_util.h"
#include "model/model.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <set>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;

using ptest::ElfSegment;
using ptest::ElfSpec;
using ptest::TempDir;
using Id = RecordBuilder::Id;

constexpr uint64_t kStart = 1'000'000;  // session start (ns)
constexpr uint64_t kEnd = 9'000'000;
constexpr uint64_t kKernelText = 0xffffffff81000000ull;

// A spool directory assembled from record builders.
class SpoolFixture {
 public:
  SpoolFixture() {
    m_info.layout = RecordLayout{};
    m_info.startTs = kStart;
    m_info.endTs = kEnd;
    m_info.samplingHz = 8000;
    m_info.mode = "launch";
    m_info.plan = "system-wide";
    m_info.cpuCount = 4;
    m_info.finished = true;
    m_info.maxSampleRate = 100000;
    m_info.cpuTimeMaxPercent = 25;
  }

  RecordBuilder& meta(uint32_t ring = 0) { return m_meta[ring]; }
  RecordBuilder& data(uint32_t ring = 0) { return m_data[ring]; }
  SessionInfo& info() { return m_info; }
  const fs::path& dir() const { return m_dir.path(); }

  // Writes the files and returns the set.
  SpoolSet build() {
    {
      auto w = SpoolWriter::create(spoolMetaPath(m_dir.path()), SpoolKind::Meta, m_info.layout);
      for (auto& [ring, b] : m_meta) EXPECT_TRUE(w->append(ring, b.bytes()).ok());
      EXPECT_TRUE(w->close().ok());
    }
    {
      auto w = SpoolWriter::create(spoolDataPath(m_dir.path(), 0), SpoolKind::Data, m_info.layout);
      for (auto& [ring, b] : m_data) EXPECT_TRUE(w->append(ring, b.bytes()).ok());
      EXPECT_TRUE(w->close().ok());
    }
    EXPECT_TRUE(writeSessionInfo(m_dir.path(), m_info).ok());
    auto set = SpoolSet::openDirectory(m_dir.path());
    EXPECT_TRUE(set.ok());
    return *set;
  }

  fs::path out() const { return m_dir.path() / "out.tachyon"; }

 private:
  TempDir m_dir;
  SessionInfo m_info;
  std::map<uint32_t, RecordBuilder> m_meta, m_data;
};

ConvertOptions rootOptions(uint32_t pid) {
  ConvertOptions o;
  o.rootPids = {pid};
  return o;
}

Id idOf(uint32_t pid, uint32_t tid, uint64_t t, uint32_t cpu = 0) {
  return Id{pid, tid, t, cpu, 0, 0};
}

void addSample(RecordBuilder& b, uint32_t pid, uint32_t tid, uint64_t t, std::vector<uint64_t> chain,
               uint16_t misc = kMiscUser, uint32_t cpu = 0, uint64_t ip = 0) {
  RecordBuilder::Sample s;
  s.misc = misc;
  s.id = idOf(pid, tid, t, cpu);
  s.chain = std::move(chain);
  s.ip = ip;
  if (s.ip == 0) {
    for (uint64_t v : s.chain) {
      if (v < kContextMax) {
        s.ip = v;
        break;
      }
    }
  }
  b.sample(s);
}

void addMmap(RecordBuilder& b, uint32_t pid, uint64_t t, uint64_t addr, uint64_t len, uint64_t pgoff,
             const std::string& path, uint64_t inode, const std::vector<uint8_t>& kernelId = {}) {
  RecordBuilder::Mmap2 m;
  m.pid = pid;
  m.tid = pid;
  m.addr = addr;
  m.len = len;
  m.pgoff = pgoff;
  m.devMajor = 8;
  m.devMinor = 1;
  m.inode = inode;
  m.name = path;
  m.buildId = kernelId;
  m.id = idOf(pid, pid, t);
  b.mmap2(m);
}

struct Loaded {
  trace::TraceReader reader;
  std::vector<trace::ModuleInfo> modules;
  std::vector<trace::ThreadRec> threads;
  std::vector<trace::ProcessRec> processes;
  std::vector<trace::FrameRec> frames;
  std::vector<trace::CallstackNodeRec> nodes;
  trace::SampleBatch samples;
  trace::StringTable strings;
  nlohmann::json info;
};

Loaded load(const fs::path& path) {
  auto r = trace::TraceReader::open(path);
  EXPECT_TRUE(r.ok()) << r.error().toString();
  Loaded l{std::move(*r), {}, {}, {}, {}, {}, {}, {}, {}};
  l.modules = *l.reader.readModuleInfos();
  l.threads = *l.reader.readThreads();
  l.processes = *l.reader.readProcesses();
  l.frames = *l.reader.readFrames();
  l.nodes = *l.reader.readCallstackNodes();
  l.samples = *l.reader.readSamples();
  l.strings = *l.reader.readStrings();
  auto info = l.reader.readCaptureInfo();
  EXPECT_TRUE(info.ok());
  l.info = nlohmann::json::parse(*info);
  return l;
}

// Frames of a callstack, leaf first, as (module, value) pairs.
std::vector<std::pair<uint32_t, uint64_t>> stackOf(const Loaded& l, CallstackIdx leaf) {
  std::vector<std::pair<uint32_t, uint64_t>> out;
  uint32_t node = leaf.get();
  while (node != 0 && node < l.nodes.size()) {
    const trace::FrameRec f = l.frames[l.nodes[node].frame.get()];
    const uint64_t value = f.addr_or_rva;  // packed member: copy before binding a reference
    out.emplace_back(f.module.get(), value);
    node = l.nodes[node].parent.get();
  }
  return out;
}

std::string str(const Loaded& l, StringIdx s) {
  return std::string(l.strings.get(s));
}

std::vector<uint8_t> idBytes(const trace::ModuleInfo& m) {
  return std::vector<uint8_t>(m.id.begin(), m.id.begin() + m.id_len);
}

class ConvertTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // libfoo.so: a PIE shared object. Segments as GNU ld lays them out.
    m_lib = m_files.path() / "libfoo.so";
    ptest::writeElf(
        m_lib, ElfSpec{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
                        0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02, 0x03, 0x04, 0x05},
                       {ElfSegment{0, 0, 0x1000, 0x1000, 4}, ElfSegment{0x1000, 0x1000, 0x2000, 0x2000, 5},
                        ElfSegment{0x4000, 0x3000, 0x800, 0x1800, 6}},
                       3});
    // prog: a non-PIE executable linked at 0x400000.
    m_exe = m_files.path() / "prog";
    ptest::writeElf(m_exe, ElfSpec{{0xAB, 0xCD, 0xEF, 0x01},
                                   {ElfSegment{0x400000, 0, 0x1000, 0x1000, 4},
                                    ElfSegment{0x401000, 0x1000, 0x4000, 0x4000, 5},
                                    ElfSegment{0x406000, 0x5000, 0x100, 0x200, 6}},
                                   2});
  }

  ConvertOptions options(uint32_t pid) {
    ConvertOptions o = rootOptions(pid);
    o.procRoot = m_files.path() / "noproc";
    o.fileMapper = [this](uint32_t, std::string_view recorded) -> fs::path {
      if (recorded == "/test/libfoo.so") return m_lib;
      if (recorded == "/test/prog") return m_exe;
      return fs::path("/does/not/exist") / std::string(recorded);
    };
    return o;
  }

  // The process: fork + exec + executable mapping + library mapping (the library at a random-looking base).
  void launchRecords(SpoolFixture& f, uint32_t pid = 100, uint64_t libBase = 0x7f0000000000ull) {
    f.meta().fork(pid, 1, pid, 1, kStart + 10, idOf(1, 1, kStart + 10));
    f.meta().comm(pid, pid, "prog", true, idOf(pid, pid, kStart + 20));
    addMmap(f.meta(), pid, kStart + 30, 0x401000, 0x4000, 0x1000, "/test/prog", 11);
    addMmap(f.meta(), pid, kStart + 40, libBase + 0x1000, 0x2000, 0x1000, "/test/libfoo.so", 12);
  }

  TempDir m_files;
  fs::path m_lib, m_exe;
};

TEST_F(ConvertTest, ModulesFollowTheElfBaseRule) {
  SpoolFixture f;
  launchRecords(f);
  addSample(f.data(), 100, 100, kStart + 1000,
            {kContextUser, 0x7f0000001234ull, 0x7f0000001500ull, 0x401200ull});
  const SpoolSet set = f.build();
  auto stats = convertSpool(set, f.out(), options(100));
  ASSERT_TRUE(stats.ok()) << stats.error().toString();
  Loaded l = load(f.out());

  ASSERT_EQ(l.modules.size(), 2u);
  const trace::ModuleInfo* lib = nullptr;
  const trace::ModuleInfo* exe = nullptr;
  for (const auto& m : l.modules) {
    if (str(l, m.path) == "/test/libfoo.so") lib = &m;
    if (str(l, m.path) == "/test/prog") exe = &m;
  }
  ASSERT_NE(lib, nullptr);
  ASSERT_NE(exe, nullptr);

  // PIE library: the lowest PT_LOAD (vaddr 0) is mapped at 0x7f0000000000, although only the executable
  // segment (offset 0x1000) was reported by MMAP2.
  EXPECT_EQ(lib->base, 0x7f0000000000ull);
  EXPECT_EQ(lib->size, 0x4000u + 0x1800u)
      << "image size: end of the highest PT_LOAD minus the lowest address";
  EXPECT_EQ(lib->pid, 100u);
  EXPECT_EQ(lib->id_kind, trace::kModuleIdGnuBuildId);
  EXPECT_EQ(lib->id_len, 20);
  EXPECT_EQ(lib->id[0], 0x11);
  EXPECT_EQ(lib->id[19], 0x05);
  // Non-PIE executable: the lowest PT_LOAD is at 0x400000 and the segment it was linked at is where it runs.
  EXPECT_EQ(exe->base, 0x400000u);
  EXPECT_EQ(exe->id_kind, trace::kModuleIdGnuBuildId);
  EXPECT_EQ(idBytes(*exe), (std::vector<uint8_t>{0xAB, 0xCD, 0xEF, 0x01}));
  EXPECT_EQ(lib->load_ts, kStart + 40) << "module load time: the mapping that created it";
  EXPECT_EQ(lib->unload_ts, kEnd) << "still loaded: the trace end";

  // Frames: rva = address - base.
  ASSERT_EQ(l.samples.size(), 1u);
  const auto stack = stackOf(l, l.samples.callstack[0]);
  ASSERT_EQ(stack.size(), 3u);
  const uint32_t libIdx = static_cast<uint32_t>(lib - l.modules.data());
  const uint32_t exeIdx = static_cast<uint32_t>(exe - l.modules.data());
  EXPECT_EQ(stack[0], (std::pair<uint32_t, uint64_t>{libIdx, 0x1234}));
  EXPECT_EQ(stack[1], (std::pair<uint32_t, uint64_t>{libIdx, 0x1500}));
  EXPECT_EQ(stack[2], (std::pair<uint32_t, uint64_t>{exeIdx, 0x1200}));
}

TEST_F(ConvertTest, SegmentsWithDifferentOffsetToVaddrDeltas) {
  // lld-style layout: the executable segment is at vaddr 0x2000 but file offset 0x1000.
  const fs::path lld = m_files.path() / "liblld.so";
  ptest::writeElf(lld,
                  ElfSpec{{1, 2, 3, 4},
                          {ElfSegment{0, 0, 0x1000, 0x1000, 4}, ElfSegment{0x2000, 0x1000, 0x3000, 0x3000, 5},
                           ElfSegment{0x6000, 0x4000, 0x100, 0x100, 6}},
                          3});
  SpoolFixture f;
  f.meta().fork(100, 1, 100, 1, kStart + 1, idOf(1, 1, kStart + 1));
  f.meta().comm(100, 100, "prog", true, idOf(100, 100, kStart + 2));
  addMmap(f.meta(), 100, kStart + 3, 0x7f2200002000ull, 0x3000, 0x1000, "/test/liblld.so", 5);
  addSample(f.data(), 100, 100, kStart + 100, {kContextUser, 0x7f2200002abcull});
  ConvertOptions o = options(100);
  o.fileMapper = [&](uint32_t, std::string_view) { return lld; };
  ASSERT_TRUE(convertSpool(f.build(), f.out(), o).ok());
  Loaded l = load(f.out());
  ASSERT_EQ(l.modules.size(), 1u);
  EXPECT_EQ(l.modules[0].base, 0x7f2200000000ull) << "bias = mapping - (vaddr of the mapped file offset)";
  const auto stack = stackOf(l, l.samples.callstack[0]);
  ASSERT_EQ(stack.size(), 1u);
  EXPECT_EQ(stack[0].second, 0x2abcu);
}

TEST_F(ConvertTest, KernelBuildIdIsUsedWhenTheFileIsGone) {
  SpoolFixture f;
  f.meta().fork(100, 1, 100, 1, kStart + 1, idOf(1, 1, kStart + 1));
  f.meta().comm(100, 100, "prog", true, idOf(100, 100, kStart + 2));
  const std::vector<uint8_t> kid = {9, 8, 7, 6, 5, 4, 3, 2};
  addMmap(f.meta(), 100, kStart + 3, 0x7f0000010000ull, 0x1000, 0, "/gone/libgone.so", 0, kid);
  addSample(f.data(), 100, 100, kStart + 100, {kContextUser, 0x7f0000010040ull});
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  Loaded l = load(f.out());
  ASSERT_EQ(l.modules.size(), 1u);
  EXPECT_EQ(idBytes(l.modules[0]), kid);
  EXPECT_EQ(l.modules[0].base, 0x7f0000010000ull) << "no file to read: base = address - pgoff";
  EXPECT_EQ(stats->modulesFromKernelId, 1u);
  const auto stack = stackOf(l, l.samples.callstack[0]);
  EXPECT_EQ(stack[0].second, 0x40u);
}

TEST_F(ConvertTest, ModuleWithoutAnyIdentityHasNoId) {
  SpoolFixture f;
  f.meta().fork(100, 1, 100, 1, kStart + 1, idOf(1, 1, kStart + 1));
  addMmap(f.meta(), 100, kStart + 3, 0x7f0000010000ull, 0x1000, 0, "/gone/libgone.so", 3);
  addSample(f.data(), 100, 100, kStart + 100, {kContextUser, 0x7f0000010040ull});
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  Loaded l = load(f.out());
  ASSERT_EQ(l.modules.size(), 1u);
  EXPECT_EQ(l.modules[0].id_kind, 0);
  bool warned = false;
  for (const auto& w : stats->warnings) warned |= w.find("no build-id") != std::string::npos;
  EXPECT_TRUE(warned);
}

TEST_F(ConvertTest, TwoSegmentsOfOneLibraryShareOneModuleAndAnonymousMemoryStaysAbsolute) {
  SpoolFixture f;
  launchRecords(f);
  addMmap(f.meta(), 100, kStart + 50, 0x7f0000000000ull, 0x1000, 0, "/test/libfoo.so", 12);  // the R segment
  addSample(f.data(), 100, 100, kStart + 100,
            {kContextUser, 0x7f0000000010ull, 0x7f0000001100ull, 0x7ff000001000ull /* nothing mapped */});
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  Loaded l = load(f.out());
  size_t libs = 0;
  for (const auto& m : l.modules) libs += str(l, m.path) == "/test/libfoo.so" ? 1 : 0;
  EXPECT_EQ(libs, 1u);
  const auto stack = stackOf(l, l.samples.callstack[0]);
  ASSERT_EQ(stack.size(), 3u);
  EXPECT_EQ(stack[0].second, 0x10u);
  EXPECT_EQ(stack[0].first, stack[1].first);
  EXPECT_EQ(stack[2].first, kInvalidIdx) << "unmapped frames keep their absolute address";
  EXPECT_EQ(stack[2].second, 0x7ff000001000ull);
  EXPECT_EQ(stats->framesUnmapped, 1u);
}

TEST_F(ConvertTest, KernelFramesMarkersAndLeafFlag) {
  SpoolFixture f;
  launchRecords(f);
  // A sample taken in the kernel: [KERNEL, k0, k1, USER, u0, u1].
  addSample(
      f.data(), 100, 100, kStart + 100,
      {kContextKernel, kKernelText + 0x10, kKernelText + 0x20, kContextUser, 0x7f0000001010ull, 0x401100ull},
      kMiscKernel, 1);
  // A user sample: [USER, u0].
  addSample(f.data(), 100, 100, kStart + 200, {kContextUser, 0x7f0000001020ull});
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  Loaded l = load(f.out());
  ASSERT_EQ(l.samples.size(), 2u);
  EXPECT_EQ(l.samples.flags[0] & trace::kSampleFlagKernelLeaf, trace::kSampleFlagKernelLeaf);
  EXPECT_EQ(l.samples.flags[1] & trace::kSampleFlagKernelLeaf, 0);
  EXPECT_EQ(l.samples.cpu[0], 1u);
  const auto k = stackOf(l, l.samples.callstack[0]);
  ASSERT_EQ(k.size(), 4u) << "the context markers are stripped";
  EXPECT_EQ(k[0], (std::pair<uint32_t, uint64_t>{kInvalidIdx, kKernelText + 0x10}));
  EXPECT_EQ(k[1], (std::pair<uint32_t, uint64_t>{kInvalidIdx, kKernelText + 0x20}));
  EXPECT_NE(k[2].first, kInvalidIdx) << "user frames below the kernel part are still resolved";
  EXPECT_EQ(k[2].second, 0x1010u);
  EXPECT_EQ(stats->framesKernel, 2u);
}

TEST_F(ConvertTest, KernelSampleWithExcludedKernelCallchainKeepsItsLeaf) {
  SpoolFixture f;
  launchRecords(f);
  // exclude_callchain_kernel: the chain holds only the user part, but the sample was taken in the kernel.
  addSample(f.data(), 100, 100, kStart + 100, {kContextUser, 0x7f0000001010ull, 0x401100ull}, kMiscKernel, 0,
            kKernelText + 0x777);
  ASSERT_TRUE(convertSpool(f.build(), f.out(), options(100)).ok());
  Loaded l = load(f.out());
  ASSERT_EQ(l.samples.size(), 1u);
  EXPECT_EQ(l.samples.flags[0] & trace::kSampleFlagKernelLeaf, trace::kSampleFlagKernelLeaf);
  const auto s = stackOf(l, l.samples.callstack[0]);
  ASSERT_EQ(s.size(), 3u);
  EXPECT_EQ(s[0], (std::pair<uint32_t, uint64_t>{kInvalidIdx, kKernelText + 0x777}))
      << "the ip is the kernel leaf";
  EXPECT_EQ(s[1].second, 0x1010u);
}

TEST_F(ConvertTest, EmptyCallchainFallsBackToTheIp) {
  SpoolFixture f;
  launchRecords(f);
  addSample(f.data(), 100, 100, kStart + 100, {}, kMiscUser, 0, 0x401234);
  ASSERT_TRUE(convertSpool(f.build(), f.out(), options(100)).ok());
  Loaded l = load(f.out());
  const auto s = stackOf(l, l.samples.callstack[0]);
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0].second, 0x1234u);
}

TEST_F(ConvertTest, ProcessesThreadsAndNames) {
  SpoolFixture f;
  launchRecords(f);
  f.meta().fork(100, 100, 101, 100, kStart + 500, idOf(100, 100, kStart + 500));
  f.meta().comm(100, 101, "worker-1", false, idOf(100, 101, kStart + 510));
  addSample(f.data(), 100, 100, kStart + 1000, {kContextUser, 0x401100ull});
  addSample(f.data(), 100, 101, kStart + 1100, {kContextUser, 0x401100ull});
  f.meta().exit(100, 100, 101, 100, kStart + 5000, idOf(100, 101, kStart + 5000));
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  Loaded l = load(f.out());

  ASSERT_EQ(l.processes.size(), 1u);
  EXPECT_EQ(l.processes[0].pid, 100u);
  EXPECT_EQ(l.processes[0].parent_pid, 1u);
  EXPECT_EQ(str(l, l.processes[0].name), "prog");
  EXPECT_EQ(l.processes[0].start_ts, kStart + 10);

  ASSERT_EQ(l.threads.size(), 2u);
  EXPECT_EQ(l.threads[0].tid, 100u);
  EXPECT_EQ(l.threads[1].tid, 101u);
  EXPECT_EQ(str(l, l.threads[1].name), "worker-1");
  EXPECT_EQ(l.threads[1].start_ts, kStart + 500);
  EXPECT_EQ(l.threads[1].end_ts, kStart + 5000);
  EXPECT_EQ(l.threads[0].flags & trace::kThreadFlagTargetProcess, trace::kThreadFlagTargetProcess);
  auto names = l.reader.readThreadNames();
  ASSERT_TRUE(names.ok());
  bool sawWorker = false;
  for (const auto& n : *names)
    sawWorker |= n.tid == 101 && str(l, n.name) == "worker-1" && n.ts == kStart + 510;
  EXPECT_TRUE(sawWorker);

  EXPECT_EQ(l.reader.header().platform, trace::kPlatformLinux);
  EXPECT_EQ(l.reader.header().clock_frequency_hz, 1'000'000'000ull);
  EXPECT_EQ(l.reader.header().start_ts, kStart);
  EXPECT_EQ(l.reader.header().end_ts, kEnd);
}

TEST_F(ConvertTest, OnlyTheTargetProcessIsKeptAndIdleIsDropped) {
  SpoolFixture f;
  launchRecords(f);
  f.meta().fork(200, 1, 200, 1, kStart + 10, idOf(1, 1, kStart + 10));
  f.meta().comm(200, 200, "other", true, idOf(200, 200, kStart + 20));
  addSample(f.data(), 100, 100, kStart + 1000, {kContextUser, 0x401100ull});
  addSample(f.data(), 200, 200, kStart + 1001, {kContextUser, 0x401100ull});
  addSample(f.data(), 0, 0, kStart + 1002, {kContextKernel, kKernelText}, kMiscKernel);
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->samplesRead, 3u);
  EXPECT_EQ(stats->samplesWritten, 1u);
  EXPECT_EQ(stats->samplesDroppedNotTarget, 1u);
  EXPECT_EQ(stats->samplesDroppedIdle, 1u);
  Loaded l = load(f.out());
  ASSERT_EQ(l.threads.size(), 1u);
  EXPECT_EQ(l.threads[0].pid, 100u);
}

TEST_F(ConvertTest, SystemModeKeepsEveryProcessButIdle) {
  SpoolFixture f;
  f.info().mode = "system";
  launchRecords(f);
  f.meta().fork(200, 1, 200, 1, kStart + 10, idOf(1, 1, kStart + 10));
  f.meta().comm(200, 200, "other", true, idOf(200, 200, kStart + 20));
  addSample(f.data(), 100, 100, kStart + 1000, {kContextUser, 0x401100ull});
  addSample(f.data(), 200, 200, kStart + 1001, {kContextUser, 0x401100ull});
  addSample(f.data(), 0, 0, kStart + 1002, {kContextKernel, kKernelText}, kMiscKernel);
  ConvertOptions o = options(100);
  o.rootPids.clear();
  auto stats = convertSpool(f.build(), f.out(), o);
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->samplesWritten, 2u);
  Loaded l = load(f.out());
  EXPECT_EQ(l.processes.size(), 2u);
  for (const auto& t : l.threads)
    EXPECT_EQ(t.flags & trace::kThreadFlagTargetProcess, 0u) << "no target in system mode";
  EXPECT_EQ(l.info["mode"], "system");

  o.keepIdle = true;
  auto withIdle = convertSpool(f.build(), f.dir() / "idle.tachyon", o);
  ASSERT_TRUE(withIdle.ok());
  EXPECT_EQ(withIdle->samplesWritten, 3u);
}

TEST_F(ConvertTest, FollowChildrenAndFilter) {
  SpoolFixture f;
  launchRecords(f);
  // A child that execs a matching program, one that does not match, and a grandchild of the matching one.
  f.meta().fork(110, 100, 110, 100, kStart + 100, idOf(100, 100, kStart + 100));
  f.meta().comm(110, 110, "helper-a", true, idOf(110, 110, kStart + 110));
  f.meta().fork(120, 100, 120, 100, kStart + 120, idOf(100, 100, kStart + 120));
  f.meta().comm(120, 120, "unrelated", true, idOf(120, 120, kStart + 130));
  f.meta().fork(111, 110, 111, 110, kStart + 140, idOf(110, 110, kStart + 140));
  f.meta().comm(111, 111, "helper-b", true, idOf(111, 111, kStart + 150));
  f.meta().fork(121, 120, 121, 120, kStart + 160, idOf(120, 120, kStart + 160));
  f.meta().comm(121, 121, "helper-c", true, idOf(121, 121, kStart + 170));
  for (uint32_t pid : {100u, 110u, 120u, 111u, 121u})
    addSample(f.data(), pid, pid, kStart + 1000 + pid, {kContextUser, 0x401100ull});

  ConvertOptions follow = options(100);
  follow.followChildren = true;
  auto all = convertSpool(f.build(), f.dir() / "all.tachyon", follow);
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all->samplesWritten, 5u) << "no filter: every descendant";

  follow.childFilter = "helper-*";
  auto filtered = convertSpool(f.build(), f.dir() / "filtered.tachyon", follow);
  ASSERT_TRUE(filtered.ok());
  Loaded l = load(f.dir() / "filtered.tachyon");
  std::set<uint32_t> pids;
  for (const auto& t : l.threads) pids.insert(t.pid);
  EXPECT_EQ(pids, (std::set<uint32_t>{100, 110, 111}))
      << "the unmatched child and everything below it are not followed";
  EXPECT_EQ(filtered->samplesWritten, 3u);

  ConvertOptions nofollow = options(100);
  auto only = convertSpool(f.build(), f.dir() / "only.tachyon", nofollow);
  ASSERT_TRUE(only.ok());
  EXPECT_EQ(only->samplesWritten, 1u);
}

TEST_F(ConvertTest, ExecReplacesTheAddressSpace) {
  SpoolFixture f;
  launchRecords(f);
  addSample(f.data(), 100, 100, kStart + 1000, {kContextUser, 0x7f0000001234ull});
  f.meta().comm(100, 100, "newprog", true, idOf(100, 100, kStart + 2000));  // exec: the old mappings are gone
  addSample(f.data(), 100, 100, kStart + 3000, {kContextUser, 0x7f0000001234ull});
  ASSERT_TRUE(convertSpool(f.build(), f.out(), options(100)).ok());
  Loaded l = load(f.out());
  ASSERT_EQ(l.samples.size(), 2u);
  EXPECT_NE(stackOf(l, l.samples.callstack[0])[0].first, kInvalidIdx);
  EXPECT_EQ(stackOf(l, l.samples.callstack[1])[0].first, kInvalidIdx) << "unmapped after the exec";
  // The old module was unloaded at the exec.
  bool checked = false;
  for (const auto& m : l.modules)
    if (str(l, m.path) == "/test/libfoo.so") {
      EXPECT_EQ(m.unload_ts, kStart + 2000);
      checked = true;
    }
  EXPECT_TRUE(checked);
  EXPECT_EQ(str(l, l.processes[0].name), "newprog");
}

TEST_F(ConvertTest, ForkSharesTheParentsMappingsCopyOnWrite) {
  SpoolFixture f;
  launchRecords(f);
  f.meta().fork(130, 100, 130, 100, kStart + 100, idOf(100, 100, kStart + 100));  // inherits the mappings
  addMmap(f.meta(), 130, kStart + 200, 0x7f1111000000ull, 0x1000, 0, "/test/libfoo.so", 12);  // diverges
  addSample(f.data(), 130, 130, kStart + 300, {kContextUser, 0x7f0000001234ull});
  addSample(f.data(), 100, 100, kStart + 301, {kContextUser, 0x7f1111000010ull});  // the parent never saw it
  ConvertOptions o = options(100);
  o.followChildren = true;
  ASSERT_TRUE(convertSpool(f.build(), f.out(), o).ok());
  Loaded l = load(f.out());
  ASSERT_EQ(l.samples.size(), 2u);
  EXPECT_NE(stackOf(l, l.samples.callstack[0])[0].first, kInvalidIdx)
      << "the child sees the inherited library";
  EXPECT_EQ(stackOf(l, l.samples.callstack[1])[0].first, kInvalidIdx)
      << "the parent does not see the child's mapping";
}

TEST_F(ConvertTest, RundownRecordsAreAppliedFirstAndDuplicatedForksIgnored) {
  SpoolFixture f;
  f.info().mode = "attach";
  // Rundown (ring kSpoolRingRundown): the process existed before the capture. The child comes before its
  // parent in pid order.
  RecordBuilder& rd = f.meta(kSpoolRingRundown);
  rd.fork(300, 400, 300, 400, kStart, idOf(300, 300, kStart));
  rd.fork(400, 1, 400, 1, kStart, idOf(400, 400, kStart));
  rd.fork(400, 400, 401, 400, kStart, idOf(400, 401, kStart));
  rd.comm(300, 300, "kid", false, idOf(300, 300, kStart));
  rd.comm(400, 400, "root", false, idOf(400, 400, kStart));
  rd.comm(400, 401, "root-thread", false, idOf(400, 401, kStart));
  addMmap(rd, 400, kStart, 0x401000, 0x4000, 0x1000, "/test/prog", 11);
  // The same fork seen live as well (the enable preceded the rundown).
  f.meta().fork(400, 1, 400, 1, kStart + 1, idOf(1, 1, kStart + 1));
  f.meta().fork(400, 400, 401, 400, kStart + 1, idOf(400, 400, kStart + 1));
  addSample(f.data(), 400, 401, kStart + 1000, {kContextUser, 0x401234ull});
  addSample(f.data(), 300, 300, kStart + 1001, {kContextUser, 0x401234ull});
  ConvertOptions o = options(400);
  o.followChildren = true;
  auto stats = convertSpool(f.build(), f.out(), o);
  ASSERT_TRUE(stats.ok());
  Loaded l = load(f.out());
  EXPECT_EQ(stats->samplesWritten, 2u)
      << "the child created before its parent in the rundown is still followed";
  size_t t401 = 0;
  for (const auto& t : l.threads) t401 += t.tid == 401 ? 1 : 0;
  EXPECT_EQ(t401, 1u) << "no duplicated thread";
  ASSERT_EQ(l.modules.size(), 1u);
  EXPECT_EQ(l.modules[0].base, 0x400000u);
  for (const auto& t : l.threads) {
    if (t.tid == 401) {
      EXPECT_EQ(str(l, t.name), "root-thread");
      EXPECT_EQ(t.start_ts, kStart) << "a thread known from the rundown existed before the trace";
    }
  }
}

TEST_F(ConvertTest, LostAndThrottleAreCountedAndExplained) {
  SpoolFixture f;
  launchRecords(f);
  f.meta().lost(1, 40, idOf(0, 0, kStart + 2000, 1));
  f.meta().lostSamples(2, idOf(0, 0, kStart + 2100, 1));
  f.meta().throttle(false, kStart + 3000, 1, 1, idOf(0, 0, kStart + 3000, 2));
  f.meta().throttle(true, kStart + 4500, 1, 1, idOf(0, 0, kStart + 4500, 2));
  f.meta().throttle(false, kEnd - 1000, 1, 1, idOf(0, 0, kEnd - 1000, 3));  // never unthrottled
  addSample(f.data(), 100, 100, kStart + 1000, {kContextUser, 0x401100ull});
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->eventsLost, 42u);
  EXPECT_EQ(stats->throttleEvents, 2u);
  EXPECT_EQ(stats->throttledNs, 1500u + 1000u) << "open throttles last until the end";
  Loaded l = load(f.out());
  EXPECT_EQ(l.info["events_lost"], 42);
  EXPECT_EQ(l.info["throttle"]["events"], 2);
  std::string warnings = l.info["convert_stats"]["warnings"].dump();
  EXPECT_NE(warnings.find("kernel.perf_event_max_sample_rate"), std::string::npos) << warnings;
  EXPECT_NE(warnings.find("kernel.perf_cpu_time_max_percent"), std::string::npos);
  EXPECT_NE(warnings.find("PERF_RECORD_LOST"), std::string::npos);
  auto model = model::TraceModel::load(f.out());
  ASSERT_TRUE(model.ok());
  EXPECT_EQ(model->captureInfo.events_lost, 42u);
  EXPECT_EQ(model->captureInfo.sampling_hz, 8000u);
}

TEST_F(ConvertTest, SamplesAreSortedAcrossRingsAndChunks) {
  SpoolFixture f;
  launchRecords(f);
  // Two rings interleaved in time, plus a little disorder inside each ring; > 262144 samples so that the
  // sink flushes several chunks.
  std::mt19937 rng(7);
  const int perRing = 200000;
  for (uint32_t ring = 0; ring < 2; ++ring) {
    for (int i = 0; i < perRing; ++i) {
      uint64_t t = kStart + 1000 + static_cast<uint64_t>(i) * 10 + ring * 5;
      if (rng() % 50 == 0 && t > 40) t -= 30;  // out of order by a few samples
      addSample(f.data(ring), 100, 100, t, {kContextUser, 0x401100ull});
    }
  }
  auto stats = convertSpool(f.build(), f.out(), options(100));
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->samplesWritten, 400000u);
  Loaded l = load(f.out());
  ASSERT_EQ(l.samples.size(), 400000u);
  EXPECT_TRUE(std::is_sorted(l.samples.ts.begin(), l.samples.ts.end())) << "output is ordered across chunks";
  size_t sampleChunks = 0;
  for (const auto& c : l.reader.chunks())
    sampleChunks += c.type == static_cast<uint32_t>(trace::ChunkType::Samples) ? 1 : 0;
  EXPECT_GE(sampleChunks, 2u);
}

TEST_F(ConvertTest, WindowStartDropsOlderSamplesButKeepsHistory) {
  SpoolFixture f;
  launchRecords(f);
  addSample(f.data(), 100, 100, kStart + 1000, {kContextUser, 0x7f0000001234ull});
  addSample(f.data(), 100, 100, kStart + 5000, {kContextUser, 0x7f0000001234ull});
  ConvertOptions o = options(100);
  o.windowStartTs = kStart + 2000;
  o.endTs = kStart + 6000;
  auto stats = convertSpool(f.build(), f.out(), o);
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->samplesWritten, 1u);
  EXPECT_EQ(stats->samplesDroppedWindow, 1u);
  Loaded l = load(f.out());
  EXPECT_EQ(l.modules.size(), 1u) << "mappings made before the window are still known";
  EXPECT_EQ(l.reader.header().end_ts, kStart + 6000);
}

TEST_F(ConvertTest, UncleanSpoolIsConvertedWithAWarning) {
  SpoolFixture f;
  launchRecords(f);
  addSample(f.data(), 100, 100, kStart + 1000, {kContextUser, 0x401100ull});
  f.info().finished = false;
  f.info().endTs = 0;  // a crash: no end time
  SpoolSet set = f.build();
  fs::resize_file(set.data[0], fs::file_size(set.data[0]) - 3);  // and a cut-off last record
  auto stats = convertSpool(set, f.out(), options(100));
  ASSERT_TRUE(stats.ok()) << stats.error().toString();
  bool unclean = false, truncated = false;
  for (const auto& w : stats->warnings) {
    unclean |= w.find("did not stop cleanly") != std::string::npos;
    truncated |= w.find("truncated") != std::string::npos;
  }
  EXPECT_TRUE(unclean);
  EXPECT_TRUE(truncated);
  EXPECT_GT(stats->endTs, stats->startTs) << "the end time falls back to the last record";
  auto model = model::TraceModel::load(f.out());
  EXPECT_TRUE(model.ok());
}

TEST_F(ConvertTest, LoadsInTheModelLoaderWithConsistentIds) {
  SpoolFixture f;
  launchRecords(f);
  for (int i = 0; i < 50; ++i)
    addSample(f.data(), 100, 100, kStart + 1000 + static_cast<uint64_t>(i) * 125,
              {kContextUser, 0x7f0000001000ull + static_cast<uint64_t>(i), 0x401100ull});
  ASSERT_TRUE(convertSpool(f.build(), f.out(), options(100)).ok());
  auto model = model::TraceModel::load(f.out());
  ASSERT_TRUE(model.ok()) << model.error().toString();
  EXPECT_FALSE(model->isPartial);
  ASSERT_EQ(model->threads.size(), 1u);
  EXPECT_TRUE(model->threads[0].is_target);
  EXPECT_EQ(model->samplesByThread[0].ts.size(), 50u);
  ASSERT_EQ(model->modules.size(), 2u);
  for (const auto& m : model->modules) {
    EXPECT_EQ(m.id.kind, trace::kModuleIdGnuBuildId);
    EXPECT_GT(m.size, 0u);
  }
  for (const auto& fr : model->frames) {
    if (fr.module.valid()) EXPECT_LT(fr.module.get(), model->modules.size());
  }
}

TEST_F(ConvertTest, RejectsLayoutsItCannotOrder) {
  SpoolFixture f;
  f.info().layout = RecordLayout{kSampleIp | kSampleCallchain, true};  // no TIME
  EXPECT_EQ(convertSpool(f.build(), f.out(), options(100)).error().code, ErrorCode::Unsupported);
}

}  // namespace
}  // namespace tachyon::capture::perf
