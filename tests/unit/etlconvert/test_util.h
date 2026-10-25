// Hand-built payloads, events, PE images and trace readers for the etlconvert unit tests.
#pragma once

#include "core/span_util.h"
#include "etlconvert/etl_converter.h"
#include "etlconvert/etl_events.h"
#include "etlconvert/hex_events.h"
#include "etlconvert/pe_info.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tachyon::etlconvert::testing {

using ByteVec = std::vector<std::byte>;

template <typename T>
void put(ByteVec& out, T value) {
  core::appendPod(out, value);
}

inline void putWide(ByteVec& out, const std::u16string& text, bool terminate = true) {
  for (char16_t c : text) put<uint16_t>(out, static_cast<uint16_t>(c));
  if (terminate) put<uint16_t>(out, 0);
}

inline void putAnsi(ByteVec& out, const std::string& text) {
  for (char c : text) put<uint8_t>(out, static_cast<uint8_t>(c));
  put<uint8_t>(out, 0);
}

inline ByteVec sampledProfilePayload(uint64_t ip, uint32_t tid) {
  ByteVec b;
  put<uint64_t>(b, ip);
  put<uint32_t>(b, tid);
  put<uint16_t>(b, 1);
  put<uint16_t>(b, 0);
  return b;
}

// Wait reason / state / mode / priority are distinguishable per field.
inline ByteVec cswitchPayload(uint32_t newTid, uint32_t oldTid, int8_t waitReason = 0, int8_t oldState = 5,
                              int8_t newPriority = 8) {
  ByteVec b;
  put<uint32_t>(b, newTid);
  put<uint32_t>(b, oldTid);
  put<int8_t>(b, newPriority);
  put<int8_t>(b, 9);
  put<uint8_t>(b, 1);
  put<int8_t>(b, 0);
  put<int8_t>(b, waitReason);
  put<int8_t>(b, 1);
  put<int8_t>(b, oldState);
  put<int8_t>(b, 3);
  put<uint32_t>(b, 77);
  put<uint32_t>(b, 0);
  return b;
}

inline ByteVec readyThreadPayload(uint32_t tid, int8_t reason = 1, int8_t flag = 3) {
  ByteVec b;
  put<uint32_t>(b, tid);
  put<int8_t>(b, reason);
  put<int8_t>(b, 2);
  put<int8_t>(b, flag);
  put<int8_t>(b, 0);
  return b;
}

inline ByteVec stackWalkPayload(uint64_t eventTs, uint32_t pid, uint32_t tid,
                                const std::vector<uint64_t>& frames) {
  ByteVec b;
  put<uint64_t>(b, eventTs);
  put<uint32_t>(b, pid);
  put<uint32_t>(b, tid);
  for (uint64_t f : frames) put<uint64_t>(b, f);
  return b;
}

inline ByteVec imagePayload(uint64_t base, uint64_t size, uint32_t pid, uint32_t timeDateStamp,
                            const std::u16string& path) {
  ByteVec b;
  put<uint64_t>(b, base);
  put<uint64_t>(b, size);
  put<uint32_t>(b, pid);
  put<uint32_t>(b, 0x1234);
  put<uint32_t>(b, timeDateStamp);
  put<uint32_t>(b, 0);
  put<uint64_t>(b, base);
  for (int i = 0; i < 4; ++i) put<uint32_t>(b, 0);
  putWide(b, path);
  return b;
}

inline ByteVec threadPayload(uint32_t pid, uint32_t tid) {
  ByteVec b;
  put<uint32_t>(b, pid);
  put<uint32_t>(b, tid);
  for (int i = 0; i < 12; ++i) put<uint32_t>(b, 0);
  return b;
}

inline ByteVec threadNamePayload(uint32_t pid, uint32_t tid, const std::u16string& name) {
  ByteVec b;
  put<uint32_t>(b, pid);
  put<uint32_t>(b, tid);
  putWide(b, name);
  return b;
}

