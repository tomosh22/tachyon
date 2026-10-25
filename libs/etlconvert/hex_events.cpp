#include "etlconvert/hex_events.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

namespace tachyon::etlconvert {

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

bool parseHexDigits(std::string_view text, uint64_t& out) {
  if (text.empty()) return false;
  uint64_t v = 0;
  for (char c : text) {
    const int n = nibble(c);
    if (n < 0) return false;
    v = (v << 4) | static_cast<uint64_t>(n);
  }
  out = v;
  return true;
}
}  // namespace

std::optional<Guid> parseGuid(std::string_view text) {
  if (text.size() == 38 && text.front() == '{' && text.back() == '}') text = text.substr(1, 36);
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
    return std::nullopt;
  Guid g;
  uint64_t v = 0;
  if (!parseHexDigits(text.substr(0, 8), v)) return std::nullopt;
  g.data1 = static_cast<uint32_t>(v);
  if (!parseHexDigits(text.substr(9, 4), v)) return std::nullopt;
  g.data2 = static_cast<uint16_t>(v);
  if (!parseHexDigits(text.substr(14, 4), v)) return std::nullopt;
  g.data3 = static_cast<uint16_t>(v);
  const std::string_view tail = text.substr(19);  // "xxxx-xxxxxxxxxxxx"
  size_t out = 0;
  for (size_t i = 0; i < tail.size(); ++i) {
    if (tail[i] == '-') continue;
    if (i + 1 >= tail.size() || out >= 8) return std::nullopt;
    if (!parseHexDigits(tail.substr(i, 2), v)) return std::nullopt;
    g.data4[out++] = static_cast<uint8_t>(v);
    ++i;
  }
  if (out != 8) return std::nullopt;
  return g;
}

std::string guidToString(const Guid& g) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.data1, g.data2,
                g.data3, g.data4[0], g.data4[1], g.data4[2], g.data4[3], g.data4[4], g.data4[5], g.data4[6],
                g.data4[7]);
  return buf;
}

std::string formatHexEventLine(const EtwEvent& e) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string data;
  data.reserve(e.payload.size() * 2);
  for (std::byte b : e.payload) {
    data.push_back(kDigits[static_cast<uint8_t>(b) >> 4]);
    data.push_back(kDigits[static_cast<uint8_t>(b) & 0xF]);
  }
  std::ostringstream os;
  os << "event provider=" << guidToString(e.provider) << " opcode=" << static_cast<unsigned>(e.opcode)
     << " version=" << static_cast<unsigned>(e.version) << " ts=" << e.timestamp << " tid=" << e.threadId
     << " pid=" << e.processId << " cpu=" << e.cpu << " data=" << data;
  return os.str();
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
      return makeError(ErrorCode::CorruptData,
                       "line " + std::to_string(lineNo) + ": " + parsed.error().message);
    }
    events.push_back(std::move(parsed).value());
  }
  return events;
}

Result<std::vector<HexEvent>> loadHexEventFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return makeError(ErrorCode::NotFound, "cannot open " + path.string());
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return parseHexEvents(buffer.str());
}

}  // namespace tachyon::etlconvert
