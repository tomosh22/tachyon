// Windows-only: records ~3 s of a real kernel ETW session while tachyon-testbed runs its "spin" scenario,
// converts the ETL with EtlConverter::convert and checks the resulting .tachyon file. Skipped when the
// process cannot start a kernel session (not elevated).
//
// hot_a / hot_b / hot_c ranking: the testbed exe is mapped through DbgHelp (SymFromAddr on the PDB that sits
// next to the exe) to name the leaf function of every sample. This needs no new dependency (dbghelp ships
// with Windows) and uses the real PDB, so it also verifies the RVAs we write. If the PDB cannot be loaded the
// ranking check is skipped with a message instead of failing; DIA-based symbolization is a later work
// package.
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// dbghelp.h must come after windows.h.
#include <dbghelp.h>

#include "cli.h"
#include "etlconvert/etl_converter.h"
#include "etlconvert/pe_info.h"
#include "etlconvert/stack_matcher.h"
#include "etw_platform.h"
#include "etw_stats.h"
#include "report.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tachyon::etlconvert {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Resolves RVAs of one image to function names through DbgHelp.
class PdbNames {
 public:
  explicit PdbNames(const std::filesystem::path& exe) {
    m_process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(m_process, nullptr, FALSE)) return;
    m_initialized = true;
    const std::wstring wide = exe.wstring();
    m_base = SymLoadModuleExW(m_process, nullptr, wide.c_str(), nullptr, kFakeBase, 0, nullptr, 0);
  }
  ~PdbNames() {
    if (m_initialized) SymCleanup(m_process);
  }
  PdbNames(const PdbNames&) = delete;
  PdbNames& operator=(const PdbNames&) = delete;

  bool usable() const { return m_initialized && m_base != 0; }

  std::string nameOf(uint64_t rva) const {
    if (!usable()) return {};
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (!SymFromAddr(m_process, m_base + rva, &displacement, sym)) return {};
    return std::string(sym->Name, sym->NameLen);
  }

 private:
  static constexpr DWORD64 kFakeBase = 0x10000000;
  HANDLE m_process = nullptr;
  bool m_initialized = false;
  DWORD64 m_base = 0;
};

// CI hides the stdout of passing tests, so the key numbers go to stderr, to a file in the build directory and
// to the gtest XML (RecordProperty "summary").
class Tee {
 public:
  template <typename T>
  Tee& operator<<(const T& value) {
    std::ostringstream one;
    one << value;
    std::fprintf(stderr, "%s", one.str().c_str());
    m_text += one.str();
    return *this;
  }
  const std::string& text() const { return m_text; }

 private:
  std::string m_text;
};

