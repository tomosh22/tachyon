#include "etw_types.h"

#include <fmt/format.h>

namespace tachyon::etlstat {

namespace {
int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::optional<uint64_t> parseHexDigits(std::string_view text) {
  uint64_t value = 0;
  for (char c : text) {
    const int v = hexValue(c);
    if (v < 0) return std::nullopt;
    value = (value << 4) | static_cast<uint64_t>(v);
  }
  return value;
}
}  // namespace

std::optional<Guid> parseGuid(std::string_view text) {
  if (text.size() == 38 && text.front() == '{' && text.back() == '}') text = text.substr(1, 36);
  if (text.size() != 36) return std::nullopt;
  if (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') return std::nullopt;
  const auto d1 = parseHexDigits(text.substr(0, 8));
  const auto d2 = parseHexDigits(text.substr(9, 4));
  const auto d3 = parseHexDigits(text.substr(14, 4));
  if (!d1 || !d2 || !d3) return std::nullopt;
  Guid g;
  g.data1 = static_cast<uint32_t>(*d1);
  g.data2 = static_cast<uint16_t>(*d2);
  g.data3 = static_cast<uint16_t>(*d3);
  const std::string_view tail[8] = {text.substr(19, 2), text.substr(21, 2), text.substr(24, 2),
                                    text.substr(26, 2), text.substr(28, 2), text.substr(30, 2),
                                    text.substr(32, 2), text.substr(34, 2)};
  for (size_t i = 0; i < 8; ++i) {
    const auto b = parseHexDigits(tail[i]);
    if (!b) return std::nullopt;
    g.data4[i] = static_cast<uint8_t>(*b);
  }
  return g;
}

std::string guidToString(const Guid& g) {
  return fmt::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", g.data1,
                     g.data2, g.data3, g.data4[0], g.data4[1], g.data4[2], g.data4[3], g.data4[4], g.data4[5],
                     g.data4[6], g.data4[7]);
}

std::string providerName(const Guid& guid) {
  if (guid == guids::kProcess) return "Process";
  if (guid == guids::kThread) return "Thread";
  if (guid == guids::kImage) return "Image";
  if (guid == guids::kPerfInfo) return "PerfInfo";
  if (guid == guids::kStackWalk) return "StackWalk";
  if (guid == guids::kEventTrace) return "EventTrace";
  return guidToString(guid);
}

std::string_view opcodeName(const Guid& provider, uint8_t opcode) {
  using namespace opcodes;
  if (provider == guids::kProcess || provider == guids::kThread) {
    switch (opcode) {
      case kProcessStart: return "Start";
      case kProcessEnd: return "End";
      case kProcessDcStart: return "DCStart";
      case kProcessDcEnd: return "DCEnd";
      default: break;
    }
    if (provider == guids::kThread) {
      switch (opcode) {
        case kThreadCSwitch: return "CSwitch";
        case kThreadReadyThread: return "ReadyThread";
        case kThreadSetName: return "SetName";
        default: break;
      }
    }
  } else if (provider == guids::kImage) {
    switch (opcode) {
      case kImageLoad: return "Load";
      case kImageUnload: return "Unload";
      case kImageDcStart: return "DCStart";
      case kImageDcEnd: return "DCEnd";
      default: break;
    }
  } else if (provider == guids::kPerfInfo) {
    if (opcode == kPerfInfoSampledProfile) return "SampledProfile";
  } else if (provider == guids::kStackWalk) {
    switch (opcode) {
      case kStackWalkStack: return "Stack";
      case kStackWalkKeyKernel: return "StackKeyKernel";
      case kStackWalkKeyUser: return "StackKeyUser";
      case kStackWalkKeyRundown: return "StackKeyRundown";
      case kStackWalkKeyDelete: return "StackKeyDelete";
      default: break;
    }
  }
  return {};
}

}  // namespace tachyon::etlstat
