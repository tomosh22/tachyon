#include "etw_hex.h"

#include <fmt/format.h>

#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>

namespace tachyon::etlstat {

namespace {
int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

template <typename T>
bool parseUnsigned(std::string_view text, T& out) {
  uint64_t value = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || ptr != text.data() + text.size()) return false;
  if (value > std::numeric_limits<T>::max()) return false;
  out = static_cast<T>(value);
  return true;
}
}  // namespace

EtwEvent HexEvent::view() const {
  EtwEvent e = event;
  e.payload = std::span<const std::byte>(data.data(), data.size());
  return e;
}

std::string toHex(std::span<const std::byte> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (std::byte b : bytes) {
    const auto v = static_cast<uint8_t>(b);
    out.push_back(kDigits[v >> 4]);
    out.push_back(kDigits[v & 0xF]);
  }
  return out;
}

std::string formatHexEventLine(const EtwEvent& e) {
  return fmt::format("event provider={} opcode={} version={} ts={} tid={} pid={} cpu={} data={}",
                     guidToString(e.provider), e.opcode, e.version, e.timestamp, e.threadId, e.processId,
                     e.cpu, toHex(e.payload));
}

Result<HexEvent> parseHexEventLine(std::string_view line) {
  std::istringstream in{std::string(line)};
  std::string token;
  if (!(in >> token) || token != "event")
    return makeError(ErrorCode::CorruptData, "line must start with 'event'");
  HexEvent out;
  bool haveProvider = false;
  while (in >> token) {
    const size_t eq = token.find('=');
    if (eq == std::string::npos) return makeError(ErrorCode::CorruptData, "expected key=value: " + token);
    const std::string_view key = std::string_view(token).substr(0, eq);
    const std::string_view value = std::string_view(token).substr(eq + 1);
    bool good = true;
    if (key == "provider") {
      auto g = parseGuid(value);
      good = g.has_value();
      if (g) out.event.provider = *g;
      haveProvider = good;
    } else if (key == "opcode") {
      good = parseUnsigned(value, out.event.opcode);
    } else if (key == "version") {
      good = parseUnsigned(value, out.event.version);
    } else if (key == "ts") {
      good = parseUnsigned(value, out.event.timestamp);
    } else if (key == "tid") {
      good = parseUnsigned(value, out.event.threadId);
    } else if (key == "pid") {
      good = parseUnsigned(value, out.event.processId);
    } else if (key == "cpu") {
      good = parseUnsigned(value, out.event.cpu);
    } else if (key == "data") {
      good = value.size() % 2 == 0;
      for (size_t i = 0; good && i + 1 < value.size(); i += 2) {
        const int hi = nibble(value[i]);
        const int lo = nibble(value[i + 1]);
        if (hi < 0 || lo < 0) {
          good = false;
        } else {
          out.data.push_back(static_cast<std::byte>((hi << 4) | lo));
        }
      }
    } else {
      return makeError(ErrorCode::CorruptData, "unknown key: " + std::string(key));
    }
    if (!good) return makeError(ErrorCode::CorruptData, "bad value for " + std::string(key));
  }
  if (!haveProvider) return makeError(ErrorCode::CorruptData, "missing provider");
  return out;
}

Result<std::vector<HexEvent>> parseHexEvents(std::string_view text) {
  std::vector<HexEvent> events;
  size_t lineNo = 0;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    ++lineNo;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty() || line.front() == '#') continue;
    auto parsed = parseHexEventLine(line);
    if (!parsed.ok()) {
      return makeError(ErrorCode::CorruptData, fmt::format("line {}: {}", lineNo, parsed.error().message));
    }
    events.push_back(std::move(parsed).value());
  }
  return events;
}

Result<std::vector<HexEvent>> loadHexEventFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return makeError(ErrorCode::NotFound, "cannot open " + path);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return parseHexEvents(buffer.str());
}

bool HexDumpSelector::select(const EtwEvent& event) {
  uint32_t& seen = m_seen[EventKey{event.provider, event.opcode, event.version}];
  if (seen >= m_limit) return false;
  ++seen;
  return true;
}

}  // namespace tachyon::etlstat
