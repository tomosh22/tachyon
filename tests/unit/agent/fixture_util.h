// Writes a small valid .tachyon trace used as the mock backend's fixture.
#pragma once

#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace tachyon::agent::testing_util {

inline std::filesystem::path writeFixtureTrace(const std::filesystem::path& path) {
  trace::FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = 100;
  h.end_ts = 200;
  h.platform = trace::kPlatformLinux;
  auto w = trace::TraceWriter::open(path, h);
  EXPECT_TRUE(w.ok());
  if (!w.ok()) return path;
  w->internString("agent-fixture");
  trace::ProcessRec proc{};
  proc.pid = 42;
  proc.start_ts = 100;
  proc.end_ts = 200;
  w->append(std::span<const trace::ProcessRec>(&proc, 1));
  EXPECT_TRUE(w->finish().ok());
  return path;
}

}  // namespace tachyon::agent::testing_util