TEST(EtlConvertWindows, ConvertsRealTestbedEtl) {
  Tee out;
  const std::filesystem::path exe = TACHYON_TESTBED_EXE;
  ASSERT_TRUE(std::filesystem::exists(exe)) << exe;
  const std::filesystem::path etl = std::filesystem::path(TACHYON_BINARY_DIR) / "etlconvert_real.etl";
  const std::filesystem::path outFile = std::filesystem::path(TACHYON_BINARY_DIR) / "etlconvert_real.tachyon";
  std::error_code ec;
  std::filesystem::remove(etl, ec);
  std::filesystem::remove(outFile, ec);

  etlstat::CliOptions rec;
  rec.command = etlstat::Command::Record;
  rec.outPath = etl.string();
  rec.seconds = 10;  // upper bound: recording ends when the testbed exits
  rec.hz = 8000;
  rec.exePath = exe.string();
  rec.exeArgs = "--scenario spin --duration 2";
  auto recorded = etlstat::recordEtl(rec);
  if (!recorded.ok() && recorded.error().code == ErrorCode::NoPrivilege) {
    GTEST_SKIP() << "not elevated: " << recorded.error().message;
  }
  ASSERT_TRUE(recorded.ok()) << recorded.error().toString();
  const uint32_t pid = recorded->exePid;
  ASSERT_NE(pid, 0u);
  out << "[etlconvert] recorded pid " << pid << ", eventsLost " << recorded->eventsLost << ", "
      << recorded->fileBytes << " bytes\n";

  ConvertOptions options;
  options.targetPids = {pid};
  options.samplingHz = 8000;
  options.eventsLost = recorded->eventsLost;
  options.agentVersion = "etlconvert-windows-test";
  float lastProgress = 0;
  bool progressMonotonic = true;
  options.progress = [&](float p) {
    progressMonotonic &= p >= lastProgress;
    lastProgress = p;
  };
  auto converted = EtlConverter::convert(etl, outFile, options);
  ASSERT_TRUE(converted.ok()) << converted.error().toString();
  const ConvertStats& st = *converted;
  EXPECT_TRUE(progressMonotonic);
  EXPECT_FLOAT_EQ(lastProgress, 1.0f);

  out << "[etlconvert] events " << st.eventsTotal << " (samples " << st.sampledProfileEvents << ", cswitch "
      << st.cswitchEvents << ", ready " << st.readyEvents << ", stackwalk " << st.stackWalkEvents
      << "); written: samples " << st.samplesWritten << ", cswitch " << st.cswitchesWritten << ", ready "
      << st.readiesWritten << "; stacks matched " << st.stacksMatched << " unmatched " << st.stacksUnmatched
      << " two-part " << st.stacksTwoPart << "; modules rsds " << st.modulesRsds << " fallback "
      << st.modulesFallback << " (missing " << st.modulesFileMissing << ", differ " << st.modulesMismatch
      << ", no codeview " << st.modulesNoCodeView << "); " << st.seconds << " s\n";
  for (size_t k = 0; k < 3; ++k) {
    out << "[etlconvert] stack match roles for " << parentKindName(static_cast<ParentKind>(k)) << ":";
    for (size_t r = 0; r < 3; ++r)
      out << ' ' << matchRoleName(static_cast<MatchRole>(r)) << '=' << st.matchRole[k][r];
    out << " (with stack " << st.parentsWithStack[k] << ", without " << st.parentsWithoutStack[k] << ")\n";
  }
  for (const std::string& w : st.warnings) out << "[etlconvert] warning: " << w << "\n";

  auto reader = trace::TraceReader::open(outFile);
  ASSERT_TRUE(reader.ok()) << reader.error().toString();
  EXPECT_EQ(reader->header().platform, trace::kPlatformWindows);
  EXPECT_EQ(reader->header().flags & trace::kFlagPartial, 0u);
  auto strings = reader->readStrings();
  auto threads = reader->readThreads();
  auto modules = reader->readModuleInfos();
  auto samples = reader->readSamples();
  auto switches = reader->readContextSwitches();
  auto nodes = reader->readCallstackNodes();
  auto frames = reader->readFrames();
  auto info = reader->readCaptureInfo();
  ASSERT_TRUE(strings.ok() && threads.ok() && modules.ok() && samples.ok() && switches.ok() && nodes.ok() &&
              frames.ok() && info.ok());
  EXPECT_NE(info->find("\"sampling_hz\""), std::string::npos);

  // Samples exist for the testbed pid, and only target threads were kept.
  std::set<uint32_t> targetTids;
  for (const trace::ThreadRec& t : *threads) {
    if (t.pid == pid && (t.flags & trace::kThreadFlagTargetProcess)) targetTids.insert(t.tid);
  }
  ASSERT_GE(targetTids.size(), 2u) << "the testbed has a main thread and a worker";
  ASSERT_GT(samples->size(), 500u) << "2 s at 8 kHz should give thousands of samples";
  std::map<uint32_t, uint64_t> samplesPerTid;
  for (uint32_t tid : samples->tid) {
    EXPECT_TRUE(targetTids.count(tid) != 0) << "sample of non-target thread " << tid;
    ++samplesPerTid[tid];
  }

  // Sample counts must agree with etlstat's analysis of the same file.
  etlstat::StatsAccumulator acc;
  etlstat::TraceFileInfo finfo;
  auto consumed = etlstat::consumeEtl(etl.string(), acc, finfo, nullptr, nullptr);
  ASSERT_TRUE(consumed.ok()) << consumed.error().toString();
  acc.finish();
  uint64_t etlstatSamples = 0;
  for (const auto& [tid, n] : acc.snapshot().samplesPerThread) {
    if (targetTids.count(tid) != 0) etlstatSamples += n;
  }
  EXPECT_EQ(samples->size(), etlstatSamples) << "converter and etlstat disagree on the testbed sample count";
  EXPECT_EQ(st.samplesWritten, samples->size());
  EXPECT_EQ(reader->header().clock_frequency_hz, finfo.perfFreq);

  // Every testbed thread that ran has context switches.
  std::set<uint32_t> switched;
  for (const trace::ContextSwitchRec& c : *switches) {
    switched.insert(c.new_tid);
    switched.insert(c.old_tid);
  }
  for (const auto& [tid, n] : samplesPerTid) {
    EXPECT_TRUE(switched.count(tid) != 0)
        << "thread " << tid << " has " << n << " samples but no context switch";
  }

  // At least one module is identified by PDB GUID; the testbed exe matches its own PE header.
  const trace::StringTable& strs = *strings;
  uint32_t rsds = 0;
  const trace::ModuleInfo* testbedModule = nullptr;
  uint32_t testbedModuleIdx = kInvalidIdx;
  for (size_t i = 0; i < modules->size(); ++i) {
    const trace::ModuleInfo& m = (*modules)[i];
    if (m.id_kind == trace::kModuleIdPdb) ++rsds;
    if (m.pid == pid && endsWith(lower(std::string(strs.get(m.path))), "tachyon-testbed.exe")) {
      testbedModule = &m;
      testbedModuleIdx = static_cast<uint32_t>(i);
    }
  }
  EXPECT_GE(rsds, 1u);
  ASSERT_NE(testbedModule, nullptr) << "testbed exe not among the modules";
  auto pe = parsePeFile(exe);
  ASSERT_TRUE(pe.ok()) << pe.error().toString();
  if (pe->hasCodeView) {
    EXPECT_EQ(testbedModule->id_kind, trace::kModuleIdPdb);
    EXPECT_EQ(std::memcmp(testbedModule->id.data(), pe->guid, 16), 0);
    EXPECT_EQ(testbedModule->age, pe->age);
  }

  // Leaf functions of the testbed exe, by symbol name.
  std::map<std::string, uint64_t> byName;
  uint64_t inExe = 0;
  PdbNames names(exe);
  for (size_t i = 0; i < samples->size(); ++i) {
    uint32_t node = samples->callstack[i].get();
    if (node == 0) continue;
    const trace::FrameRec& leaf = (*frames)[(*nodes)[node].frame.get()];
    if (leaf.module.get() != testbedModuleIdx) continue;
    ++inExe;
    byName[names.nameOf(leaf.addr_or_rva)]++;
  }
  out << "[etlconvert] leaf samples in tachyon-testbed.exe: " << inExe << " of " << samples->size() << "\n";
  EXPECT_GT(inExe, samples->size() / 4);
  std::vector<std::pair<uint64_t, std::string>> ranked;
  for (const auto& [name, n] : byName) ranked.emplace_back(n, name);
  std::sort(ranked.rbegin(), ranked.rend());
  for (size_t i = 0; i < ranked.size() && i < 8; ++i)
    out << "[etlconvert]   " << ranked[i].first << "  "
        << (ranked[i].second.empty() ? "<no symbol>" : ranked[i].second) << "\n";

  auto countOf = [&](const char* needle) -> uint64_t {
    uint64_t total = 0;
    for (const auto& [name, n] : byName) {
      if (name.find(needle) != std::string::npos) total += n;
    }
    return total;
  };
  const uint64_t a = countOf("hot_a");
  const uint64_t b = countOf("hot_b");
  const uint64_t c = countOf("hot_c");
  if (!names.usable() || a + b + c == 0) {
    out << "[etlconvert] hot_a/hot_b/hot_c ranking skipped: PDB symbols not available next to the exe\n";
  } else {
    out << "[etlconvert] hot_a=" << a << " hot_b=" << b << " hot_c=" << c << "\n";
    EXPECT_GT(a, b);
    EXPECT_GT(b, c);
  }

  RecordProperty("summary", out.text());
  std::ofstream(std::filesystem::path(TACHYON_BINARY_DIR) / "etlconvert_real_summary.txt") << out.text();
  std::filesystem::remove(etl, ec);
  std::filesystem::remove(outFile, ec);
}

}  // namespace
}  // namespace tachyon::etlconvert

#endif  // TACHYON_PLATFORM_WINDOWS
