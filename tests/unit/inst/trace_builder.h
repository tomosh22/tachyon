// Test-only: writes a small sampled trace (all table and sampling chunk types) to merge instrumentation into.
#pragma once

#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <span>
#include <vector>

namespace tachyon::inst::testing_util {

struct BaseTrace {
  std::filesystem::path path;
  uint64_t startTs = 0, endTs = 0;
  uint64_t clockHz = 0;
};

// Writes a finalized trace: 3 strings + "", 2 modules, 3 frames, a 3-deep callstack, samples, context
// switches, ready events, a process, a thread, a clock calibration and a CaptureInfo. Times are in
// [startTs, endTs] at clockHz.
inline BaseTrace writeBaseTrace(const std::filesystem::path& path, uint64_t startTs = 1000,
                                uint64_t endTs = 2'000'000, uint64_t clockHz = 1'000'000'000,
                                bool withCaptureInfo = true, uint32_t extraFlags = 0) {
  using namespace trace;
  FileHeader h;
  h.clock_frequency_hz = clockHz;
  h.start_ts = startTs;
  h.end_ts = endTs;
  h.platform = kPlatformLinux;
  h.flags = extraFlags;
  auto w = TraceWriter::open(path, h);
  EXPECT_TRUE(w.ok());
  if (!w.ok()) return {path, startTs, endTs, clockHz};
  const StringIdx app = w->internString("app.exe");
  const StringIdx lib = w->internString("libfoo.so");
  const StringIdx thread = w->internString("main-thread");
  ProcessRec proc{};
  proc.pid = 1234;
  proc.parent_pid = 1;
  proc.start_ts = startTs;
  proc.end_ts = endTs;
  proc.name = app;
  proc.command_line = app;
  w->append(std::span<const ProcessRec>(&proc, 1));
  ThreadRec th{};
  th.tid = 1234;
  th.pid = 1234;
  th.start_ts = startTs;
  th.end_ts = endTs;
  th.name = thread;
  th.flags = kThreadFlagTargetProcess;
  w->append(std::span<const ThreadRec>(&th, 1));
  // ModuleRec is 78 bytes: an array of them would have misaligned elements, so build each one aligned and
  // copy the bytes (trace-format.md, Alignment).
  alignas(8) std::byte moduleBytes[2 * sizeof(ModuleRec)];
  auto putModule = [&](size_t index, uint64_t base, uint64_t size, StringIdx path, uint8_t idByte) {
    alignas(8) ModuleRec m;
    std::memset(static_cast<void*>(&m), 0, sizeof(m));
    m.pid = 1234;
    m.base = base;
    m.size = size;
    m.path = path;
    m.id_kind = kModuleIdGnuBuildId;
    m.id_len = 4;
    m.id[0] = idByte;
    m.load_ts = startTs;
    m.unload_ts = endTs;
    std::memcpy(moduleBytes + index * sizeof(ModuleRec), &m, sizeof(m));
  };
  putModule(0, 0x400000, 0x10000, app, 0xAB);
  putModule(1, 0x7f0000, 0x2000, lib, 0xCD);
  w->append(std::span<const ModuleRec>(reinterpret_cast<const ModuleRec*>(moduleBytes), 2));
  const FrameIdx f0 = w->internFrame(ModuleIdx(0), 0x100);
  const FrameIdx f1 = w->internFrame(ModuleIdx(0), 0x200);
  const FrameIdx f2 = w->internFrame(ModuleIdx(1), 0x80);
  const FrameIdx stack[3] = {f2, f1, f0};
  const CallstackIdx leaf = w->internCallstack(std::span<const FrameIdx>(stack, 3));
  SampleBatch samples;
  for (uint64_t i = 0; i < 50; ++i) samples.push(startTs + i * 10, 1234, leaf, static_cast<uint16_t>(i % 4));
  w->append(samples);
  ContextSwitchRec cs{};
  cs.ts = startTs + 100;
  cs.cpu = 1;
  cs.old_tid = 1234;
  cs.new_tid = 0;
  cs.old_thread_stack = leaf;
  w->append(std::span<const ContextSwitchRec>(&cs, 1));
  ReadyRec ready{};
  ready.ts = startTs + 200;
  ready.readied_tid = 1234;
  ready.readying_tid = 99;
  ready.readying_stack = leaf;
  w->append(std::span<const ReadyRec>(&ready, 1));
  ClockCalibrationRec cal{};
  cal.trace_ts = startTs;
  cal.other_clock_ts = 5;
  cal.other_clock_kind = kClockTsc;
  w->append(std::span<const ClockCalibrationRec>(&cal, 1));
  if (withCaptureInfo) w->appendCaptureInfo(R"({"sampling_hz":8000,"agent_version":"test"})");
  EXPECT_TRUE(w->finish().ok());
  return {path, startTs, endTs, clockHz};
}

}  // namespace tachyon::inst::testing_util
