#include "capture/windows_etw/session_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace tachyon::capture::etw {

BufferSettings computeBufferSettings(uint32_t logicalCores, uint32_t samplingHz, const BufferOverrides& o) {
  if (samplingHz == 0) samplingHz = kDefaultSamplingHz;
  BufferSettings s;
  s.bufferSizeKb = kDefaultBufferSizeKb;
  s.minimumBuffers = std::max<uint32_t>(kMinBufferCount, 4 * logicalCores);
  const uint32_t headroom = 2 + (samplingHz + 3999) / 4000;
  s.maximumBuffers = std::min<uint32_t>(s.minimumBuffers * headroom, kMaxBufferCountCap);
  s.flushTimerSeconds = 1;

  if (o.bufferSizeKb) s.bufferSizeKb = *o.bufferSizeKb;
  if (o.minimumBuffers) s.minimumBuffers = *o.minimumBuffers;
  if (o.maximumBuffers) s.maximumBuffers = *o.maximumBuffers;
  if (o.flushTimerSeconds) s.flushTimerSeconds = *o.flushTimerSeconds;

  s.bufferSizeKb = std::clamp<uint32_t>(s.bufferSizeKb, 64, 16384);
  s.minimumBuffers = std::max<uint32_t>(s.minimumBuffers, 2);
  s.maximumBuffers = std::max(s.maximumBuffers, s.minimumBuffers);
  if (s.flushTimerSeconds == 0) s.flushTimerSeconds = 1;
  return s;
}

uint32_t profileIntervalUnits(uint32_t hz) {
  if (hz == 0) hz = kDefaultSamplingHz;
  if (hz >= 8000) return kMinProfileIntervalUnits;
  return std::max<uint32_t>(kMinProfileIntervalUnits, 10'000'000u / hz);
}

double intervalUnitsToHz(uint32_t units) {
  return units == 0 ? 0.0 : 10'000'000.0 / static_cast<double>(units);
}

uint32_t enableFlagsFor(const CaptureOptions&) {
  return kFlagProcess | kFlagThread | kFlagImageLoad | kFlagProfile | kFlagCSwitch | kFlagDispatcher;
}

std::vector<StackEventId> stackEventsFor(const CaptureOptions& options) {
  std::vector<StackEventId> ids;
  if (options.stackProfile)
    ids.push_back({etlconvert::guids::kPerfInfo, etlconvert::opcodes::kPerfInfoSampledProfile});
  if (options.stackCSwitch) ids.push_back({etlconvert::guids::kThread, etlconvert::opcodes::kThreadCSwitch});
  if (options.stackReady)
    ids.push_back({etlconvert::guids::kThread, etlconvert::opcodes::kThreadReadyThread});
  return ids;
}

std::string makeSessionName(uint32_t pid, uint32_t counter) {
  return std::string(kSessionNamePrefix) + " " + std::to_string(pid) + " " + std::to_string(counter);
}

etlconvert::Guid makeSessionGuid(uint32_t pid, uint32_t counter, uint64_t stamp) {
  // Fixed base, then mix the identifying values in.
  etlconvert::Guid g{0x7ac5a1e0u, 0x1e71u, 0x4a7cu, {0x9b, 0x1d, 0x54, 0x61, 0x63, 0x68, 0x79, 0x6f}};
  g.data1 ^= pid;
  g.data2 = static_cast<uint16_t>(g.data2 ^ (counter & 0xFFFFu));
  g.data3 = static_cast<uint16_t>(g.data3 ^ ((counter >> 16) & 0xFFFFu) ^ (stamp & 0xFFFFu));
  for (size_t i = 0; i < 4; ++i) g.data4[4 + i] ^= static_cast<uint8_t>((stamp >> (16 + 8 * i)) & 0xFFu);
  return g;
}

namespace {

std::optional<uint32_t> parseDigits(std::string_view tok) {
  if (tok.empty() || tok.size() > 10) return std::nullopt;
  uint64_t v = 0;
  for (char c : tok) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  if (v > 0xFFFFFFFFull) return std::nullopt;
  return static_cast<uint32_t>(v);
}

}  // namespace

SessionNameInfo parseSessionName(std::string_view name) {
  SessionNameInfo info;
  const std::string prefix = std::string(kSessionNamePrefix) + " ";
  if (name.substr(0, prefix.size()) != prefix) return info;
  info.tachyon = true;
  std::string_view rest = name.substr(prefix.size());
  bool havePid = false;
  while (!rest.empty()) {
    const size_t sp = rest.find(' ');
    const std::string_view tok = rest.substr(0, sp);
    rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
    if (tok.empty()) continue;
    const bool digitStart = tok.front() >= '0' && tok.front() <= '9';
    if (!havePid) {
      if (!digitStart) continue;  // a word such as "etlstat" between the prefix and the pid
      const auto num = parseDigits(tok);
      if (!num || *num == 0) break;  // starts like a pid but is not one: give up rather than guess
      info.pid = num;
      havePid = true;
    } else {
      info.counter = parseDigits(tok);
      break;
    }
  }
  return info;
}

