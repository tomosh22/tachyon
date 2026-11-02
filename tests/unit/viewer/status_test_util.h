// Shared helpers of the WP-48 tests (status bar, banner, Symbols panel, toasts, cheat sheet).
#pragma once

#include "symbols/json_symbol_provider.h"
#include "trace/trace_writer.h"
#include "viewer/app.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <thread>

namespace tachyon::viewer::status_test {

namespace fs = std::filesystem;

inline fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

// A fresh empty directory under the build tree, removed with the object.
class TempDir {
 public:
  explicit TempDir(const std::string& name) {
    static std::atomic<int> counter{0};
    m_path = fs::path(TACHYON_BINARY_DIR) / "viewer_status_tmp" / (name + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(m_path, ec);
    fs::create_directories(m_path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(m_path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const fs::path& path() const { return m_path; }

 private:
  fs::path m_path;
};

inline AppOptions headlessOptions(const char* tag = "status") {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.configPath =
      fs::path(TACHYON_BINARY_DIR) / (std::string("viewer_") + tag + "_test_no_such_settings.json");
  return o;
}

// Makes every provider load take `delayMs` and runs the resolution on one worker, so progress is observable.
inline std::function<void(symbols::SymbolService&)> slowProviders(int delayMs) {
  return [delayMs](symbols::SymbolService& service) {
    service.setWorkerThreads(1);
    symbols::SymbolService* svc = &service;
    service.setProviderFactory(
        [svc, delayMs](const model::Module&,
                       const fs::path& file) -> std::unique_ptr<symbols::ISymbolProvider> {
          std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
          if (file.extension() == ".json")
            return std::make_unique<symbols::JsonSymbolProvider>(svc->stringSink());
          return nullptr;
        });
  };
}

class Harness {
 public:
  explicit Harness(AppOptions options = headlessOptions()) : app(makeNullRenderer(), std::move(options)) {
    std::string error;
    EXPECT_TRUE(app.init(error)) << error;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigInputTrickleEventQueue = false;
  }

  bool until(const std::function<bool()>& pred, int seconds = 60) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  void frames(int n = 1) {
    for (int i = 0; i < n; ++i) app.frame(0);
  }
  bool open(const fs::path& path) {
    app.openFile(path);
    return until([&] { return !app.loading(); }) && app.document() != nullptr;
  }
  bool symbolsDone() {
    if (!app.document() || !app.document()->symbols()) return false;
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    return p.total > 0 && p.done == p.total;
  }
  void key(ImGuiKey k) {
    ImGui::GetIO().AddKeyEvent(k, true);
    frames(1);
    ImGui::GetIO().AddKeyEvent(k, false);
    frames(1);
  }
  void click(float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frames(1);
    io.AddMouseButtonEvent(0, true);
    frames(1);
    io.AddMouseButtonEvent(0, false);
    frames(2);
  }

  App app;
};

// Writes a small sampled trace with the given CaptureInfo JSON (skipped when empty). `finish` false leaves a
// partial file. `droppedMarkers` adds that many "tachyon.dropped" markers.
inline void writeTrace(const fs::path& path, const std::string& captureInfo, bool finish = true,
                       uint32_t droppedMarkers = 0) {
  using namespace trace;
  FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = 1000;
  h.end_ts = 2'000'000;
  h.platform = kPlatformLinux;
  auto w = TraceWriter::open(path, h);
  ASSERT_TRUE(w.ok());
  const StringIdx app = w->internString("app.exe");
  const StringIdx lib = w->internString("libfoo.so");
  const StringIdx thread = w->internString("main-thread");
  ProcessRec proc{};
  proc.pid = 1234;
  proc.parent_pid = 1;
  proc.start_ts = h.start_ts;
  proc.end_ts = h.end_ts;
  proc.name = app;
  proc.command_line = app;
  w->append(std::span<const ProcessRec>(&proc, 1));
  ThreadRec th{};
  th.tid = 1234;
  th.pid = 1234;
  th.start_ts = h.start_ts;
  th.end_ts = h.end_ts;
  th.name = thread;
  th.flags = kThreadFlagTargetProcess;
  w->append(std::span<const ThreadRec>(&th, 1));
  alignas(8) std::byte moduleBytes[2 * sizeof(ModuleRec)];
  auto putModule = [&](size_t index, uint64_t base, StringIdx path, uint8_t idByte) {
    alignas(8) ModuleRec m;
    std::memset(static_cast<void*>(&m), 0, sizeof(m));
    m.pid = 1234;
    m.base = base;
    m.size = 0x10000;
    m.path = path;
    m.id_kind = kModuleIdGnuBuildId;
    m.id_len = 4;
    m.id[0] = idByte;
    m.load_ts = h.start_ts;
    m.unload_ts = h.end_ts;
    std::memcpy(moduleBytes + index * sizeof(ModuleRec), &m, sizeof(m));
  };
  putModule(0, 0x400000, app, 0xAB);
  putModule(1, 0x7f0000, lib, 0xCD);
  w->append(std::span<const ModuleRec>(reinterpret_cast<const ModuleRec*>(moduleBytes), 2));
  const FrameIdx f0 = w->internFrame(ModuleIdx(0), 0x100);
  const FrameIdx f1 = w->internFrame(ModuleIdx(1), 0x80);
  const FrameIdx stack[2] = {f1, f0};
  const CallstackIdx leaf = w->internCallstack(std::span<const FrameIdx>(stack, 2));
  SampleBatch samples;
  for (uint64_t i = 0; i < 50; ++i)
    samples.push(h.start_ts + i * 10, 1234, leaf, static_cast<uint16_t>(i % 4));
  w->append(samples);
  if (droppedMarkers > 0) {
    const StringIdx name = w->internString("tachyon.dropped");
    std::vector<MarkerRec> markers(droppedMarkers);
    for (uint32_t i = 0; i < droppedMarkers; ++i) {
      markers[i] = MarkerRec{};
      markers[i].ts = h.start_ts + 100 + i;
      markers[i].tid = 1234;
      markers[i].name = name;
    }
    w->append(std::span<const MarkerRec>(markers.data(), markers.size()));
  }
  if (!captureInfo.empty()) w->appendCaptureInfo(captureInfo);
  if (finish) EXPECT_TRUE(w->finish().ok());
}

}  // namespace tachyon::viewer::status_test