// Process v3 payload without SID: {ptr key; pid; ppid; session; exit; ptr dirbase; 8 byte empty SID; name;
// cmdline}.
inline ByteVec processPayload(uint32_t pid, uint32_t ppid, const std::string& name,
                              const std::u16string& cmdline = u"") {
  ByteVec b;
  put<uint64_t>(b, 0xFFFF800012345678ull);
  put<uint32_t>(b, pid);
  put<uint32_t>(b, ppid);
  put<uint32_t>(b, 1);
  put<int32_t>(b, 0);
  put<uint64_t>(b, 0x1000);
  put<uint64_t>(b, 0);  // "no SID"
  putAnsi(b, name);
  putWide(b, cmdline);
  return b;
}

inline EtwEvent makeEvent(const Guid& provider, uint8_t opcode, uint8_t version, uint64_t ts, uint32_t tid,
                          uint32_t pid, uint16_t cpu, const ByteVec& payload) {
  EtwEvent e;
  e.provider = provider;
  e.opcode = opcode;
  e.version = version;
  e.timestamp = ts;
  e.threadId = tid;
  e.processId = pid;
  e.cpu = cpu;
  e.payload = Bytes(payload.data(), payload.size());
  return e;
}

// Owns the payloads of a scripted event sequence, so tests can build it incrementally.
class EventScript {
 public:
  EventScript& add(const Guid& provider, uint8_t opcode, uint8_t version, uint64_t ts, uint32_t tid,
                   uint32_t pid, uint16_t cpu, ByteVec payload) {
    HexEvent h;
    h.event = makeEvent(provider, opcode, version, ts, tid, pid, cpu, {});
    h.data = std::move(payload);
    m_events.push_back(std::move(h));
    return *this;
  }
  EventScript& sample(uint64_t ts, uint32_t tid, uint32_t pid, uint16_t cpu, uint64_t ip) {
    return add(guids::kPerfInfo, opcodes::kPerfInfoSampledProfile, 2, ts, tid, pid, cpu,
               sampledProfilePayload(ip, tid));
  }
  EventScript& cswitch(uint64_t ts, uint32_t headerTid, uint16_t cpu, uint32_t newTid, uint32_t oldTid,
                       int8_t waitReason = 0) {
    return add(guids::kThread, opcodes::kThreadCSwitch, 2, ts, headerTid, 0, cpu,
               cswitchPayload(newTid, oldTid, waitReason));
  }
  EventScript& ready(uint64_t ts, uint32_t headerTid, uint32_t pid, uint16_t cpu, uint32_t readiedTid) {
    return add(guids::kThread, opcodes::kThreadReadyThread, 2, ts, headerTid, pid, cpu,
               readyThreadPayload(readiedTid));
  }
  EventScript& stack(uint64_t ts, uint64_t parentTs, uint32_t pid, uint32_t tid,
                     const std::vector<uint64_t>& frames) {
    return add(guids::kStackWalk, opcodes::kStackWalkStack, 2, ts, tid, pid, 0,
               stackWalkPayload(parentTs, pid, tid, frames));
  }
  EventScript& process(uint8_t opcode, uint64_t ts, uint32_t pid, uint32_t ppid, const std::string& name) {
    return add(guids::kProcess, opcode, 3, ts, 4, 4, 0,
               processPayload(pid, ppid, name, u"\"" + std::u16string(name.begin(), name.end()) + u"\""));
  }
  EventScript& thread(uint8_t opcode, uint64_t ts, uint32_t pid, uint32_t tid) {
    return add(guids::kThread, opcode, 3, ts, 4, 4, 0, threadPayload(pid, tid));
  }
  EventScript& image(uint8_t opcode, uint64_t ts, uint32_t pid, uint64_t base, uint64_t size, uint32_t tds,
                     const std::u16string& path) {
    return add(guids::kImage, opcode, 3, ts, 4, 4, 0, imagePayload(base, size, pid, tds, path));
  }
  EventScript& setName(uint64_t ts, uint32_t pid, uint32_t tid, const std::u16string& name) {
    return add(guids::kThread, opcodes::kThreadSetName, 2, ts, tid, pid, 0,
               threadNamePayload(pid, tid, name));
  }