OrphanVerdict classifySession(std::string_view loggerName,
                              const std::function<bool(uint32_t)>& isProcessAlive) {
  const SessionNameInfo info = parseSessionName(loggerName);
  if (!info.tachyon) return OrphanVerdict::NotTachyon;
  if (!info.pid) return OrphanVerdict::Unparseable;
  return isProcessAlive(*info.pid) ? OrphanVerdict::Alive : OrphanVerdict::Orphan;
}

Result<void> validateCaptureOptions(const CaptureOptions& o) {
  switch (o.mode) {
    case CaptureMode::Launch:
      if (o.exe.empty()) return makeError(ErrorCode::InvalidArgument, "launch mode needs an executable");
      break;
    case CaptureMode::Attach:
      if (o.pid == 0) return makeError(ErrorCode::InvalidArgument, "attach mode needs a pid");
      break;
    case CaptureMode::System: break;
  }
  if (o.outPath.empty()) return makeError(ErrorCode::InvalidArgument, "outPath is required");
  return ok();
}

Result<SessionConfig> makeSessionConfig(const CaptureOptions& options, std::string name,
                                        const etlconvert::Guid& guid, std::filesystem::path etlPath,
                                        uint32_t logicalCores, const BufferOverrides& overrides) {
  if (name.empty()) return makeError(ErrorCode::InvalidArgument, "session name is empty");
  if (etlPath.empty()) return makeError(ErrorCode::InvalidArgument, "ETL path is empty");
  SessionConfig c;
  c.name = std::move(name);
  c.guid = guid;
  c.etlPath = std::move(etlPath);
  c.buffers = computeBufferSettings(logicalCores, options.samplingHz, overrides);
  c.enableFlags = enableFlagsFor(options);
  c.profileIntervalUnits = profileIntervalUnits(options.samplingHz);
  c.stackEvents = stackEventsFor(options);
  if (options.circularMb > 0) {
    if (options.circularMb < kMinCircularMb || options.circularMb > kMaxCircularMb)
      return makeError(ErrorCode::InvalidArgument, "circularMb must be between " +
                                                       std::to_string(kMinCircularMb) + " and " +
                                                       std::to_string(kMaxCircularMb));
    c.circular = true;
    c.maxFileMb = options.circularMb;
  }
  return c;
}

std::filesystem::path etlSegmentPath(const std::filesystem::path& out, uint32_t segment) {
  std::string s = out.string();
  if (segment > 0) s += "." + std::to_string(segment);
  s += ".etl";
  return s;
}

bool isValidFlushName(std::string_view name) {
  return !name.empty() && name.find_first_of("/\\") == std::string_view::npos && name != "." && name != "..";
}

std::string captureOptionsJson(const CaptureOptions& o, const SessionConfig& c,
                               const std::vector<uint32_t>& targetPids) {
  nlohmann::json j;
  j["backend"] = "etw";
  j["mode"] = std::string(captureModeName(o.mode));
  j["pid"] = o.pid;
  if (o.mode == CaptureMode::Launch) {
    j["exe"] = o.exe;
    j["args"] = o.args;
    j["cwd"] = o.cwd;
    j["start_immediately"] = o.startImmediately;
  }
  j["follow_children"] = o.followChildren;
  j["child_filter"] = o.childFilter;
  j["target_pids"] = targetPids;
  j["sampling_hz_requested"] = o.samplingHz;
  j["sampling_hz_effective"] = intervalUnitsToHz(c.profileIntervalUnits);
  j["profile_interval_100ns"] = c.profileIntervalUnits;
  j["circular_mb"] = o.circularMb;
  j["stackwalk"] = {{"profile", o.stackProfile}, {"cswitch", o.stackCSwitch}, {"readythread", o.stackReady}};
  j["kernel_stacks"] = o.kernelStacks;
  j["enable_flags"] = c.enableFlags;
  j["buffers"] = {{"size_kb", c.buffers.bufferSizeKb},
                  {"min", c.buffers.minimumBuffers},
                  {"max", c.buffers.maximumBuffers},
                  {"flush_timer_s", c.buffers.flushTimerSeconds}};
  j["session"] = c.name;
  return j.dump();
}

}  // namespace tachyon::capture::etw
