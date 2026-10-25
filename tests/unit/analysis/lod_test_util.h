// Shared helpers of the WP-36 LOD tests: fixture loading and brute-force reference computations that follow
// the definitions in analysis/lod.h literally (no pyramid, no binary search tricks).
#pragma once

#include "analysis/lod.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::analysis::testutil {

inline std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

// Loaded once per process and fixture; states are built.
inline const model::TraceModel& fixture(const std::string& name) {
  static std::map<std::string, std::unique_ptr<model::TraceModel>> cache;
  auto& slot = cache[name];
  if (!slot) {
    auto loaded = model::TraceModel::load(generated(name + ".tachyon"));
    EXPECT_TRUE(loaded.ok()) << (loaded.ok() ? "" : loaded.error().toString());
    slot = std::make_unique<model::TraceModel>(std::move(*loaded));
    EXPECT_TRUE(buildThreadStates(*slot).ok());
  }
  return *slot;
}

struct Ext {
  uint64_t s, e;
  uint32_t leaf;
};

// Sample extents per the lod.h definition (clipped to the next sample and to the trace range).
inline std::vector<Ext> extents(const model::TraceModel& m, size_t thread) {
  std::vector<Ext> out;
  const auto& sc = m.samplesByThread[thread];
  const uint64_t start = m.header.start_ts, end = m.header.end_ts;
  const uint64_t interval = std::max<uint64_t>(m.sampleIntervalTicks, 1);
  for (size_t i = 0; i < sc.ts.size(); ++i) {
    uint64_t e = sc.ts[i] + interval;
    if (i + 1 < sc.ts.size() && sc.ts[i + 1] < e) e = sc.ts[i + 1];
    e = std::min(e, end);
    const uint64_t s = std::max(sc.ts[i], start);
    if (e <= s) continue;
    const uint32_t leaf = sc.callstack[i].get() < m.nodes.size() ? sc.callstack[i].get() : 0;
    out.push_back({s, e, leaf});
  }
  return out;
}

// Root-exclusive path of a leaf: element d-1 is the node at depth d.
inline std::vector<uint32_t> pathOf(const model::TraceModel& m, uint32_t leaf) {
  std::vector<uint32_t> p;
  for (uint32_t v = leaf; v != 0; v = m.nodes[v].parent.get()) p.push_back(v);
  std::reverse(p.begin(), p.end());
  return p;
}

inline uint32_t bruteLca(const model::TraceModel& m, uint32_t a, uint32_t b) {
  const auto pa = pathOf(m, a), pb = pathOf(m, b);
  uint32_t r = 0;
  for (size_t i = 0; i < std::min(pa.size(), pb.size()) && pa[i] == pb[i]; ++i) r = pa[i];
  return r;
}

inline uint8_t round255(uint64_t num, uint64_t den) {
  return static_cast<uint8_t>(std::min<uint64_t>(255, (num * 255 + den / 2) / den));
}

}  // namespace tachyon::analysis::testutil
