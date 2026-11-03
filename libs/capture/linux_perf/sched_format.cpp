#include "capture/linux_perf/sched_format.h"

#include <algorithm>
#include <cstring>

namespace tachyon::capture::perf {

namespace {

constexpr uint32_t kMaxPayload = 65536;

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool isIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool parseUnsigned(std::string_view s, uint64_t& out) {
  s = trim(s);
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    if (v > (UINT64_MAX - 9) / 10) return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  out = v;
  return true;
}

// "field:<decl>;\toffset:8;\tsize:16;\tsigned:0;" -> a field. Declarations look like "char prev_comm[16]",
// "pid_t prev_pid", "unsigned long ip", "__data_loc char[] name".
bool parseFieldLine(std::string_view line, TracepointField& out) {
  std::string_view decl;
  uint64_t offset = 0, size = 0, sgn = 0;
  bool haveOffset = false, haveSize = false, haveDecl = false;
  while (!line.empty()) {
    const size_t semi = line.find(';');
    std::string_view part = trim(line.substr(0, semi));
    line = semi == std::string_view::npos ? std::string_view{} : line.substr(semi + 1);
    if (part.empty()) continue;
    const size_t colon = part.find(':');
    if (colon == std::string_view::npos) return false;
    const std::string_view key = trim(part.substr(0, colon));
    const std::string_view value = part.substr(colon + 1);
    if (key == "field" || key == "field special") {
      decl = trim(value);
      haveDecl = !decl.empty();
    } else if (key == "offset") {
      if (!parseUnsigned(value, offset)) return false;
      haveOffset = true;
    } else if (key == "size") {
      if (!parseUnsigned(value, size)) return false;
      haveSize = true;
    } else if (key == "signed") {
      if (!parseUnsigned(value, sgn)) return false;
    }
  }
  if (!haveDecl || !haveOffset || !haveSize) return false;
  if (offset > kMaxPayload || size > kMaxPayload || offset + size > kMaxPayload) return false;

  // Name: the identifier that ends the declaration, or the one before a trailing "[N]" ("char
  // prev_comm[16]");
  // "__data_loc char[] name" has the array marker in the type.
  std::string_view head = decl;
  bool isArray = decl.find('[') != std::string_view::npos;
  std::string_view arrayPart;
  if (!head.empty() && head.back() == ']') {
    const size_t bracket = head.rfind('[');
    if (bracket == std::string_view::npos) return false;
    arrayPart = head.substr(bracket);
    head = trim(head.substr(0, bracket));
  }
  const size_t end = head.size();
  size_t begin = end;
  while (begin > 0 && isIdentChar(head[begin - 1])) --begin;
  if (begin == end) return false;
  out = TracepointField{};
  out.name = std::string(head.substr(begin, end - begin));
  out.type = std::string(trim(head.substr(0, begin))) + std::string(arrayPart);
  out.offset = static_cast<uint32_t>(offset);
  out.size = static_cast<uint32_t>(size);
  out.isSigned = sgn != 0;
  out.isArray = isArray;
  out.isDataLoc = decl.rfind("__data_loc", 0) == 0 || decl.rfind("__rel_loc", 0) == 0;
  return true;
}

template <typename F>
bool readInt(std::span<const uint8_t> raw, const F& f, uint64_t& out) {
  if (static_cast<uint64_t>(f.offset) + f.size > raw.size()) return false;
  uint64_t v = 0;
  std::memcpy(&v, raw.data() + f.offset, f.size);  // little-endian host and trace
  if (f.isSigned && f.size < 8) {
    const uint64_t sign = 1ull << (f.size * 8 - 1);
    if ((v & sign) != 0) v |= ~((sign << 1) - 1);
  }
  out = v;
  return true;
}

bool intWidth(uint32_t size) {
  return size == 1 || size == 2 || size == 4 || size == 8;
}

template <typename F>
void bind(const TracepointFormat& fmt, std::string_view name, F& out, bool required, std::string& missing) {
  const TracepointField* f = fmt.find(name);
  if (f == nullptr || f->isArray || f->isDataLoc || !intWidth(f->size)) {
    if (required) missing += (missing.empty() ? "" : ", ") + std::string(name);
    return;
  }
  out.offset = f->offset;
  out.size = f->size;
  out.isSigned = f->isSigned;
  out.present = true;
}

}  // namespace

const TracepointField* TracepointFormat::find(std::string_view fieldName) const {
  for (const TracepointField& f : fields)
    if (f.name == fieldName) return &f;
  return nullptr;
}

Result<TracepointFormat> parseTracepointFormat(std::string_view text) {
  TracepointFormat fmt;
  bool haveId = false, inFormat = false;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    const std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    const std::string_view t = trim(line);
    if (t.empty()) continue;
    if (t.rfind("name:", 0) == 0) {
      fmt.name = std::string(trim(t.substr(5)));
    } else if (t.rfind("ID:", 0) == 0) {
      uint64_t id = 0;
      if (!parseUnsigned(t.substr(3), id) || id > 0xFFFFFFFFull)
        return makeError(ErrorCode::CorruptData, "tracepoint format has a malformed ID line");
      fmt.id = static_cast<uint32_t>(id);
      haveId = true;
    } else if (t.rfind("format:", 0) == 0) {
      inFormat = true;
    } else if (t.rfind("print fmt:", 0) == 0) {
      break;
    } else if (inFormat && t.rfind("field", 0) == 0) {
      TracepointField f;
      if (!parseFieldLine(t, f))
        return makeError(ErrorCode::CorruptData,
                         "tracepoint format has a malformed field line: " + std::string(t));
      fmt.fields.push_back(std::move(f));
    }
  }
  if (fmt.name.empty()) return makeError(ErrorCode::CorruptData, "tracepoint format has no name line");
  if (!haveId) return makeError(ErrorCode::CorruptData, "tracepoint format has no ID line");
  if (fmt.fields.empty()) return makeError(ErrorCode::CorruptData, "tracepoint format has no fields");
  return fmt;
}