  const std::vector<HexEvent>& events() const { return m_events; }

  // Feeds the events through a pipeline (metadata pass first when options.twoPass).
  Result<ConvertStats> run(const std::filesystem::path& out, const ConvertOptions& options,
                           uint64_t freq = 10'000'000) const {
    EtlFileInfo info;
    info.clockFrequencyHz = freq;
    info.numberOfProcessors = 8;
    TACHYON_TRY_ASSIGN(std::unique_ptr<ConversionPipeline> pipeline,
                       ConversionPipeline::create(out, options, info));
    if (options.twoPass) {
      pipeline->beginPass(PipelinePass::Metadata);
      for (const HexEvent& e : m_events) pipeline->onEvent(e.view());
    }
    pipeline->beginPass(PipelinePass::Main);
    for (const HexEvent& e : m_events) pipeline->onEvent(e.view());
    return pipeline->finish();
  }

 private:
  std::vector<HexEvent> m_events;
};

// ---- PE images
// ---------------------------------------------------------------------------------------------

struct PeSpec {
  bool is64 = true;
  uint32_t timeDateStamp = 0x5F000000;
  uint32_t sizeOfImage = 0x20000;
  bool codeView = true;
  uint8_t guid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01};
  uint32_t age = 3;
  std::string pdb = "C:\\build\\game.pdb";
  std::vector<std::pair<uint32_t, uint32_t>> functions;  // (begin, end) RVAs for .pdata
};

// One .rdata-like section at RVA 0x1000 / file offset 0x400 holding the debug directory (offset 0), the RSDS
// record (offset 0x40) and the exception table (offset 0x400).
inline ByteVec buildPe(const PeSpec& spec) {
  ByteVec f(0x1400 + 0x1000, std::byte{0});
  auto w16 = [&](size_t at, uint16_t v) { std::memcpy(f.data() + at, &v, 2); };
  auto w32 = [&](size_t at, uint32_t v) { std::memcpy(f.data() + at, &v, 4); };
  w16(0, 0x5A4D);
  w32(0x3C, 0x80);
  w32(0x80, 0x00004550);
  const size_t fh = 0x84;
  const uint16_t optSize = spec.is64 ? 240 : 224;
  w16(fh, spec.is64 ? 0x8664 : 0x014C);
  w16(fh + 2, 1);
  w32(fh + 4, spec.timeDateStamp);
  w16(fh + 16, optSize);
  const size_t opt = fh + 20;
  w16(opt, spec.is64 ? 0x20B : 0x10B);
  w32(opt + 56, spec.sizeOfImage);
  w32(opt + 64, 0xABCD);
  w32(opt + (spec.is64 ? 108 : 92), 16);
  const size_t dirs = opt + (spec.is64 ? 112 : 96);
  const uint32_t sectionRva = 0x1000;
  const uint32_t sectionRaw = 0x400;
  if (!spec.functions.empty()) {
    w32(dirs + 3 * 8, sectionRva + 0x400);
    w32(dirs + 3 * 8 + 4, static_cast<uint32_t>(spec.functions.size() * 12));
  }
  if (spec.codeView) {
    w32(dirs + 6 * 8, sectionRva);
    w32(dirs + 6 * 8 + 4, 28);
  }
  const size_t sec = opt + optSize;
  std::memcpy(f.data() + sec, ".rdata", 6);
  w32(sec + 8, 0x1000);
  w32(sec + 12, sectionRva);
  w32(sec + 16, 0x1000);
  w32(sec + 20, sectionRaw);
  if (spec.codeView) {
    // IMAGE_DEBUG_DIRECTORY at section offset 0.
    const size_t dd = sectionRaw;
    w32(dd + 4, spec.timeDateStamp);
    w32(dd + 12, 2);  // IMAGE_DEBUG_TYPE_CODEVIEW
    const uint32_t recordSize = static_cast<uint32_t>(24 + spec.pdb.size() + 1);
    w32(dd + 16, recordSize);
    w32(dd + 20, sectionRva + 0x40);
    w32(dd + 24, sectionRaw + 0x40);
    const size_t rec = sectionRaw + 0x40;
    w32(rec, 0x53445352);
    std::memcpy(f.data() + rec + 4, spec.guid, 16);
    w32(rec + 20, spec.age);
    std::memcpy(f.data() + rec + 24, spec.pdb.c_str(), spec.pdb.size() + 1);
  }
  size_t at = sectionRaw + 0x400;
  for (const auto& [b, e] : spec.functions) {
    w32(at, b);
    w32(at + 4, e);
    at += 12;
  }
  return f;
}

