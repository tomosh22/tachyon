// Windows named-pipe names derived from the agent's pipe name. Pure string functions (no Windows API), so
// they are unit-tested on every platform.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace tachyon::control {

// The agent's default pipe (pipe names are case-insensitive).
inline constexpr std::string_view kDefaultWindowsAgentPipe = R"(\\.\pipe\tachyon-agent)";

// Longest full pipe name (including `\\.\pipe\`) Windows accepts.
inline constexpr size_t kMaxWindowsPipeNameChars = 256;

// Thread-safe. The instrumentation pipe of process `pid` for the agent listening on `agentAddress`: the
// machine-wide `\\.\pipe\tachyon-inst-<pid>` for the default agent pipe, and
// `<agentAddress>-tachyon-inst-<pid>` for any other, so agents on private pipes (concurrent test processes, a
// second agent) never collide on a pid.
inline std::string windowsInstrumentationPipeName(std::string_view agentAddress, uint32_t pid) {
  const std::string suffix = std::to_string(pid);
  bool isDefault = agentAddress.empty() || agentAddress.size() == kDefaultWindowsAgentPipe.size();
  if (isDefault && !agentAddress.empty()) {
    for (size_t i = 0; i < agentAddress.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(agentAddress[i])) !=
          std::tolower(static_cast<unsigned char>(kDefaultWindowsAgentPipe[i]))) {
        isDefault = false;
        break;
      }
    }
  }
  if (isDefault) return R"(\\.\pipe\tachyon-inst-)" + suffix;
  std::string name = std::string(agentAddress) + "-tachyon-inst-" + suffix;
  if (name.size() > kMaxWindowsPipeNameChars) {
    // Too long for a pipe name: a deterministic short name from a hash of the agent address (FNV-1a, 64 bit,
    // case-folded like the pipe namespace).
    uint64_t h = 1469598103934665603ull;
    for (const char c : agentAddress) {
      h ^= static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
      h *= 1099511628211ull;
    }
    char hex[17];
    for (int i = 0; i < 16; ++i) hex[i] = "0123456789abcdef"[(h >> ((15 - i) * 4)) & 0xF];
    hex[16] = '\0';
    name = std::string(R"(\\.\pipe\tachyon-inst-)") + hex + "-" + suffix;
  }
  return name;
}

}  // namespace tachyon::control
