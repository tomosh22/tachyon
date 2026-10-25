// Command line parsing for etlstat.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/result.h"

namespace tachyon::etlstat {

enum class Command { Help, Record, Stats, Replay };

struct CliOptions {
  Command command = Command::Help;
  // record
  std::string outPath;
  uint32_t seconds = 10;
  uint32_t hz = 8000;
  std::string exePath;
  std::string exeArgs;
  // stats / replay
  std::string inputPath;  // .etl for stats, .hexevents for replay
  bool json = false;
  uint32_t dumpHexCount = 0;
  std::string dumpOutPath;  // default: <input>.hexevents
  std::optional<uint32_t> pid;
  std::string image;  // process image name filter, e.g. tachyon-testbed.exe
};

// Thread-safe: pure function. `args` excludes argv[0].
Result<CliOptions> parseCli(std::span<const std::string_view> args);

// Thread-safe: pure function.
std::string usageText();

}  // namespace tachyon::etlstat
