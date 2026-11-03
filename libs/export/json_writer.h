// Minimal streaming JSON writer (WP-63). Output is compact, deterministic and always valid UTF-8 JSON:
// invalid UTF-8 in strings is replaced by U+FFFD, non-finite doubles are written as null. Values are buffered
// and handed to the std::ostream in large blocks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace tachyon::exporters {

class JsonWriter {
 public:
  // Not thread-safe (one writer per stream). The stream must outlive the writer.
  explicit JsonWriter(std::ostream& out);

  // All members: not thread-safe. The caller keeps the structure balanced (every begin has its end; a key
  // precedes each value inside an object).
  void beginObject();
  void endObject();
  void beginArray();
  void endArray();
  // Writes "name": and expects the value next.
  void key(std::string_view name);

  void value(std::string_view s);
  void value(const char* s) { value(std::string_view(s)); }
  void value(uint64_t v);
  void value(int64_t v);
  void value(uint32_t v) { value(static_cast<uint64_t>(v)); }
  void value(int v) { value(static_cast<int64_t>(v)); }
  void value(double v);
  void value(bool v);
  void null();
  // Writes `json` verbatim as one value (already valid JSON: a number, a nested fragment ...).
  void raw(std::string_view json);

  // Writes a fixed-point decimal of `nanos` / 1000 with up to three fractional digits (trailing zeros
  // trimmed), i.e. a nanosecond count rendered in microseconds, exactly. Negative values get a '-'.
  void microsFromNanos(int64_t nanos);

  // Writes the buffered bytes to the stream and returns whether the stream is still good.
  bool flush();
  bool good() const { return m_good; }

 private:
  void beforeValue();
  void append(std::string_view s);
  void appendString(std::string_view s);
  void maybeFlush() {
    if (m_buf.size() >= kFlushAt) writeOut();
  }
  void writeOut();

  static constexpr size_t kFlushAt = size_t{1} << 16;
  std::ostream& m_out;
  std::string m_buf;
  bool m_needComma = false;
  bool m_afterKey = false;
  bool m_good = true;
};

}  // namespace tachyon::exporters
