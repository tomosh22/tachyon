// Shared helpers for the model tests.
#pragma once

#include "core/hash.h"
#include "model/model.h"

#include <unistd.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tachyon::model::testing_util {

inline const char* const kAllFixtures[] = {"tiny",   "medium",    "scopes",  "fibers",
                                           "frames", "recursion", "partial", "partial_torn"};

inline std::filesystem::path fixturePath(const std::string& name) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / (name + ".tachyon");
}

inline nlohmann::json loadExpected(const std::string& name) {
  std::ifstream f(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / (name + ".expected.json"));
  return nlohmann::json::parse(f);
}

// Creates a unique temp directory and removes it on destruction.
class TempDir {
 public:
  TempDir() {
    static std::atomic<uint32_t> counter{0};
    m_path = std::filesystem::temp_directory_path() /
             ("tachyon_model_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(m_path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::filesystem::path& path() const { return m_path; }

 private:
  std::filesystem::path m_path;
};

template <typename T>
uint64_t hashVec(const std::vector<T>& v, uint64_t seed) {
  seed = core::hashCombine(seed, v.size());
  return v.empty() ? seed : core::hashBytes(v.data(), v.size() * sizeof(T), seed);
}

// Hash over every loaded field (content, not capacity), used to compare loads byte for byte.
inline uint64_t fingerprint(const TraceModel& m) {
  uint64_t h = core::hashPod(m.header);
  h = core::hashCombine(h, m.isPartial);
  h = core::hashCombine(h, m.sampleIntervalTicks);
  h = core::hashCombine(h, m.captureInfo.sampling_hz);
  h = core::hashCombine(h, m.captureInfo.events_lost);
  h = core::hashString(m.captureInfo.raw_json, h);
  for (const Process& p : m.processes) {
    h = core::hashPod(p.pid, h);
    h = core::hashPod(p.parent_pid, h);
    h = core::hashPod(p.start_ts, h);
    h = core::hashPod(p.end_ts, h);
    h = core::hashString(p.name, h);
    h = core::hashString(p.command_line, h);
  }
  for (const Thread& t : m.threads) {
    h = core::hashPod(t.tid, h);
    h = core::hashPod(t.pid, h);
    h = core::hashString(t.name, h);
    h = core::hashPod(t.first_ts, h);
    h = core::hashPod(t.last_ts, h);
    h = core::hashCombine(h, static_cast<uint64_t>(t.is_target) | (static_cast<uint64_t>(t.role) << 8));
  }
  for (const Module& mod : m.modules) {
    h = core::hashPod(mod.pid, h);
    h = core::hashPod(mod.base, h);
    h = core::hashPod(mod.size, h);
    h = core::hashString(mod.path, h);
    h = core::hashCombine(h, mod.id.kind);
    h = core::hashBytes(mod.id.bytes.data(), mod.id.bytes.size(), h);
    h = core::hashCombine(h, mod.id.len);
    h = core::hashCombine(h, mod.id.age);
    h = core::hashPod(mod.load_ts, h);
    h = core::hashPod(mod.unload_ts, h);
  }
  for (const Frame& f : m.frames) {
    h = core::hashPod(f.module.get(), h);
    h = core::hashPod(f.addr_or_rva, h);
  }
  for (const CallstackNode& n : m.nodes) {
    h = core::hashPod(n.parent.get(), h);
    h = core::hashPod(n.frame.get(), h);
  }
  for (size_t i = 0; i < m.strings.size(); ++i)
    h = core::hashString(m.strings.get(StringIdx(static_cast<uint32_t>(i))), h);
  for (const SampleColumns& s : m.samplesByThread) {
    h = hashVec(s.ts, h);
    h = hashVec(s.callstack, h);
    h = hashVec(s.cpu, h);
    h = hashVec(s.flags, h);
  }
  h = hashVec(m.contextSwitches, h);
  h = hashVec(m.readyEvents, h);
  h = hashVec(m.scopeEvents, h);
  h = hashVec(m.clockCalibrations, h);
  for (const FiberSwitch& f : m.fiberSwitches) {
    h = core::hashPod(f.ts, h);
    h = core::hashPod(f.thread.get(), h);
    h = core::hashPod(f.kind, h);
    h = core::hashPod(f.fiber_id, h);
    h = core::hashPod(f.other_fiber_id, h);
  }
  for (const FrameMark& f : m.frameMarks) {
    h = core::hashPod(f.ts, h);
    h = core::hashPod(f.thread.get(), h);
    h = core::hashPod(f.name.get(), h);
    h = core::hashPod(f.frame_number, h);
  }
  for (const Marker& k : m.markers) {
    h = core::hashPod(k.ts, h);
    h = core::hashPod(k.thread.get(), h);
    h = core::hashPod(k.name.get(), h);
    h = core::hashPod(k.data.get(), h);
    h = core::hashPod(k.color, h);
  }
  for (const PlotSeries& p : m.plots) {
    h = core::hashPod(p.name.get(), h);
    h = hashVec(p.ts, h);
    h = hashVec(p.value, h);
  }
  return h;
}

}  // namespace tachyon::model::testing_util