// ---- trace reading
// -----------------------------------------------------------------------------------------

struct ReadBack {
  trace::TraceReader reader;
  trace::StringTable strings;
  std::vector<trace::CallstackNodeRec> nodes;
  std::vector<trace::FrameRec> frames;
  std::vector<trace::ModuleInfo> modules;
  std::vector<trace::ThreadRec> threads;
  std::vector<trace::ProcessRec> processes;
  trace::SampleBatch samples;
  std::vector<trace::ContextSwitchRec> switches;
  std::vector<trace::ReadyRec> readies;
  std::vector<trace::ThreadNameRec> names;
  std::string captureInfo;

  // Leaf-to-root frames of a callstack node, as (module, addrOrRva).
  std::vector<std::pair<uint32_t, uint64_t>> stackOf(CallstackIdx cs) const {
    std::vector<std::pair<uint32_t, uint64_t>> out;
    for (uint32_t n = cs.get(); n != 0 && n != kInvalidIdx; n = nodes[n].parent.get()) {
      // FrameRec is packed (12 bytes): copy to an aligned local before touching members.
      alignas(8) trace::FrameRec fr;
      std::memcpy(static_cast<void*>(&fr), static_cast<const void*>(&frames[nodes[n].frame.get()]),
                  sizeof(fr));
      const uint32_t module = fr.module.get();
      const uint64_t addr = fr.addr_or_rva;
      out.emplace_back(module, addr);
    }
    return out;
  }
  std::string_view str(StringIdx i) const { return strings.get(i); }
};

inline std::unique_ptr<ReadBack> readBack(const std::filesystem::path& path) {
  auto reader = trace::TraceReader::open(path);
  EXPECT_TRUE(reader.ok()) << (reader.ok() ? "" : reader.error().toString());
  if (!reader.ok()) return nullptr;
  auto rb = std::make_unique<ReadBack>(
      ReadBack{std::move(reader).value(), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}});
  auto take = [](auto&& r, auto& dst) {
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) dst = std::move(r).value();
  };
  take(rb->reader.readStrings(), rb->strings);
  take(rb->reader.readCallstackNodes(), rb->nodes);
  take(rb->reader.readFrames(), rb->frames);
  take(rb->reader.readModuleInfos(), rb->modules);
  take(rb->reader.readThreads(), rb->threads);
  take(rb->reader.readProcesses(), rb->processes);
  take(rb->reader.readSamples(), rb->samples);
  take(rb->reader.readContextSwitches(), rb->switches);
  take(rb->reader.readReadyEvents(), rb->readies);
  take(rb->reader.readThreadNames(), rb->names);
  take(rb->reader.readCaptureInfo(), rb->captureInfo);
  return rb;
}

inline std::filesystem::path tempPath(const std::string& name) {
  const auto dir = std::filesystem::path(TACHYON_BINARY_DIR) / "etlconvert_tmp";
  std::filesystem::create_directories(dir);
  return dir / name;
}

}  // namespace tachyon::etlconvert::testing