Result<SchedSwitchDecoder> SchedSwitchDecoder::create(const TracepointFormat& format) {
  SchedSwitchDecoder d;
  d.m_id = format.id;
  std::string missing;
  bind(format, "prev_pid", d.m_prevPid, true, missing);
  bind(format, "prev_state", d.m_prevState, true, missing);
  bind(format, "next_pid", d.m_nextPid, true, missing);
  bind(format, "next_prio", d.m_nextPrio, true, missing);
  bind(format, "prev_prio", d.m_prevPrio, false, missing);
  if (!missing.empty())
    return makeError(ErrorCode::InvalidArgument,
                     "sched_switch format lacks usable integer field(s): " + missing);
  for (const Field* f : {&d.m_prevPid, &d.m_prevPrio, &d.m_prevState, &d.m_nextPid, &d.m_nextPrio})
    if (f->present) d.m_minSize = std::max(d.m_minSize, f->offset + f->size);
  return d;
}

bool SchedSwitchDecoder::decode(std::span<const uint8_t> raw, SchedSwitchEvent& out) const {
  if (raw.size() < m_minSize) return false;
  uint64_t v = 0;
  out = SchedSwitchEvent{};
  if (!readInt(raw, m_prevPid, v)) return false;
  out.prevPid = static_cast<uint32_t>(v);
  if (m_prevPrio.present && readInt(raw, m_prevPrio, v)) out.prevPrio = static_cast<int32_t>(v);
  if (!readInt(raw, m_prevState, v)) return false;
  out.prevState = v;
  if (!readInt(raw, m_nextPid, v)) return false;
  out.nextPid = static_cast<uint32_t>(v);
  if (!readInt(raw, m_nextPrio, v)) return false;
  out.nextPrio = static_cast<int32_t>(v);
  return true;
}

Result<SchedWakeDecoder> SchedWakeDecoder::create(const TracepointFormat& format) {
  SchedWakeDecoder d;
  d.m_id = format.id;
  std::string missing;
  bind(format, "pid", d.m_pid, true, missing);
  bind(format, "prio", d.m_prio, false, missing);
  bind(format, "target_cpu", d.m_targetCpu, false, missing);
  bind(format, "success", d.m_success, false, missing);
  if (!missing.empty())
    return makeError(ErrorCode::InvalidArgument,
                     "wakeup tracepoint format lacks usable integer field(s): " + missing);
  for (const Field* f : {&d.m_pid, &d.m_prio, &d.m_targetCpu, &d.m_success})
    if (f->present) d.m_minSize = std::max(d.m_minSize, f->offset + f->size);
  return d;
}

bool SchedWakeDecoder::decode(std::span<const uint8_t> raw, SchedWakeEvent& out) const {
  if (raw.size() < m_minSize) return false;
  uint64_t v = 0;
  out = SchedWakeEvent{};
  if (!readInt(raw, m_pid, v)) return false;
  out.pid = static_cast<uint32_t>(v);
  if (m_prio.present && readInt(raw, m_prio, v)) out.prio = static_cast<int32_t>(v);
  if (m_targetCpu.present && readInt(raw, m_targetCpu, v)) out.targetCpu = static_cast<int32_t>(v);
  if (m_success.present && readInt(raw, m_success, v)) out.success = v != 0;
  return true;
}

uint32_t tracepointCommonType(std::span<const uint8_t> raw) {
  if (raw.size() < 2) return 0;
  uint16_t v = 0;
  std::memcpy(&v, raw.data(), 2);
  return v;
}

SwitchStateEncoding encodeSwitchState(uint64_t prevState) {
  SwitchStateEncoding e;
  const uint64_t low = prevState & 0xFF;
  e.oldWaitReason = static_cast<uint8_t>(low);
  if ((low & 0x30) != 0)
    e.oldState = 4;  // EXIT_DEAD / EXIT_ZOMBIE: the task is gone
  else if (low == 0)
    e.oldState = 1;  // still runnable: preempted (or yielded)
  else
    e.oldState = 5;  // sleeping: S, D, T, t, P, I
  return e;
}

}  // namespace tachyon::capture::perf
