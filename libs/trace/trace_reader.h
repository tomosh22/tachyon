// Reader for .tachyon trace files, including recovery of unfinalized (partial) files.
#pragma once

#include "core/result.h"
#include "trace/string_table.h"
#include "trace/trace_format.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tachyon::trace {

class TraceReader {
 public:
  // Thread-safe: opens the file and reads the footer. If footer_offset == 0 or the footer is unreadable the
  // chunks are scanned from offset 64 up to the first truncated/invalid chunk and header().flags gets
  // kFlagPartial.
  static Result<TraceReader> open(const std::filesystem::path& path);

  TraceReader(TraceReader&&) noexcept;
  TraceReader& operator=(TraceReader&&) noexcept;
  TraceReader(const TraceReader&) = delete;
  TraceReader& operator=(const TraceReader&) = delete;
  ~TraceReader();

  // Thread-safe for const access.
  const FileHeader& header() const { return m_header; }
  // Thread-safe for const access. One entry per chunk in file order (the footer chunk itself is excluded).
  std::span<const FooterRec> chunks() const { return m_chunks; }
  // Thread-safe for const access.
  bool isPartial() const { return (m_header.flags & kFlagPartial) != 0; }

  // Not thread-safe (shared file cursor). Reads and decompresses one chunk's payload into `out`.
  Result<void> readChunk(const FooterRec& chunk, std::vector<std::byte>& out);

  // Typed helpers: not thread-safe. Each concatenates all chunks of that type in file order.
  Result<StringTable> readStrings();
  Result<std::vector<ProcessRec>> readProcesses() { return readRecords<ProcessRec>(ChunkType::Processes); }
  Result<std::vector<ThreadRec>> readThreads() { return readRecords<ThreadRec>(ChunkType::Threads); }
  Result<std::vector<ModuleRec>> readModules() { return readRecords<ModuleRec>(ChunkType::Modules); }
  // Not thread-safe. Modules as naturally aligned ModuleInfo; prefer this over readModules() (see
  // ModuleInfo).
  Result<std::vector<ModuleInfo>> readModuleInfos() {
    TACHYON_TRY_ASSIGN(std::vector<ModuleRec> recs, readModules());
    std::vector<ModuleInfo> infos;
    infos.reserve(recs.size());
    for (const ModuleRec& r : recs) infos.push_back(toModuleInfo(r));
    return infos;
  }
  Result<std::vector<FrameRec>> readFrames() { return readRecords<FrameRec>(ChunkType::Frames); }
  Result<std::vector<CallstackNodeRec>> readCallstackNodes() {
    return readRecords<CallstackNodeRec>(ChunkType::CallstackNodes);
  }
  Result<SampleBatch> readSamples();
  Result<std::vector<ContextSwitchRec>> readContextSwitches() {
    return readRecords<ContextSwitchRec>(ChunkType::ContextSwitches);
  }
  Result<std::vector<ReadyRec>> readReadyEvents() { return readRecords<ReadyRec>(ChunkType::ReadyEvents); }
  Result<std::vector<ScopeEventRec>> readScopeEvents() {
    return readRecords<ScopeEventRec>(ChunkType::ScopeEvents);
  }
  Result<std::vector<FiberEventRec>> readFiberEvents() {
    return readRecords<FiberEventRec>(ChunkType::FiberEvents);
  }
  Result<std::vector<FrameMarkRec>> readFrameMarks() {
    return readRecords<FrameMarkRec>(ChunkType::FrameMarks);
  }
  Result<std::vector<MarkerRec>> readMarkers() { return readRecords<MarkerRec>(ChunkType::Markers); }
  Result<std::vector<PlotSampleRec>> readPlotSamples() {
    return readRecords<PlotSampleRec>(ChunkType::PlotSamples);
  }
  Result<std::vector<ThreadNameRec>> readThreadNames() {
    return readRecords<ThreadNameRec>(ChunkType::ThreadNames);
  }
  Result<std::vector<ClockCalibrationRec>> readClockCalibrations() {
    return readRecords<ClockCalibrationRec>(ChunkType::ClockCalibration);
  }
  // Raw JSON of the (first) CaptureInfo chunk; NotFound if absent.
  Result<std::string> readCaptureInfo();

  // Thread-safe: pure function. Appends the records of one already decoded chunk payload to `out`.
  template <typename T>
  static Result<void> decodeRecords(const FooterRec& chunk, std::span<const std::byte> payload,
                                    std::vector<T>& out) {
    if (chunk.record_count > kMaxRecordsPerChunk || payload.size() % sizeof(T) != 0 ||
        payload.size() / sizeof(T) != chunk.record_count)
      return makeError(ErrorCode::CorruptData, "chunk payload size does not match record count");
    const size_t at = out.size();
    const size_t n = static_cast<size_t>(chunk.record_count);
    if constexpr (sizeof(T) % alignof(uint32_t) == 0) {
      out.resize(at + n);
      if (!payload.empty()) std::memcpy(out.data() + at, payload.data(), payload.size());
    } else {
      // Records whose size is not a multiple of 4 (ModuleRec, 78 bytes) leave StrongId members misaligned in
      // arrays, so they must not be value-initialized in place; copy each from an aligned local instead.
      out.reserve(at + n);
      for (size_t i = 0; i < n; ++i) {
        alignas(8) T rec;
        std::memcpy(static_cast<void*>(&rec), payload.data() + i * sizeof(T), sizeof(T));
        out.push_back(rec);
      }
    }
    return ok();
  }
  // Thread-safe: pure function. Appends the samples of one decoded Samples chunk payload to `out`.
  static Result<void> decodeSamples(const FooterRec& chunk, std::span<const std::byte> payload,
                                    SampleBatch& out);

  // Not thread-safe. Concatenates all chunks of `type` as records of type T.
  template <typename T>
  Result<std::vector<T>> readRecords(ChunkType type) {
    std::vector<T> out;
    std::vector<std::byte> payload;
    for (const FooterRec& c : m_chunks) {
      if (c.type != static_cast<uint32_t>(type)) continue;
      TACHYON_TRY(readChunk(c, payload));
      TACHYON_TRY(decodeRecords<T>(c, payload, out));
    }
    return out;
  }

 private:
  TraceReader();
  Result<void> readFooter(uint64_t fileSize);
  void scanChunks(uint64_t fileSize);

  std::unique_ptr<std::ifstream> m_file;
  FileHeader m_header;
  std::vector<FooterRec> m_chunks;
  uint64_t m_fileSize = 0;
};

}  // namespace tachyon::trace
