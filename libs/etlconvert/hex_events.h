// Loader for the text "hex event" fixture format written by `etlstat stats --dump-hex` (and hand-built
// fixtures under tests/fixtures/etw). One event per line:
//   event provider=<guid> opcode=<n> version=<n> ts=<n> tid=<n> pid=<n> cpu=<n> data=<hex bytes, no spaces>
// Blank lines and lines starting with '#' are ignored.
#pragma once

#include "core/result.h"
#include "etlconvert/etl_events.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::etlconvert {

struct HexEvent {
  EtwEvent event;  // payload is rebound by view()
  std::vector<std::byte> data;

  // Thread-safe: pure function. The event with its payload pointing at `data`.
  EtwEvent view() const {
    EtwEvent e = event;
    e.payload = Bytes(data.data(), data.size());
    return e;
  }
};

// Thread-safe: pure function. "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", optionally in braces.
std::optional<Guid> parseGuid(std::string_view text);
// Thread-safe: pure function. Lower-case, no braces.
std::string guidToString(const Guid& guid);
// Thread-safe: pure function.
Result<HexEvent> parseHexEventLine(std::string_view line);
// Thread-safe: pure function.
Result<std::vector<HexEvent>> parseHexEvents(std::string_view text);
// Thread-safe: reads a file.
Result<std::vector<HexEvent>> loadHexEventFile(const std::filesystem::path& path);
// Thread-safe: pure function. One "event ..." line (no trailing newline); the inverse of parseHexEventLine.
std::string formatHexEventLine(const EtwEvent& event);

}  // namespace tachyon::etlconvert
