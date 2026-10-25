// Text "hex event" format used for captured fixtures (tests/fixtures/etw/*.hexevents) and for replaying them
// without Windows. One event per line:
//   event provider=<guid> opcode=<n> version=<n> ts=<n> tid=<n> pid=<n> cpu=<n> data=<hex bytes, no spaces>
// Blank lines and lines starting with '#' are ignored.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "etw_stats.h"
#include "etw_types.h"

namespace tachyon::etlstat {

struct HexEvent {
  EtwEvent event;  // `event.payload` is rebound by view(); do not use directly
  std::vector<std::byte> data;

  // Thread-safe: pure function. EtwEvent whose payload points at `data`.
  EtwEvent view() const;
};

// Thread-safe: pure function. One "event ..." line (no trailing newline).
std::string formatHexEventLine(const EtwEvent& event);
// Thread-safe: pure function.
Result<HexEvent> parseHexEventLine(std::string_view line);
// Thread-safe: pure function. Parses a whole fixture text.
Result<std::vector<HexEvent>> parseHexEvents(std::string_view text);
// Thread-safe (reads a file).
Result<std::vector<HexEvent>> loadHexEventFile(const std::string& path);

// Thread-safe: pure function. Lower-case hex of the bytes, no separators.
std::string toHex(std::span<const std::byte> bytes);

// Selects the first `perKeyLimit` events of each (provider, opcode, version) for dumping. Not thread-safe.
class HexDumpSelector {
 public:
  explicit HexDumpSelector(uint32_t perKeyLimit) : m_limit(perKeyLimit) {}
  // Returns true (and counts it) when this event should be dumped.
  bool select(const EtwEvent& event);

 private:
  uint32_t m_limit;
  std::map<EventKey, uint32_t> m_seen;
};

}  // namespace tachyon::etlstat
