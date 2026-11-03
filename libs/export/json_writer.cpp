#include "export/json_writer.h"

#include <charconv>
#include <cmath>
#include <ostream>

namespace tachyon::exporters {

JsonWriter::JsonWriter(std::ostream& out) : m_out(out) {
  m_buf.reserve(kFlushAt + 4096);
}

void JsonWriter::writeOut() {
  if (!m_buf.empty()) {
    m_out.write(m_buf.data(), static_cast<std::streamsize>(m_buf.size()));
    if (!m_out) m_good = false;
    m_buf.clear();
  }
}

bool JsonWriter::flush() {
  writeOut();
  m_out.flush();
  if (!m_out) m_good = false;
  return m_good;
}

void JsonWriter::append(std::string_view s) {
  m_buf.append(s.data(), s.size());
}

void JsonWriter::beforeValue() {
  if (m_afterKey) {
    m_afterKey = false;
  } else if (m_needComma) {
    m_buf.push_back(',');
  }
  m_needComma = true;
}

void JsonWriter::beginObject() {
  beforeValue();
  m_buf.push_back('{');
  m_needComma = false;
}

void JsonWriter::endObject() {
  m_buf.push_back('}');
  m_needComma = true;
  maybeFlush();
}

void JsonWriter::beginArray() {
  beforeValue();
  m_buf.push_back('[');
  m_needComma = false;
}

void JsonWriter::endArray() {
  m_buf.push_back(']');
  m_needComma = true;
  maybeFlush();
}

void JsonWriter::key(std::string_view name) {
  if (m_needComma) m_buf.push_back(',');
  appendString(name);
  m_buf.push_back(':');
  m_needComma = false;
  m_afterKey = true;
}

void JsonWriter::appendString(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  m_buf.push_back('"');
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      switch (c) {
        case '"': append("\\\""); break;
        case '\\': append("\\\\"); break;
        case '\n': append("\\n"); break;
        case '\r': append("\\r"); break;
        case '\t': append("\\t"); break;
        case '\b': append("\\b"); break;
        case '\f': append("\\f"); break;
        default:
          if (c < 0x20) {
            append("\\u00");
            m_buf.push_back(kHex[c >> 4]);
            m_buf.push_back(kHex[c & 15]);
          } else {
            m_buf.push_back(static_cast<char>(c));
          }
      }
      ++i;
      continue;
    }
    // Multi-byte sequence: accept only well-formed UTF-8 (no overlongs, no surrogates, <= U+10FFFF).
    size_t len = 0;
    uint32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07u;
    }
    bool okSeq = len != 0 && i + len <= n;
    for (size_t k = 1; okSeq && k < len; ++k) {
      if ((p[i + k] & 0xC0u) != 0x80u) okSeq = false;
      cp = (cp << 6) | (p[i + k] & 0x3Fu);
    }
    if (okSeq) {
      if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
          (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
        okSeq = false;
    }
    if (okSeq) {
      m_buf.append(reinterpret_cast<const char*>(p + i), len);
      i += len;
    } else {
      append("\xEF\xBF\xBD");  // U+FFFD
      ++i;
    }
  }
  m_buf.push_back('"');
}

void JsonWriter::value(std::string_view s) {
  beforeValue();
  appendString(s);
  maybeFlush();
}

void JsonWriter::value(uint64_t v) {
  beforeValue();
  char tmp[24];
  const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
  m_buf.append(tmp, static_cast<size_t>(r.ptr - tmp));
  maybeFlush();
}

void JsonWriter::value(int64_t v) {
  beforeValue();
  char tmp[24];
  const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
  m_buf.append(tmp, static_cast<size_t>(r.ptr - tmp));
  maybeFlush();
}

void JsonWriter::value(double v) {
  beforeValue();
  if (!std::isfinite(v)) {
    append("null");
    return;
  }
  char tmp[40];
  const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
  m_buf.append(tmp, static_cast<size_t>(r.ptr - tmp));
}

void JsonWriter::value(bool v) {
  beforeValue();
  append(v ? "true" : "false");
}

void JsonWriter::null() {
  beforeValue();
  append("null");
}

void JsonWriter::raw(std::string_view json) {
  beforeValue();
  append(json);
  maybeFlush();
}

void JsonWriter::microsFromNanos(int64_t nanos) {
  beforeValue();
  const uint64_t mag = nanos < 0 ? uint64_t{0} - static_cast<uint64_t>(nanos) : static_cast<uint64_t>(nanos);
  if (nanos < 0) m_buf.push_back('-');
  char tmp[24];
  const auto r = std::to_chars(tmp, tmp + sizeof tmp, mag / 1000);
  m_buf.append(tmp, static_cast<size_t>(r.ptr - tmp));
  const auto frac = static_cast<uint32_t>(mag % 1000);
  if (frac != 0) {
    const char digits[3] = {static_cast<char>('0' + frac / 100), static_cast<char>('0' + (frac / 10) % 10),
                            static_cast<char>('0' + frac % 10)};
    size_t count = 3;
    while (count > 0 && digits[count - 1] == '0') --count;
    m_buf.push_back('.');
    m_buf.append(digits, count);
  }
}

}  // namespace tachyon::exporters
