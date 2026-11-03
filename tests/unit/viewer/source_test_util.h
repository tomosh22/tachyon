// Shared helpers of the WP-45 tests (Source panel): temp trees with source files, and a writer for small
// traces whose only module is a real binary (the testbed) so that symbols, source lines and code bytes are
// all real. Header-only; also used by the Windows test.
#pragma once

#include "trace/trace_writer.h"
#include "viewer/app.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <thread>
#include <vector>

namespace tachyon::viewer::source_test {

namespace fs = std::filesystem;

// A fresh directory under the build tree, removed with the object.
class TempTree {
 public:
  explicit TempTree(const std::string& name) {
    // ctest runs the tests of one binary as parallel processes: the clock makes the directory unique.
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    m_root = fs::path(TACHYON_BINARY_DIR) / "viewer_source_tmp" /
             (name + "_" + std::to_string(stamp) + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(m_root, ec);
    fs::create_directories(m_root, ec);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(m_root, ec);
  }
  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;
  const fs::path& root() const { return m_root; }
  fs::path write(const std::string& relative, const std::string& content) const {
    const fs::path p = m_root / relative;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary) << content;
    return p;
  }

 private:
  fs::path m_root;
};

// One stack of the test trace: frame RVAs of the single module, leaf first, and how many samples have it.
struct StackSpec {
  std::vector<uint64_t> leafToRoot;
  uint32_t count = 1;
};

constexpr uint64_t kSampleStepTicks = 100'000;  // 100 us at the 1 GHz test clock

// The module of the test trace: its image file and identity record.
struct ModuleSpec {
  std::string path;
  uint8_t idKind = 0;  // trace::kModuleIdPdb / kModuleIdGnuBuildId, 0 = none
  std::vector<uint8_t> id;
  uint32_t age = 0;
};

// Writes a trace with one process, one thread that is Running for the whole trace, the module and the
// samples (interleaved round-robin over `stacks` so that every stack is spread over the time range).
inline void writeModuleTrace(const fs::path& path, const ModuleSpec& module,
                             const std::vector<StackSpec>& stacks) {
  using namespace trace;
  FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = 1000;
  uint64_t total = 0;
  for (const StackSpec& s : stacks) total += s.count;
  h.end_ts = h.start_ts + 2000 + (total + 2) * kSampleStepTicks;
#ifdef _WIN32
  h.platform = kPlatformWindows;
#else
  h.platform = kPlatformLinux;
#endif
  auto w = TraceWriter::open(path, h);
  ASSERT_TRUE(w.ok());
  const uint32_t tid = 4321;
  const StringIdx app = w->internString("testbed");
  ProcessRec proc{};
  proc.pid = tid;
  proc.parent_pid = 1;
  proc.start_ts = h.start_ts;
  proc.end_ts = h.end_ts;
  proc.name = app;
  proc.command_line = app;
  w->append(std::span<const ProcessRec>(&proc, 1));
  ThreadRec th{};
  th.tid = tid;
  th.pid = tid;
  th.start_ts = h.start_ts;
  th.end_ts = h.end_ts;
  th.name = w->internString("main");
  th.flags = kThreadFlagTargetProcess;
  w->append(std::span<const ThreadRec>(&th, 1));
  alignas(8) ModuleRec m;
  std::memset(static_cast<void*>(&m), 0, sizeof(m));
  m.pid = tid;
  m.base = 0x555555554000ull;
  m.size = 0x1000000;
  m.path = w->internString(module.path);
  m.id_kind = module.idKind;
  m.id_len = static_cast<uint8_t>(std::min<size_t>(module.id.size(), sizeof(m.id)));
  std::memcpy(m.id, module.id.data(), m.id_len);
  m.age = module.age;
  m.load_ts = h.start_ts;
  m.unload_ts = h.end_ts;
  w->append(std::span<const ModuleRec>(&m, 1));

  // Running from the start: the first switch-in makes every later sample weigh one sampling interval.
  ContextSwitchRec in{};
  in.ts = h.start_ts + 1;
  in.cpu = 0;
  in.old_tid = 0;
  in.new_tid = tid;
  in.old_thread_stack = CallstackIdx(0);
  w->append(std::span<const ContextSwitchRec>(&in, 1));

  std::vector<CallstackIdx> callstacks;
  for (const StackSpec& s : stacks) {
    std::vector<FrameIdx> frames;
    for (const uint64_t rva : s.leafToRoot) frames.push_back(w->internFrame(ModuleIdx(0), rva));
    callstacks.push_back(w->internCallstack(std::span<const FrameIdx>(frames.data(), frames.size())));
  }
  std::vector<uint32_t> left;
  for (const StackSpec& s : stacks) left.push_back(s.count);
  SampleBatch samples;
  uint64_t ts = h.start_ts + 1000;
  bool any = true;
  while (any) {
    any = false;
    for (size_t i = 0; i < stacks.size(); ++i) {
      if (left[i] == 0) continue;
      --left[i];
      any = true;
      samples.push(ts, tid, callstacks[i], 0);
      ts += kSampleStepTicks;
    }
  }
  w->append(samples);
  ASSERT_TRUE(w->finish().ok());
}

inline AppOptions headlessOptions(const char* tag = "source") {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.configPath =
      fs::path(TACHYON_BINARY_DIR) / (std::string("viewer_") + tag + "_test_no_such_settings.json");
  return o;
}

}  // namespace tachyon::viewer::source_test
